#include "reco/TasteProfile.h"

#include "app/AppPaths.h"
#include "core/GenreTags.h"
#include "reco/TrackScorer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace {

bool reject(QString *error, const QString &message)
{
    *error = message;
    return false;
}

bool keys(const QJsonObject &object, const QSet<QString> &required,
          const QSet<QString> &optional, QString *error)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key())) {
            return reject(error, QStringLiteral("Unknown taste profile key: %1.").arg(it.key()));
        }
    }
    for (const QString &key : required) {
        if (!object.contains(key)) {
            return reject(error, QStringLiteral("Missing taste profile key: %1.").arg(key));
        }
    }
    return true;
}

bool document(const QByteArray &bytes, QJsonObject *object, QString *error)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(bytes);
    Q_UNUSED(text);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (decoder.hasError() || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return reject(error, QStringLiteral("Taste profile must be a complete UTF-8 JSON object."));
    }
    // Decode key tokens so escaped spellings cannot bypass duplicate detection.
    QList<QSet<QString>> stack;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes.at(i) == '{') {
            stack.append(QSet<QString>{});
        } else if (bytes.at(i) == '}') {
            stack.removeLast();
        } else if (bytes.at(i) == '"') {
            const qsizetype start = i++;
            while (i < bytes.size() && bytes.at(i) != '"') {
                if (bytes.at(i) == '\\') {
                    ++i;
                }
                ++i;
            }
            qsizetype next = i + 1;
            while (next < bytes.size() && QByteArray(" \t\r\n").contains(bytes.at(next))) {
                ++next;
            }
            if (next < bytes.size() && bytes.at(next) == ':' && !stack.isEmpty()) {
                const QString key = QJsonDocument::fromJson("[" + bytes.mid(start, i - start + 1) + "]")
                                        .array().first().toString();
                if (stack.last().contains(key)) {
                    return reject(error, QStringLiteral("Duplicate taste profile key: %1.").arg(key));
                }
                stack.last().insert(key);
            }
        }
    }
    *object = doc.object();
    return true;
}

bool genreLabel(const QString &label)
{
    if (label.isEmpty() || label.size() > 100 || label != GenreTags::folded(label)
        || GenreTags::isNonGenre(label)) {
        return false;
    }
    for (const QChar ch : label) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char(' ') && ch != QLatin1Char('-')
            && ch != QLatin1Char('&')) {
            return false;
        }
    }
    return true;
}

bool shares(const QJsonValue &value, bool genres, QMap<QString, double> *result, QString *error)
{
    if (!value.isObject() || value.toObject().size() > 256) {
        return reject(error, QStringLiteral("Taste shares must be an object with at most 256 buckets."));
    }
    double total = 0.0;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString label = it.key();
        bool valid = genreLabel(label);
        if (!genres) {
            bool ok = false;
            const int decade = label.toInt(&ok);
            valid = ok && decade >= 1000 && decade <= 2990 && decade % 10 == 0
                && label == QString::number(decade);
        }
        const double share = it.value().toDouble();
        if (!valid || !it.value().isDouble() || !std::isfinite(share) || share <= 0.0 || share > 1.0) {
            return reject(error, QStringLiteral("Invalid taste share bucket."));
        }
        total += share;
        result->insert(label, share);
    }
    if (!object.isEmpty() && std::abs(total - 1.0) > 0.000001) {
        return reject(error, QStringLiteral("Taste shares must sum to 1."));
    }
    return true;
}

QJsonObject objectFor(const TasteProfile &profile)
{
    QJsonObject genres;
    for (auto it = profile.genres.begin(); it != profile.genres.end(); ++it) {
        genres.insert(it.key(), it.value());
    }
    QJsonObject features;
    for (auto it = profile.features.begin(); it != profile.features.end(); ++it) {
        features.insert(it.key(), QJsonObject{{QStringLiteral("q25"), it->q25},
                                             {QStringLiteral("q50"), it->q50},
                                             {QStringLiteral("q75"), it->q75}});
    }
    QJsonObject object{{QStringLiteral("format"), QStringLiteral("muzaiten-taste-profile")},
                       {QStringLiteral("version"), 1}, {QStringLiteral("genres"), genres},
                       {QStringLiteral("features"), features}};
    if (!profile.decades.isEmpty()) {
        QJsonObject decades;
        for (auto it = profile.decades.begin(); it != profile.decades.end(); ++it) {
            decades.insert(it.key(), it.value());
        }
        object.insert(QStringLiteral("decades"), decades);
    }
    return object;
}

bool readBytes(const QString &path, qsizetype maximum, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        return reject(error, QStringLiteral("Could not open local taste profile file."));
    }
    if (file.size() > maximum) {
        return reject(error, QStringLiteral("Taste profile file exceeds its size limit."));
    }
    *bytes = file.read(maximum + 1);
    return file.error() == QFileDevice::NoError && bytes->size() <= maximum
        ? true : reject(error, QStringLiteral("Could not read bounded taste profile file."));
}

bool atomicWrite(const QString &path, const QByteArray &bytes, bool replace, QString *error)
{
    if (!replace) {
        QTemporaryFile temporary(path + QStringLiteral(".XXXXXX"));
        if (!temporary.open() || temporary.write(bytes) != bytes.size() || !temporary.flush()
            || ::fsync(temporary.handle()) != 0) {
            return reject(error, QStringLiteral("Could not prepare atomic taste profile write."));
        }
        // Linking the completed sibling file publishes atomically without
        // replacing a destination that appeared after the preview.
        if (::link(QFile::encodeName(temporary.fileName()).constData(), QFile::encodeName(path).constData()) != 0) {
            return reject(error, QStringLiteral("Could not write taste profile. Existing files require explicit replace."));
        }
        return true;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        return reject(error, QStringLiteral("Could not atomically write taste profile."));
    }
    return true;
}

// Retained buckets as shares in whole hundredths. Largest-remainder rounding
// keeps each set summing to exactly 1.00; a bucket that rounds to nothing is
// dropped. Ties go to the earlier label so the result is deterministic.
QMap<QString, double> retainedShares(const QMap<QString, int> &support)
{
    qint64 total = 0;
    for (const int count : support) {
        if (count >= TasteProfiles::MinimumSupport) {
            total += count;
        }
    }
    struct Bucket {
        QString label;
        qint64 hundredths;
        qint64 remainder;
    };
    QList<Bucket> buckets;
    qint64 assigned = 0;
    for (auto it = support.begin(); it != support.end(); ++it) {
        if (it.value() >= TasteProfiles::MinimumSupport) {
            const qint64 scaled = qint64{it.value()} * 100;
            buckets.append({it.key(), scaled / total, scaled % total});
            assigned += scaled / total;
        }
    }
    std::stable_sort(buckets.begin(), buckets.end(),
                     [](const Bucket &left, const Bucket &right) { return left.remainder > right.remainder; });
    for (qsizetype i = 0; assigned < 100 && i < buckets.size(); ++i, ++assigned) {
        ++buckets[i].hundredths;
    }
    QMap<QString, double> shares;
    for (const Bucket &bucket : buckets) {
        if (bucket.hundredths > 0) {
            shares.insert(bucket.label, static_cast<double>(bucket.hundredths) / 100.0);
        }
    }
    return shares;
}

bool saveSeeds(const QMap<QString, TasteProfile> &profiles, QString *error)
{
    QJsonArray entries;
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        entries.append(QJsonObject{{QStringLiteral("name"), it.key()},
                                   {QStringLiteral("profile"), objectFor(it.value())}});
    }
    if (profiles.size() > 32) {
        return reject(error, QStringLiteral("At most 32 local taste seeds can be stored."));
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("format"), QStringLiteral("muzaiten-taste-seeds")},
        {QStringLiteral("version"), 1}, {QStringLiteral("profiles"), entries}}).toJson();
    return atomicWrite(TasteProfiles::storagePath(), bytes, true, error);
}

} // namespace

namespace TasteProfiles {

QString contentsDescription()
{
    return QStringLiteral("Contains canonical genre shares, tempo and energy quartiles, and optional decade shares. "
                          "Buckets supported by fewer than five distinct tracks are omitted. "
                          "Shares and energy quartiles are rounded to hundredths and tempo quartiles to whole BPM. "
                          "These aggregates reveal library taste. No artist, album or track names, recording identifiers, paths, timestamps, "
                          "raw counts, listening history, skips or ratings are included.");
}

bool validName(const QString &name)
{
    if (name.isEmpty() || name != name.trimmed() || name.size() > 100) {
        return false;
    }
    for (const QChar ch : name) {
        if (!ch.isPrint() || ch == QLatin1Char('/') || ch == QLatin1Char('\\')) {
            return false;
        }
    }
    return true;
}

bool parse(const QByteArray &bytes, TasteProfile *profile, QString *error)
{
    error->clear();
    if (bytes.size() > MaximumBytes) {
        return reject(error, QStringLiteral("Taste profile file exceeds 64 KiB."));
    }
    QJsonObject object;
    if (!document(bytes, &object, error)
        || !keys(object, {QStringLiteral("format"), QStringLiteral("version"),
                          QStringLiteral("genres"), QStringLiteral("features")},
                         {QStringLiteral("decades")}, error)) {
        return false;
    }
    if (object.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("muzaiten-taste-profile"))
        || !object.value(QStringLiteral("version")).isDouble()
        || object.value(QStringLiteral("version")).toDouble() != 1.0) {
        return reject(error, QStringLiteral("Unsupported taste profile format or version; expected v1."));
    }
    TasteProfile parsed;
    if (!shares(object.value(QStringLiteral("genres")), true, &parsed.genres, error)
        || (object.contains(QStringLiteral("decades"))
            && !shares(object.value(QStringLiteral("decades")), false, &parsed.decades, error))) {
        return false;
    }
    const QJsonValue features = object.value(QStringLiteral("features"));
    if (!features.isObject()
        || !keys(features.toObject(), {}, {QStringLiteral("tempo_bpm"), QStringLiteral("energy")}, error)) {
        return reject(error, QStringLiteral("Taste features must contain only tempo_bpm and energy quartiles."));
    }
    const QJsonObject featureObject = features.toObject();
    for (auto it = featureObject.begin(); it != featureObject.end(); ++it) {
        if (!it.value().isObject()
            || !keys(it.value().toObject(), {QStringLiteral("q25"), QStringLiteral("q50"),
                                            QStringLiteral("q75")}, {}, error)) {
            return reject(error, QStringLiteral("Taste quartiles require exactly q25, q50 and q75."));
        }
        QVector<double> values;
        for (const QString &key : {QStringLiteral("q25"), QStringLiteral("q50"), QStringLiteral("q75")}) {
            const QJsonValue entry = it.value().toObject().value(key);
            const double value = entry.toDouble();
            const bool tempo = it.key() == QLatin1String("tempo_bpm");
            if (!entry.isDouble() || !std::isfinite(value) || value < 0.0
                || value > (tempo ? 1000.0 : 1.0) || (tempo && value == 0.0)) {
                return reject(error, QStringLiteral("Invalid taste feature quartile."));
            }
            values.append(value);
        }
        if (values[0] > values[1] || values[1] > values[2]) {
            return reject(error, QStringLiteral("Taste feature quartiles must be ordered."));
        }
        parsed.features.insert(it.key(), {values[0], values[1], values[2]});
    }
    if (parsed.genres.isEmpty() && parsed.features.isEmpty()) {
        return reject(error, QStringLiteral("Taste seed needs a supported genre or feature distribution."));
    }
    *profile = std::move(parsed);
    return true;
}

bool encode(const TasteProfile &profile, QByteArray *bytes, QString *error)
{
    for (auto it = profile.features.begin(); it != profile.features.end(); ++it) {
        if (!std::isfinite(it->q25) || !std::isfinite(it->q50) || !std::isfinite(it->q75)) {
            return reject(error, QStringLiteral("Invalid taste feature quartile."));
        }
    }
    const QByteArray encoded = QJsonDocument(objectFor(profile)).toJson();
    TasteProfile validated;
    if (!parse(encoded, &validated, error)) {
        return false;
    }
    *bytes = encoded;
    return true;
}

bool read(const QString &path, TasteProfile *profile, QString *error)
{
    QByteArray bytes;
    return readBytes(path, MaximumBytes, &bytes, error) && parse(bytes, profile, error);
}

bool write(const QString &path, const QByteArray &preview, bool replace, QString *error)
{
    TasteProfile validated;
    return parse(preview, &validated, error) && atomicWrite(path, preview, replace, error);
}

bool aggregate(const QVector<TrackScorer::Candidate> &tracks, bool optIn, bool includeDecades,
               TasteProfile *profile, QString *error)
{
    if (!optIn) {
        return reject(error, QStringLiteral("Taste export requires explicit opt-in on every invocation: --opt-in."));
    }
    QMap<QString, int> genreSupport;
    QMap<QString, int> decadeSupport;
    QMap<QString, QVector<double>> samples;
    QSet<QString> identities;
    QSet<qint64> contentGroups;
    for (const auto &track : tracks) {
        const QString identity = track.songKey.isEmpty() ? track.path : track.songKey;
        if (identity.isEmpty() || identities.contains(identity)
            || (track.contentGroupId >= 0 && contentGroups.contains(track.contentGroupId))) {
            continue;
        }
        identities.insert(identity);
        if (track.contentGroupId >= 0) {
            contentGroups.insert(track.contentGroupId);
        }
        const QSet<QString> genres(track.genresFolded.begin(), track.genresFolded.end());
        for (const QString &genre : genres) {
            if (genreLabel(genre)) {
                ++genreSupport[genre];
            }
        }
        if (std::isfinite(track.tempoBpm) && track.tempoBpm > 0.0 && track.tempoBpm <= 1000.0) {
            samples[QStringLiteral("tempo_bpm")].append(track.tempoBpm);
        }
        if (std::isfinite(track.energy) && track.energy >= 0.0 && track.energy <= 1.0) {
            samples[QStringLiteral("energy")].append(track.energy);
        }
        if (includeDecades && track.year >= 1000 && track.year < 3000) {
            ++decadeSupport[QString::number(track.year / 10 * 10)];
        }
    }
    TasteProfile aggregated;
    aggregated.genres = retainedShares(genreSupport);
    aggregated.decades = retainedShares(decadeSupport);
    for (auto it = samples.begin(); it != samples.end(); ++it) {
        if (it->size() < MinimumSupport) {
            continue;
        }
        std::sort(it->begin(), it->end());
        // An interpolated quartile can equal one track's exact extractor value,
        // so tempo rounds to whole BPM and energy to hundredths.
        const bool tempo = it.key() == QLatin1String("tempo_bpm");
        const auto quantile = [&values = it.value(), tempo](double fraction) {
            const double index = fraction * static_cast<double>(values.size() - 1);
            const auto lower = static_cast<qsizetype>(std::floor(index));
            const auto upper = static_cast<qsizetype>(std::ceil(index));
            const double value = values[lower] + (values[upper] - values[lower]) * (index - static_cast<double>(lower));
            return tempo ? std::clamp(std::round(value), 1.0, 1000.0) : std::round(value * 100.0) / 100.0;
        };
        aggregated.features.insert(it.key(), {quantile(0.25), quantile(0.5), quantile(0.75)});
    }
    QByteArray validated;
    if (!encode(aggregated, &validated, error)) {
        return false;
    }
    *profile = std::move(aggregated);
    return true;
}

double similarity(const TasteProfile &profile, const TrackScorer::Candidate &candidate)
{
    double genre = 0.0;
    for (const QString &label : candidate.genresFolded) {
        genre = std::max(genre, profile.genres.value(label));
    }
    double features = 0.0;
    for (auto it = profile.features.begin(); it != profile.features.end(); ++it) {
        const bool tempo = it.key() == QLatin1String("tempo_bpm");
        const double value = tempo ? candidate.tempoBpm : candidate.energy;
        if (!std::isfinite(value) || (tempo ? value <= 0.0 || value > 1000.0 : value < 0.0 || value > 1.0)) {
            continue;
        }
        const double scale = std::max(it->q75 - it->q25, tempo ? 20.0 : 0.1);
        features += std::max(0.0, 1.0 - std::abs(value - it->q50) / scale);
    }
    if (!profile.features.isEmpty()) {
        features /= static_cast<double>(profile.features.size());
    }
    return std::clamp(2.0 * genre + features, 0.0, MaximumScore);
}

QString storagePath()
{
    return QDir(AppPaths::configDir()).filePath(QStringLiteral("radio-taste-seeds.json"));
}

bool load(QMap<QString, TasteProfile> *profiles, QString *error)
{
    error->clear();
    QMap<QString, TasteProfile> loaded;
    const QString path = storagePath();
    if (!QFileInfo::exists(path)) {
        profiles->clear();
        return true;
    }
    QByteArray bytes;
    QJsonObject object;
    if (!readBytes(path, 2 * 1024 * 1024, &bytes, error) || !document(bytes, &object, error)
        || !keys(object, {QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("profiles")}, {}, error)) {
        return false;
    }
    if (object.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("muzaiten-taste-seeds"))
        || object.value(QStringLiteral("version")) != QJsonValue(1)
        || !object.value(QStringLiteral("profiles")).isArray()
        || object.value(QStringLiteral("profiles")).toArray().size() > 32) {
        return reject(error, QStringLiteral("Invalid local taste seed store."));
    }
    for (const QJsonValue &value : object.value(QStringLiteral("profiles")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        TasteProfile profile;
        if (!value.isObject() || !keys(entry, {QStringLiteral("name"), QStringLiteral("profile")}, {}, error)
            || !validName(name) || loaded.contains(name) || !entry.value(QStringLiteral("profile")).isObject()
            || !parse(QJsonDocument(entry.value(QStringLiteral("profile")).toObject()).toJson(), &profile, error)) {
            return reject(error, QStringLiteral("Invalid or duplicate local taste seed."));
        }
        loaded.insert(name, profile);
    }
    *profiles = std::move(loaded);
    return true;
}

bool importProfile(const QString &name, const TasteProfile &profile, bool replace, bool dryRun, QString *error)
{
    QByteArray bytes;
    if (!validName(name)) {
        return reject(error, QStringLiteral("Taste seed name must be 1 to 100 printable characters without slashes or surrounding whitespace."));
    }
    if (!encode(profile, &bytes, error)) {
        return false;
    }
    QLockFile lock(storagePath() + QStringLiteral(".lock"));
    if (!dryRun && !lock.tryLock()) {
        return reject(error, QStringLiteral("Taste seed store is busy."));
    }
    QMap<QString, TasteProfile> profiles;
    if (!load(&profiles, error)) {
        return false;
    }
    if (dryRun) {
        return true;
    }
    if (profiles.contains(name) && !replace) {
        return reject(error, QStringLiteral("Taste seed already exists. Explicit --replace is required."));
    }
    profiles.insert(name, profile);
    return saveSeeds(profiles, error);
}

bool remove(const QString &name, QString *error)
{
    QLockFile lock(storagePath() + QStringLiteral(".lock"));
    if (!lock.tryLock()) {
        return reject(error, QStringLiteral("Taste seed store is busy."));
    }
    QMap<QString, TasteProfile> profiles;
    if (!load(&profiles, error)) {
        return false;
    }
    if (!profiles.remove(name)) {
        return reject(error, QStringLiteral("Taste seed not found."));
    }
    return saveSeeds(profiles, error);
}

} // namespace TasteProfiles
