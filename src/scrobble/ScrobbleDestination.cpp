#include "scrobble/ScrobbleDestination.h"

#include "scrobble/ListenBrainzUrl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace {

constexpr int kDocumentVersion = 1;

constexpr auto kTypeLastFm = "lastfm";
constexpr auto kTypeCompatible = "listenbrainz";
QString typeToString(ScrobbleDestination::Type type)
{
    return type == ScrobbleDestination::Type::LastFm ? QString::fromLatin1(kTypeLastFm)
                                                     : QString::fromLatin1(kTypeCompatible);
}

}   // namespace

bool ScrobbleDestination::isReserved() const
{
    return id == ScrobbleDestinationConfig::lastFmId() || id == ScrobbleDestinationConfig::listenBrainzId();
}

const ScrobbleDestination *ScrobbleDestinationSet::find(const QString &id) const
{
    for (const ScrobbleDestination &item : items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

QString ScrobbleDestinationSet::addCustom(const QString &name, const QString &apiRoot, bool enabled)
{
    ScrobbleDestination destination;
    destination.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    destination.type = ScrobbleDestination::Type::ListenBrainzCompatible;
    destination.name = name;
    destination.apiRoot = apiRoot;
    destination.enabled = enabled;
    items.push_back(destination);
    return destination.id;
}

bool ScrobbleDestinationSet::setEnabled(const QString &id, bool enabled)
{
    for (ScrobbleDestination &item : items) {
        if (item.id == id) {
            item.enabled = enabled;
            return true;
        }
    }
    return false;
}

namespace ScrobbleDestinationConfig {

QString lastFmId() { return QStringLiteral("lastfm"); }
QString listenBrainzId() { return QStringLiteral("listenbrainz"); }

bool isCustomId(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

ScrobbleDestinationSet defaults()
{
    ScrobbleDestination lastFm;
    lastFm.id = lastFmId();
    lastFm.type = ScrobbleDestination::Type::LastFm;
    lastFm.name = QStringLiteral("Last.fm");

    ScrobbleDestination listenBrainz;
    listenBrainz.id = listenBrainzId();
    listenBrainz.type = ScrobbleDestination::Type::ListenBrainzCompatible;
    listenBrainz.name = QStringLiteral("ListenBrainz");
    listenBrainz.apiRoot = ListenBrainzUrl::officialApiRoot();

    ScrobbleDestinationSet set;
    set.items = {lastFm, listenBrainz};
    return set;
}

ScrobbleDestinationSet fromJson(const QString &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray array = root.value(QStringLiteral("destinations")).toArray();
    if (array.isEmpty()) {
        return defaults();
    }

    ScrobbleDestinationSet set;
    QSet<QString> seen;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        ScrobbleDestination destination;
        destination.id = object.value(QStringLiteral("id")).toString();
        destination.name = object.value(QStringLiteral("name")).toString();
        destination.apiRoot = object.value(QStringLiteral("apiRoot")).toString();
        destination.enabled = object.value(QStringLiteral("enabled")).toBool();
        destination.type = object.value(QStringLiteral("type")).toString() == QLatin1String(kTypeLastFm)
                               ? ScrobbleDestination::Type::LastFm
                               : ScrobbleDestination::Type::ListenBrainzCompatible;
        if (destination.id.isEmpty() || seen.contains(destination.id)) {
            continue;
        }
        if (!destination.isReserved()
            && (!isCustomId(destination.id) || destination.type != ScrobbleDestination::Type::ListenBrainzCompatible)) {
            continue;
        }
        // A compatible destination with no URL has nowhere to deliver, so it is
        // not a destination at all. The reserved ones carry their own defaults.
        if (!destination.isReserved() && destination.apiRoot.isEmpty()) {
            continue;
        }
        if (destination.name.isEmpty()) {
            destination.name = destination.apiRoot;
        }
        seen.insert(destination.id);
        set.items.push_back(destination);

    }

    // The reserved destinations are part of the contract, not of the saved data:
    // restore them at their canonical position if the document lost them.
    const ScrobbleDestinationSet fallback = defaults();
    for (int index = 0; index < fallback.items.size(); ++index) {
        const ScrobbleDestination &reserved = fallback.items.at(index);
        if (!seen.contains(reserved.id)) {
            set.items.insert(std::min<qsizetype>(index, set.items.size()), reserved);
        }
    }
    // Reserved URLs are ours to define, so a stale or absent one is corrected
    // rather than trusted.
    for (ScrobbleDestination &item : set.items) {
        if (item.id == listenBrainzId()) {
            item.type = ScrobbleDestination::Type::ListenBrainzCompatible;
            item.apiRoot = ListenBrainzUrl::officialApiRoot();
        } else if (item.id == lastFmId()) {
            item.type = ScrobbleDestination::Type::LastFm;
            item.apiRoot.clear();
        }
    }

    return set;
}

QString toJson(const ScrobbleDestinationSet &destinations)
{
    QJsonArray array;
    for (const ScrobbleDestination &item : destinations.items) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), item.id);
        object.insert(QStringLiteral("type"), typeToString(item.type));
        object.insert(QStringLiteral("name"), item.name);
        object.insert(QStringLiteral("apiRoot"), item.apiRoot);
        object.insert(QStringLiteral("enabled"), item.enabled);
        array.push_back(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kDocumentVersion);
    root.insert(QStringLiteral("destinations"), array);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString documentSettingKey() { return QStringLiteral("scrobble.destinations"); }

ScrobbleDestinationSet load(const SettingReader &read)
{
    const QString document = read(documentSettingKey());
    if (!document.isEmpty()) {
        return fromJson(document);
    }

    // No document yet: this is the first run after gaining configurable
    // destinations. Carry the two reserved switches over from the settings that
    // used to hold them so nothing silently stops scrobbling.
    ScrobbleDestinationSet set = defaults();
    for (ScrobbleDestination &item : set.items) {
        const QString legacyKey = item.id == lastFmId() ? QStringLiteral("lastfm.enabled")
                                                        : QStringLiteral("listenbrainz.enabled");
        item.enabled = read(legacyKey) == QLatin1String("true");
    }
    return set;
}

void save(const SettingWriter &write, const ScrobbleDestinationSet &destinations)
{
    write(documentSettingKey(), toJson(destinations));
    // The legacy switches stay in step so anything still reading them, and a
    // downgrade to an older build, sees the reserved destinations' real state.
    for (const ScrobbleDestination &item : destinations.items) {
        if (!item.isReserved()) {
            continue;
        }
        const QString legacyKey = item.id == lastFmId() ? QStringLiteral("lastfm.enabled")
                                                        : QStringLiteral("listenbrainz.enabled");
        write(legacyKey, item.enabled ? QStringLiteral("true") : QStringLiteral("false"));
    }
}

QString tokenSettingKey(const QString &id)
{
    if (id == listenBrainzId()) {
        return QStringLiteral("listenbrainz.token");
    }
    if (!isCustomId(id)) {
        return {};
    }
    return QStringLiteral("scrobble.destination.%1.token").arg(id);
}

}   // namespace ScrobbleDestinationConfig
