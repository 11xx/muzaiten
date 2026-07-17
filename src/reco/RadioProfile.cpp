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

#include <algorithm>

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

QJsonObject historyToJson(const RadioProfileStore::History &history)
{
    QJsonArray snapshots;
    for (const RadioProfile &snapshot : history.snapshots) {
        snapshots.append(profileToJson(snapshot));
    }
    return {{QStringLiteral("current"), history.current}, {QStringLiteral("snapshots"), snapshots}};
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
    QHash<QString, History> histories;
    const QJsonObject historiesObject = document.object().value(QStringLiteral("histories")).toObject();
    for (const RadioProfile &profile : loaded) {
        History history;
        const QJsonObject historyObject = historiesObject.value(profile.name).toObject();
        const QJsonArray snapshots = historyObject.value(QStringLiteral("snapshots")).toArray();
        for (const QJsonValue &snapshotValue : snapshots) {
            RadioProfile snapshot;
            QString error;
            if (!profileFromJson(snapshotValue, &snapshot, &error)) {
                qWarning().noquote() << "Ignoring malformed radio profile history:" << path << error;
                restoreFactoryDefaults();
                return false;
            }
            history.snapshots.push_back(std::move(snapshot));
        }
        if (history.snapshots.isEmpty()) {
            history.snapshots.push_back(profile);
        }
        while (history.snapshots.size() > HistoryLimit) {
            history.snapshots.removeFirst();
        }
        const int lastSnapshot = static_cast<int>(history.snapshots.size()) - 1;
        history.current = std::clamp(historyObject.value(QStringLiteral("current")).toInt(lastSnapshot),
                                     0, lastSnapshot);
        // The persisted profile is authoritative for compatibility with files
        // written before histories existed; align its current snapshot with it.
        history.snapshots[history.current] = profile;
        histories.insert(profile.name, std::move(history));
    }
    m_profiles = std::move(loaded);
    m_activeProfileName = active;
    m_histories = std::move(histories);
    return true;
}

bool RadioProfileStore::save() const
{
    QJsonArray profiles;
    for (const RadioProfile &profile : m_profiles) {
        profiles.append(profileToJson(profile));
    }
    QJsonObject histories;
    for (auto it = m_histories.cbegin(); it != m_histories.cend(); ++it) {
        histories.insert(it.key(), historyToJson(it.value()));
    }
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("activeProfile"), m_activeProfileName},
        {QStringLiteral("profiles"), profiles},
        {QStringLiteral("histories"), histories},
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
    m_histories.clear();
    for (const RadioProfile &profile : m_profiles) {
        ensureHistory(profile.name, profile);
    }
    return true;
}

RadioProfile *RadioProfileStore::activeProfileMutable()
{
    for (RadioProfile &profile : m_profiles) {
        if (profile.name == m_activeProfileName) {
            return &profile;
        }
    }
    return nullptr;
}

const RadioProfileStore::History *RadioProfileStore::activeHistory() const
{
    const auto it = m_histories.constFind(m_activeProfileName);
    return it == m_histories.cend() ? nullptr : &it.value();
}

RadioProfileStore::History *RadioProfileStore::activeHistoryMutable()
{
    const auto it = m_histories.find(m_activeProfileName);
    return it == m_histories.end() ? nullptr : &it.value();
}

void RadioProfileStore::ensureHistory(const QString &name, const RadioProfile &profile)
{
    if (!m_histories.contains(name)) {
        m_histories.insert(name, History{{profile}, 0});
    }
}

void RadioProfileStore::pushActiveSnapshot()
{
    RadioProfile *profile = activeProfileMutable();
    History *history = activeHistoryMutable();
    if (profile == nullptr || history == nullptr) {
        return;
    }
    while (history->snapshots.size() > history->current + 1) {
        history->snapshots.removeLast();
    }
    if (history->snapshots.value(history->current) == *profile) {
        return;
    }
    history->snapshots.push_back(*profile);
    while (history->snapshots.size() > HistoryLimit) {
        history->snapshots.removeFirst();
    }
    history->current = static_cast<int>(history->snapshots.size()) - 1;
}

bool RadioProfileStore::previewActiveProfile(const RadioProfile &profile)
{
    RadioProfile *current = activeProfileMutable();
    if (current == nullptr || profile.name != m_activeProfileName || profile.name.trimmed().isEmpty()) {
        return false;
    }
    *current = profile;
    current->modifiedAtUtc = nowUtc();
    return true;
}

bool RadioProfileStore::commitActivePreview()
{
    if (activeProfileMutable() == nullptr) {
        return false;
    }
    pushActiveSnapshot();
    return true;
}

bool RadioProfileStore::restoreActiveProfile(const RadioProfile &profile)
{
    RadioProfile *current = activeProfileMutable();
    if (current == nullptr || profile.name != m_activeProfileName) {
        return false;
    }
    *current = profile;
    return true;
}

bool RadioProfileStore::canUndo() const
{
    const History *history = activeHistory();
    return history != nullptr && history->current > 0;
}

bool RadioProfileStore::canRedo() const
{
    const History *history = activeHistory();
    return history != nullptr && history->current + 1 < history->snapshots.size();
}

bool RadioProfileStore::undo()
{
    History *history = activeHistoryMutable();
    RadioProfile *profile = activeProfileMutable();
    if (history == nullptr || profile == nullptr || history->current <= 0) {
        return false;
    }
    --history->current;
    *profile = history->snapshots.at(history->current);
    return true;
}

bool RadioProfileStore::redo()
{
    History *history = activeHistoryMutable();
    RadioProfile *profile = activeProfileMutable();
    if (history == nullptr || profile == nullptr || history->current + 1 >= history->snapshots.size()) {
        return false;
    }
    ++history->current;
    *profile = history->snapshots.at(history->current);
    return true;
}

bool RadioProfileStore::createProfile(const QString &name, bool duplicateActive)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_histories.contains(trimmed)) {
        return false;
    }
    RadioProfile profile = duplicateActive ? activeProfile() : defaultProfile();
    profile.name = trimmed;
    profile.createdAtUtc = nowUtc();
    profile.modifiedAtUtc = profile.createdAtUtc;
    m_profiles.push_back(profile);
    m_activeProfileName = trimmed;
    ensureHistory(trimmed, profile);
    return true;
}

bool RadioProfileStore::renameActiveProfile(const QString &name)
{
    const QString trimmed = name.trimmed();
    RadioProfile *profile = activeProfileMutable();
    if (profile == nullptr || trimmed.isEmpty() || (trimmed != profile->name && m_histories.contains(trimmed))) {
        return false;
    }
    const QString oldName = profile->name;
    profile->name = trimmed;
    profile->modifiedAtUtc = nowUtc();
    History history = m_histories.take(oldName);
    for (RadioProfile &snapshot : history.snapshots) {
        snapshot.name = trimmed;
    }
    m_histories.insert(trimmed, std::move(history));
    m_activeProfileName = trimmed;
    commitActivePreview();
    return true;
}

bool RadioProfileStore::deleteActiveProfile()
{
    if (m_activeProfileName == QLatin1String("Default") || m_profiles.size() <= 1) {
        return false;
    }
    const QString deleted = m_activeProfileName;
    m_profiles.erase(std::remove_if(m_profiles.begin(), m_profiles.end(), [&deleted](const RadioProfile &profile) {
        return profile.name == deleted;
    }), m_profiles.end());
    m_histories.remove(deleted);
    m_activeProfileName = m_profiles.front().name;
    return true;
}

bool RadioProfileStore::resetActiveProfile()
{
    RadioProfile *profile = activeProfileMutable();
    if (profile == nullptr) {
        return false;
    }
    profile->weights = TrackScorer::defaultWeights();
    profile->sessionDecay = TrackScorer::defaultSessionDecay();
    profile->modifiedAtUtc = nowUtc();
    pushActiveSnapshot();
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
    m_histories.clear();
    ensureHistory(m_activeProfileName, m_profiles.front());
}
