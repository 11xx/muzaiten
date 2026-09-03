#include "reco/WeightLearnerData.h"

#include "reco/TrackScorer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

QVector<TrackScorer::Component> componentsFromJson(const QByteArray &json)
{
    QVector<TrackScorer::Component> components;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isArray()) {
        return components;
    }
    const QJsonArray array = document.array();
    components.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QJsonValue componentValue = object.value(QStringLiteral("value"));
        if (name.isEmpty() || !componentValue.isDouble()) {
            continue;
        }
        components.push_back({name, componentValue.toDouble()});
    }
    return components;
}

} // namespace

namespace WeightLearnerData {

bool isEarlySkip(const QString &outcome, qint64 playedMs, qint64 durationMs)
{
    if (outcome != QLatin1String("skipped")) {
        return false;
    }
    const qint64 threshold = durationMs > 0 ? std::min(durationMs / 2, qint64(4 * 60 * 1000))
                                           : qint64(4 * 60 * 1000);
    return playedMs < threshold;
}

namespace {

// The play join: a pick's first radio play event inside the window.
constexpr const char *kPlayJoinSql =
    "SELECT rp.weights_json, rp.components_json, pe.outcome, pe.played_ms, pe.duration_ms "
    "FROM radio_picks rp "
    "JOIN play_events pe ON pe.id = ("
    " SELECT pe2.id FROM play_events pe2 "
    " WHERE pe2.source = 'radio' "
    "   AND pe2.track_path = rp.track_path "
    "   AND pe2.started_at >= rp.occurred_at "
    "   AND pe2.started_at <= rp.occurred_at + ? "
    " ORDER BY pe2.started_at ASC, pe2.id ASC "
    " LIMIT 1"
    ") "
    "ORDER BY rp.occurred_at ASC, rp.id ASC";

// The removal join: a pick removed from the queue unheard inside the window
// and matched by no radio play event there, so each pick yields at most one
// sample and a play always wins over a removal.
constexpr const char *kRemovalJoinSql =
    "SELECT rp.weights_json, rp.components_json "
    "FROM radio_picks rp "
    "WHERE EXISTS ("
    " SELECT 1 FROM queue_removals qr "
    " WHERE qr.was_radio_pick = 1 "
    "   AND qr.was_unheard = 1 "
    "   AND qr.track_path = rp.track_path "
    "   AND qr.occurred_at >= rp.occurred_at "
    "   AND qr.occurred_at <= rp.occurred_at + ?"
    ") AND NOT EXISTS ("
    " SELECT 1 FROM play_events pe "
    " WHERE pe.source = 'radio' "
    "   AND pe.track_path = rp.track_path "
    "   AND pe.started_at >= rp.occurred_at "
    "   AND pe.started_at <= rp.occurred_at + ?"
    ") "
    "ORDER BY rp.occurred_at ASC, rp.id ASC";

// Turns a pick's recorded weights and components into the learner's feature
// vector: each component's contribution divided by the weight that produced
// it. Returns false, counting the reason in `result`, when the row cannot
// feed the fit.
bool appendSample(const QString &weightsJson,
                  const QString &componentsJson,
                  bool earlySkip,
                  double weight,
                  LoadResult &result)
{
    QString parseError;
    const TrackScorer::Weights rowWeights = TrackScorer::weightsFromJson(weightsJson.toUtf8(), &parseError);
    if (!parseError.isEmpty()) {
        ++result.skippedInvalidWeights;
        return false;
    }

    WeightLearner::Sample sample;
    sample.earlySkip = earlySkip;
    sample.weight = weight;
    const QVector<TrackScorer::Component> components = componentsFromJson(componentsJson.toUtf8());
    for (const TrackScorer::Component &component : components) {
        double componentWeight = 0.0;
        if (!WeightLearner::componentWeight(rowWeights, component.name, &componentWeight)
            || componentWeight == 0.0) {
            continue;
        }
        const double signal = component.value / componentWeight;
        if (std::isfinite(signal)) {
            sample.features.insert(component.name, signal);
        }
    }
    if (sample.features.isEmpty()) {
        ++result.skippedNoSignals;
        return false;
    }
    result.samples.push_back(std::move(sample));
    return true;
}

} // namespace

LoadResult loadSamples(const QSqlDatabase &history)
{
    LoadResult result;
    if (!history.isOpen()) {
        result.error = QStringLiteral("history database is not open");
        return result;
    }

    QSqlQuery plays(history);
    plays.prepare(QString::fromLatin1(kPlayJoinSql));
    plays.addBindValue(kJoinWindowSecs);
    if (!plays.exec()) {
        result.error = plays.lastError().text();
        return result;
    }
    while (plays.next()) {
        const bool earlySkip = isEarlySkip(plays.value(2).toString(),
                                           plays.value(3).toLongLong(),
                                           plays.value(4).toLongLong());
        if (appendSample(plays.value(0).toString(), plays.value(1).toString(), earlySkip, 1.0, result)) {
            ++result.playSamples;
        }
    }

    QSqlQuery removals(history);
    removals.prepare(QString::fromLatin1(kRemovalJoinSql));
    removals.addBindValue(kJoinWindowSecs);
    removals.addBindValue(kJoinWindowSecs);
    if (!removals.exec()) {
        result.error = removals.lastError().text();
        return result;
    }
    while (removals.next()) {
        if (appendSample(removals.value(0).toString(), removals.value(1).toString(), true, kRemovalWeight, result)) {
            ++result.removalSamples;
        }
    }
    return result;
}

LoadResult loadSamplesFromPath(const QString &path)
{
    LoadResult result;
    const QString connectionName =
        QStringLiteral("muzaiten-radio-learn-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase history = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    history.setDatabaseName(path);
    if (!history.open()) {
        result.error = history.lastError().text();
        history = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return result;
    }

    result = loadSamples(history);
    history.close();
    history = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return result;
}

} // namespace WeightLearnerData
