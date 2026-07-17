#include "reco/RadioProfile.h"

#include "app/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QJsonObject decayToJson(const TrackScorer::RadioSessionDecay &decay)
{
    return {
        {QStringLiteral("decayStartTrack"), decay.decayStartTrack},
        {QStringLiteral("decayCurve"), decay.decayCurve},
        {QStringLiteral("noveltyDecayFloor"), decay.noveltyDecayFloor},
        {QStringLiteral("ratingDecayFloor"), decay.ratingDecayFloor},
    };
}

TrackScorer::RadioSessionDecay decayFromJson(const QJsonObject &object)
{
    TrackScorer::RadioSessionDecay decay = TrackScorer::defaultSessionDecay();
    if (object.contains(QStringLiteral("decayStartTrack"))) {
        decay.decayStartTrack = object.value(QStringLiteral("decayStartTrack")).toInt(decay.decayStartTrack);
    }
    if (object.contains(QStringLiteral("decayCurve"))) {
        decay.decayCurve = object.value(QStringLiteral("decayCurve")).toDouble(decay.decayCurve);
    }
    if (object.contains(QStringLiteral("noveltyDecayFloor"))) {
        decay.noveltyDecayFloor = object.value(QStringLiteral("noveltyDecayFloor")).toDouble(decay.noveltyDecayFloor);
    }
    if (object.contains(QStringLiteral("ratingDecayFloor"))) {
        decay.ratingDecayFloor = object.value(QStringLiteral("ratingDecayFloor")).toDouble(decay.ratingDecayFloor);
    }
    return decay;
}

QJsonObject profileToJson(const RadioProfile &profile)
{
    const QJsonDocument weights = QJsonDocument::fromJson(TrackScorer::weightsToJson(profile.weights));
    return {
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("createdAtUtc"), profile.createdAtUtc},
        {QStringLiteral("modifiedAtUtc"), profile.modifiedAtUtc},
        {QStringLiteral("weights"), weights.object()},
        {QStringLiteral("sessionDecay"), decayToJson(profile.sessionDecay)},
    };
}

bool profileFromJson(const QJsonValue &value, RadioProfile *profile, QString *error)
{
    if (!value.isObject()) {
        *error = QStringLiteral("profile is not an object");
        return false;
    }
    const QJsonObject object = value.toObject();
    RadioProfile parsed;
    parsed.name = object.value(QStringLiteral("name")).toString().trimmed();
    if (parsed.name.isEmpty()) {
        *error = QStringLiteral("profile name is empty");
        return false;
    }
    parsed.createdAtUtc = object.value(QStringLiteral("createdAtUtc")).toString();
    parsed.modifiedAtUtc = object.value(QStringLiteral("modifiedAtUtc")).toString();
    if (parsed.createdAtUtc.isEmpty()) {
        parsed.createdAtUtc = nowUtc();
    }
    if (parsed.modifiedAtUtc.isEmpty()) {
        parsed.modifiedAtUtc = parsed.createdAtUtc;
    }
    const QJsonValue weights = object.value(QStringLiteral("weights"));
    if (!weights.isObject()) {
        *error = QStringLiteral("profile weights are not an object");
        return false;
    }
    QString weightsError;
    parsed.weights = TrackScorer::weightsFromJson(QJsonDocument(weights.toObject()).toJson(QJsonDocument::Compact),
                                                  &weightsError);
    if (!weightsError.isEmpty()) {
        *error = QStringLiteral("invalid profile weights: %1").arg(weightsError);
        return false;
    }
    const QJsonValue decay = object.value(QStringLiteral("sessionDecay"));
    if (!decay.isObject()) {
        *error = QStringLiteral("profile session decay is not an object");
        return false;
    }
    parsed.sessionDecay = decayFromJson(decay.toObject());
    *profile = std::move(parsed);
    return true;
}

} // namespace

bool operator==(const RadioProfile &left, const RadioProfile &right)
{
    return left.name == right.name
        && left.createdAtUtc == right.createdAtUtc
        && left.modifiedAtUtc == right.modifiedAtUtc
        && TrackScorer::weightsToJson(left.weights) == TrackScorer::weightsToJson(right.weights)
        && left.sessionDecay.decayStartTrack == right.sessionDecay.decayStartTrack
        && left.sessionDecay.decayCurve == right.sessionDecay.decayCurve
        && left.sessionDecay.noveltyDecayFloor == right.sessionDecay.noveltyDecayFloor
        && left.sessionDecay.ratingDecayFloor == right.sessionDecay.ratingDecayFloor;
}

bool operator!=(const RadioProfile &left, const RadioProfile &right)
{
    return !(left == right);
}

RadioProfileStore::RadioProfileStore()
{
    restoreFactoryDefaults();
}

bool RadioProfileStore::load()
{
    const QString path = storagePath();
    QFile file(path);
    if (!QFileInfo::exists(path)) {
        restoreFactoryDefaults();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "Could not read radio profiles:" << path;
        restoreFactoryDefaults();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning().noquote() << "Ignoring malformed radio profiles:" << path;
        restoreFactoryDefaults();
        return false;
    }
    const QJsonArray profiles = document.object().value(QStringLiteral("profiles")).toArray();
    QVector<RadioProfile> loaded;
    loaded.reserve(profiles.size());
    QSet<QString> names;
    for (const QJsonValue &value : profiles) {
        RadioProfile profile;
        QString error;
        if (!profileFromJson(value, &profile, &error) || names.contains(profile.name)) {
            qWarning().noquote() << "Ignoring malformed radio profiles:" << path << error;
            restoreFactoryDefaults();
            return false;
        }
        names.insert(profile.name);
        loaded.push_back(std::move(profile));
    }
    const QString active = document.object().value(QStringLiteral("activeProfile")).toString();
    if (loaded.isEmpty() || !names.contains(active)) {
        qWarning().noquote() << "Ignoring malformed radio profiles:" << path;
        restoreFactoryDefaults();
        return false;
    }
    m_profiles = std::move(loaded);
    m_activeProfileName = active;
    return true;
}

bool RadioProfileStore::save() const
{
    QJsonArray profiles;
    for (const RadioProfile &profile : m_profiles) {
        profiles.append(profileToJson(profile));
    }
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("activeProfile"), m_activeProfileName},
        {QStringLiteral("profiles"), profiles},
    };
    QFile file(storagePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << "Could not save radio profiles:" << storagePath();
        return false;
    }
    return file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0;
}

const QVector<RadioProfile> &RadioProfileStore::profiles() const
{
    return m_profiles;
}

QString RadioProfileStore::activeProfileName() const
{
    return m_activeProfileName;
}

const RadioProfile &RadioProfileStore::activeProfile() const
{
    for (const RadioProfile &profile : m_profiles) {
        if (profile.name == m_activeProfileName) {
            return profile;
        }
    }
    return m_profiles.front();
}

bool RadioProfileStore::setProfiles(QVector<RadioProfile> profiles, const QString &activeProfileName)
{
    if (profiles.isEmpty()) {
        return false;
    }
    QSet<QString> names;
    for (const RadioProfile &profile : profiles) {
        if (profile.name.trimmed().isEmpty() || names.contains(profile.name)) {
            return false;
        }
        names.insert(profile.name);
    }
    if (!names.contains(activeProfileName)) {
        return false;
    }
    m_profiles = std::move(profiles);
    m_activeProfileName = activeProfileName;
    return true;
}

bool RadioProfileStore::setActiveProfileName(const QString &name)
{
    for (const RadioProfile &profile : m_profiles) {
        if (profile.name == name) {
            m_activeProfileName = name;
            return true;
        }
    }
    return false;
}

RadioProfile RadioProfileStore::defaultProfile()
{
    const QString timestamp = nowUtc();
    return {QStringLiteral("Default"), timestamp, timestamp,
            TrackScorer::defaultWeights(), TrackScorer::defaultSessionDecay()};
}

QString RadioProfileStore::storagePath()
{
    return QDir(AppPaths::configDir()).filePath(QStringLiteral("radio-profiles.json"));
}

void RadioProfileStore::restoreFactoryDefaults()
{
    m_profiles = {defaultProfile()};
    m_activeProfileName = m_profiles.front().name;
}
