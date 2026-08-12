#include "scrobble/ScrobbleDestination.h"

#include "scrobble/ListenBrainzUrl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

constexpr int kDocumentVersion = 1;

constexpr auto kTypeLastFm = "lastfm";
constexpr auto kTypeCompatible = "listenbrainz";
constexpr auto kCustomIdPrefix = "custom-";

// The sequence number inside a minted id, or 0 for any id we did not mint.
// Recovering it from the ids themselves keeps the counter ahead of the list even
// if the stored counter is missing or has been edited backwards by hand.
int customSequenceOf(const QString &id)
{
    if (!id.startsWith(QLatin1String(kCustomIdPrefix))) {
        return 0;
    }
    bool numeric = false;
    const int sequence = QStringView(id).mid(QLatin1String(kCustomIdPrefix).size()).toInt(&numeric);
    return numeric ? sequence : 0;
}

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
    // The sequence only ever advances, so an id is never handed out twice even
    // after the destination that held it is removed.
    destination.id = QLatin1String(kCustomIdPrefix) + QString::number(nextCustomSequence++);
    destination.type = ScrobbleDestination::Type::ListenBrainzCompatible;
    destination.name = name;
    destination.apiRoot = apiRoot;
    destination.enabled = enabled;
    items.push_back(destination);
    return destination.id;
}

namespace ScrobbleDestinationConfig {

QString lastFmId() { return QStringLiteral("lastfm"); }
QString listenBrainzId() { return QStringLiteral("listenbrainz"); }

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
    int highestCustomSequence = 0;
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

        highestCustomSequence = std::max(highestCustomSequence, customSequenceOf(destination.id));
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

    const int storedSequence = root.value(QStringLiteral("nextCustomSequence")).toInt();
    set.nextCustomSequence = std::max({1, storedSequence, highestCustomSequence + 1});
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
    root.insert(QStringLiteral("nextCustomSequence"), destinations.nextCustomSequence);
    root.insert(QStringLiteral("destinations"), array);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString documentSettingKey() { return QStringLiteral("scrobble.destinations"); }

QString tokenSettingKey(const QString &id)
{
    if (id == listenBrainzId()) {
        return QStringLiteral("listenbrainz.token");
    }
    return QStringLiteral("scrobble.destination.%1.token").arg(id);
}

}   // namespace ScrobbleDestinationConfig
