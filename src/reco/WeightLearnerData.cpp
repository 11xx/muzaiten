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

LoadResult loadSamples(const QSqlDatabase &history)
{
    LoadResult result;
    if (!history.isOpen()) {
        result.error = QStringLiteral("history database is not open");
        return result;
    }

    QSqlQuery query(history);
    query.prepare(QStringLiteral(
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
        "ORDER BY rp.occurred_at ASC, rp.id ASC"));
    query.addBindValue(kJoinWindowSecs);
    if (!query.exec()) {
        result.error = query.lastError().text();
        return result;
    }

    while (query.next()) {
        QString parseError;
        const TrackScorer::Weights rowWeights =
            TrackScorer::weightsFromJson(query.value(0).toString().toUtf8(), &parseError);
        if (!parseError.isEmpty()) {
            ++result.skippedInvalidWeights;
            continue;
        }

        WeightLearner::Sample sample;
        sample.earlySkip = isEarlySkip(query.value(2).toString(),
                                       query.value(3).toLongLong(),
                                       query.value(4).toLongLong());
        const QVector<TrackScorer::Component> components =
            componentsFromJson(query.value(1).toString().toUtf8());
        for (const TrackScorer::Component &component : components) {
            double weight = 0.0;
            if (!WeightLearner::componentWeight(rowWeights, component.name, &weight) || weight == 0.0) {
                continue;
            }
            const double signal = component.value / weight;
            if (std::isfinite(signal)) {
                sample.features.insert(component.name, signal);
            }
        }
        if (sample.features.isEmpty()) {
            ++result.skippedNoSignals;
            continue;
        }
        result.samples.push_back(std::move(sample));
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
