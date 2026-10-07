#include "reco/RadioProfileExchange.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>

#include <cmath>

namespace {

bool reject(QString *error, const QString &message)
{
    *error = message;
    return false;
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &keys, QString *error)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key())) {
            return reject(error, QStringLiteral("Unknown profile key: %1.").arg(it.key()));
        }
    }
    for (const QString &key : keys) {
        if (!object.contains(key)) {
            return reject(error, QStringLiteral("Missing profile key: %1.").arg(key));
        }
    }
    return true;
}

// QJsonDocument accepts duplicate keys; exchange objects require unique keys.
bool uniqueKeys(const QByteArray &bytes, QString *error)
{
    QList<QSet<QString>> objects;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char ch = bytes.at(i);
        if (ch == '{') {
            objects.append(QSet<QString>{});
        } else if (ch == '}') {
            objects.removeLast();
        } else if (ch == '"') {
            const qsizetype start = i++;
            while (i < bytes.size() && bytes.at(i) != '"') {
                if (bytes.at(i) == '\\') {
                    ++i;
                }
                ++i;
            }
            qsizetype next = i + 1;
            while (next < bytes.size() && (bytes.at(next) == ' ' || bytes.at(next) == '\t'
                                         || bytes.at(next) == '\r' || bytes.at(next) == '\n')) {
                ++next;
            }
            if (next < bytes.size() && bytes.at(next) == ':' && !objects.isEmpty()) {
                const QString key = QJsonDocument::fromJson("[" + bytes.mid(start, i - start + 1) + "]")
                                        .array().first().toString();
                if (objects.last().contains(key)) {
                    return reject(error, QStringLiteral("Duplicate profile key: %1.").arg(key));
                }
                objects.last().insert(key);
            }
        }
    }
    return true;
}

bool number(const QJsonObject &object, const QString &key, double minimum, double maximum,
            double *value, QString *error)
{
    const QJsonValue entry = object.value(key);
    const double parsed = entry.toDouble();
    if (!entry.isDouble() || !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return reject(error, QStringLiteral("Invalid profile value: %1.").arg(key));
    }
    *value = parsed;
    return true;
}

QJsonObject objectForProfile(const RadioProfile &profile)
{
    return {
        {QStringLiteral("format"), QStringLiteral("muzaiten-radio-profile")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("origin"), RadioProfileExchange::originName(profile.origin)},
        {QStringLiteral("weights"), QJsonDocument::fromJson(TrackScorer::weightsToJson(profile.weights)).object()},
        {QStringLiteral("sessionDecay"), QJsonObject{
             {QStringLiteral("decayStartTrack"), profile.sessionDecay.decayStartTrack},
             {QStringLiteral("decayCurve"), profile.sessionDecay.decayCurve},
             {QStringLiteral("noveltyDecayFloor"), profile.sessionDecay.noveltyDecayFloor},
             {QStringLiteral("ratingDecayFloor"), profile.sessionDecay.ratingDecayFloor}}},
    };
}

} // namespace

namespace RadioProfileExchange {

QString originName(RadioProfile::Origin origin)
{
    switch (origin) {
    case RadioProfile::Origin::Manual: return QStringLiteral("manual");
    case RadioProfile::Origin::Learned: return QStringLiteral("learned");
    case RadioProfile::Origin::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString contentsDescription()
{
    return QStringLiteral("Contains the profile name, origin, scoring weights and session decay. "
                          "No paths, listening history, track or artist identifiers, timestamps or telemetry rows.");
}

bool parse(const QByteArray &bytes, RadioProfile *profile, QString *error)
{
    error->clear();
    if (bytes.size() > MaximumBytes) {
        return reject(error, QStringLiteral("Radio profile file exceeds 64 KiB."));
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(bytes);
    Q_UNUSED(text);
    if (decoder.hasError()) {
        return reject(error, QStringLiteral("Radio profile file must be valid UTF-8."));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(error, QStringLiteral("Radio profile file must be a complete JSON object."));
    }
    if (!uniqueKeys(bytes, error)) {
        return false;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("name"),
                           QStringLiteral("origin"), QStringLiteral("weights"), QStringLiteral("sessionDecay")}, error)) {
        return false;
    }
    if (object.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("muzaiten-radio-profile"))) {
        return reject(error, QStringLiteral("Unknown radio profile format."));
    }
    const QJsonValue version = object.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        return reject(error, QStringLiteral("Unsupported radio profile version; expected 1."));
    }
    RadioProfile parsed;
    const QJsonValue name = object.value(QStringLiteral("name"));
    parsed.name = name.toString();
    if (!name.isString() || parsed.name.isEmpty() || parsed.name != parsed.name.trimmed()
        || parsed.name.size() > MaximumNameLength) {
        return reject(error, QStringLiteral("Profile name must be 1 to 200 characters without surrounding whitespace."));
    }
    for (qsizetype i = 0; i < parsed.name.size(); ++i) {
        const QChar ch = parsed.name.at(i);
        if (ch.isHighSurrogate() && i + 1 < parsed.name.size() && parsed.name.at(i + 1).isLowSurrogate()) {
            ++i;
        } else if (ch.category() == QChar::Other_Control || ch.isSurrogate()) {
            return reject(error, QStringLiteral("Profile name contains an invalid character."));
        }
    }
    const QString origin = object.value(QStringLiteral("origin")).toString();
    if (origin == QLatin1String("manual")) {
        parsed.origin = RadioProfile::Origin::Manual;
    } else if (origin == QLatin1String("learned")) {
        parsed.origin = RadioProfile::Origin::Learned;
    } else {
        return reject(error, QStringLiteral("Profile origin must be manual or learned."));
    }
    const QJsonValue weights = object.value(QStringLiteral("weights"));
    QSet<QString> weightKeys;
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        weightKeys.insert(spec.key);
    }
    if (!weights.isObject() || !exactKeys(weights.toObject(), weightKeys, error)) {
        if (error->isEmpty()) {
            *error = QStringLiteral("Profile weights must be an object.");
        }
        return false;
    }
    parsed.weights = TrackScorer::weightsFromJson(QJsonDocument(weights.toObject()).toJson(), error);
    if (!error->isEmpty()) {
        return false;
    }
    const QJsonValue decayValue = object.value(QStringLiteral("sessionDecay"));
    const QJsonObject decay = decayValue.toObject();
    if (!decayValue.isObject() || !exactKeys(decay, {QStringLiteral("decayStartTrack"), QStringLiteral("decayCurve"),
                           QStringLiteral("noveltyDecayFloor"), QStringLiteral("ratingDecayFloor")}, error)) {
        if (error->isEmpty()) {
            *error = QStringLiteral("Profile session decay must be an object.");
        }
        return false;
    }
    double start = 0.0;
    if (!number(decay, QStringLiteral("decayStartTrack"), 1.0, 1000.0, &start, error)
        || std::floor(start) != start) {
        return reject(error, QStringLiteral("decayStartTrack must be an integer from 1 to 1000."));
    }
    parsed.sessionDecay.decayStartTrack = static_cast<int>(start);
    if (!number(decay, QStringLiteral("decayCurve"), 0.0, 1.0, &parsed.sessionDecay.decayCurve, error)
        || !number(decay, QStringLiteral("noveltyDecayFloor"), 0.0, 1.0, &parsed.sessionDecay.noveltyDecayFloor, error)
        || !number(decay, QStringLiteral("ratingDecayFloor"), 0.0, 1.0, &parsed.sessionDecay.ratingDecayFloor, error)) {
        return false;
    }
    *profile = std::move(parsed);
    return true;
}

bool encode(const RadioProfile &profile, bool allowLearned, QByteArray *bytes, QString *error)
{
    error->clear();
    if (profile.origin != RadioProfile::Origin::Manual && !allowLearned) {
        return reject(error, QStringLiteral("Learned or unknown-origin profiles can reveal listening taste. "
                                           "Export requires explicit consent (CLI: --allow-learned)."));
    }
    RadioProfile exported = profile;
    if (exported.origin == RadioProfile::Origin::Unknown) {
        exported.origin = RadioProfile::Origin::Learned;
    }
    // Validate before JSON serialization can turn non-finite numbers into null.
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        double value = 0.0;
        TrackScorer::weightValue(exported.weights, spec.key, &value);
        if (!std::isfinite(value)) {
            return reject(error, QStringLiteral("Invalid profile value: %1.").arg(spec.key));
        }
    }
    const QByteArray encoded = QJsonDocument(objectForProfile(exported)).toJson(QJsonDocument::Indented);
    RadioProfile validated;
    if (!parse(encoded, &validated, error)) {
        return false;
    }
    *bytes = encoded;
    return true;
}

bool read(const QString &path, RadioProfile *profile, QString *error)
{
    error->clear();
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        return reject(error, QStringLiteral("Could not open radio profile file."));
    }
    if (file.size() > MaximumBytes) {
        return reject(error, QStringLiteral("Radio profile file exceeds 64 KiB."));
    }
    const QByteArray bytes = file.read(MaximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return reject(error, QStringLiteral("Could not read radio profile file."));
    }
    return parse(bytes, profile, error);
}

bool write(const QString &path, const RadioProfile &profile, bool allowLearned, bool replace, QString *error)
{
    QByteArray bytes;
    if (!encode(profile, allowLearned, &bytes, error)) {
        return false;
    }
    const QFileInfo target(path);
    if (!replace && (target.exists() || target.isSymLink())) {
        return reject(error, QStringLiteral("Export file already exists. Explicit replace is required."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return reject(error, QStringLiteral("Could not atomically write radio profile file."));
    }
    return true;
}

} // namespace RadioProfileExchange
