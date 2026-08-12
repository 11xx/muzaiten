#pragma once

#include <QList>
#include <QString>

// One place a listen can be delivered to. Two destinations are reserved and
// always present: Last.fm and the official ListenBrainz service. Beyond those,
// the user may configure any number of ListenBrainz-compatible servers (Koito
// and friends), each with its own URL, credentials, and delivery backlog.
//
// A destination's `id` is its permanent identity. It keys the delivery rows and
// the stored token, so it survives a rename or a URL change: pointing a custom
// destination at a moved server keeps the backlog that was owed to it. Minted
// ids are opaque and never reused, so a deleted destination's rows can never be
// inherited by a later one.
struct ScrobbleDestination {
    enum class Type {
        LastFm,
        ListenBrainzCompatible,
    };

    QString id;
    Type type = Type::ListenBrainzCompatible;
    QString name;
    QString apiRoot;   // canonical ListenBrainz API root; unused for Last.fm
    bool enabled = false;

    // The reserved destinations cannot be removed, and neither their type nor
    // their URL is editable; only custom ones are fully user-owned.
    bool isReserved() const;
};

// A configured set of destinations plus the counter that guarantees minted ids
// are never reused. Round-trips through the library settings store as one JSON
// document, so the list and the counter can never disagree.
struct ScrobbleDestinationSet {
    QList<ScrobbleDestination> items;
    int nextCustomSequence = 1;

    const ScrobbleDestination *find(const QString &id) const;
    // Appends a ListenBrainz-compatible destination with a freshly minted id and
    // returns that id, consuming one sequence number.
    QString addCustom(const QString &name, const QString &apiRoot, bool enabled);
};

namespace ScrobbleDestinationConfig {

// Reserved ids. These are also the legacy per-service flag names in the listen
// history, which is what lets the old owed/sent columns migrate across without
// remapping identity.
QString lastFmId();
QString listenBrainzId();

// The two reserved destinations, disabled, in display order.
ScrobbleDestinationSet defaults();

// Parse a stored document. Anything malformed degrades to the defaults rather
// than dropping the user's configuration silently; the reserved destinations are
// re-inserted if absent, and custom entries missing an id or URL are discarded.
ScrobbleDestinationSet fromJson(const QString &json);
QString toJson(const ScrobbleDestinationSet &destinations);

// Settings key holding the destination document.
QString documentSettingKey();
// Settings key holding one destination's token. The reserved destinations keep
// their long-standing keys so existing credentials survive untouched.
QString tokenSettingKey(const QString &id);

}   // namespace ScrobbleDestinationConfig
