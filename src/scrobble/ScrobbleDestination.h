#pragma once

#include <QList>
#include <QString>

#include <functional>

// One place a listen can be delivered to. Two destinations are reserved and
// always present: Last.fm and the official ListenBrainz service. Beyond those,
// the user may configure any number of ListenBrainz-compatible servers (Koito
// and friends), each with its own URL, credentials, and delivery backlog.
//
// A destination's `id` is its permanent identity. It keys the delivery rows and
// the stored token, so it survives a rename or a URL change: pointing a custom
// destination at a moved server keeps the backlog that was owed to it. Minted
// UUID-style ids are opaque and never reused, so a deleted destination's rows
// can never be inherited by a later one.
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

// A configured set of destinations. Round-trips through the library settings
// store as one JSON document.
struct ScrobbleDestinationSet {
    QList<ScrobbleDestination> items;

    const ScrobbleDestination *find(const QString &id) const;
    // Appends a ListenBrainz-compatible destination with a freshly minted,
    // opaque id that is never deliberately reused.
    QString addCustom(const QString &name, const QString &apiRoot, bool enabled);
    bool setEnabled(const QString &id, bool enabled);
};

namespace ScrobbleDestinationConfig {

// Reserved ids. These are also the legacy per-service flag names in the listen
// history, which is what lets the old owed/sent columns migrate across without
// remapping identity.
QString lastFmId();
QString listenBrainzId();
bool isCustomId(const QString &id);

// The two reserved destinations, disabled, in display order.
ScrobbleDestinationSet defaults();

// Parse a stored document. Anything malformed degrades to the defaults rather
// than dropping the user's configuration silently; the reserved destinations are
// re-inserted if absent, and custom entries with an invalid UUID-style id are
// discarded. A custom entry's address is normalized on load and the entry is
// discarded if it cannot be, so a document written by hand or by an older build
// cannot load an address the Add flow would refuse.
ScrobbleDestinationSet fromJson(const QString &json);
QString toJson(const ScrobbleDestinationSet &destinations);

// Settings key holding the destination document.
QString documentSettingKey();

using SettingReader = std::function<QString(const QString &key)>;
using SettingWriter = std::function<void(const QString &key, const QString &value)>;

// Reads the configured set. On the first read after upgrading, when no document
// exists yet, the reserved destinations inherit their enabled state from the
// long-standing `lastfm.enabled` / `listenbrainz.enabled` settings, so a user
// who had scrobbling on keeps it on.
ScrobbleDestinationSet load(const SettingReader &read);
void save(const SettingWriter &write, const ScrobbleDestinationSet &destinations);
// Settings key holding one destination's token. The reserved destinations keep
// their long-standing keys so existing credentials survive untouched.
//
// Empty for a destination that stores no token of its own (Last.fm holds a
// session key instead) and for an unrecognized id. Callers that write must
// check: an empty key names no row, so writing under it would store a
// credential where nothing will ever look for it or clear it.
QString tokenSettingKey(const QString &id);

}   // namespace ScrobbleDestinationConfig
