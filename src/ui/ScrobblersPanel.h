#pragma once

#include "scrobble/ScrobbleDestination.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class DestinationRow;
class ListenHistoryStore;
class QGridLayout;
class QLabel;
class ToggleSwitch;

// The one place scrobbling destinations are managed: Last.fm, official
// ListenBrainz, and any number of ListenBrainz-compatible servers, each with
// its enabled state, credentials, connection status, and pending count. It also
// carries the two scrobbling-wide controls that belong with them, offline mode
// and the Last.fm API credentials.
//
// Editing is stateless: a change is saved the moment it is made and applied
// with it, and there is no accept or cancel. Applying is per completed edit,
// never per keystroke, because a field commits when it is left rather than as
// it is typed.
//
// Saving without applying was tried and abandoned. The two states then
// disagree, and every path that delivers a listen in between - a backlog
// retried from the history tab, a track simply finishing while the window sits
// open - delivers through a scrobbler still holding the address or token the
// destination has stopped using, and records it as sent. The scrobblers are
// configured as a set, so applying one destination's edit applies every
// destination's anyway; there was never a partial apply to be had.
//
// A destination with no server address cannot deliver, so it cannot be enabled
// and it is not written to the configuration until it has one. Removal is
// immediate and cannot be taken back, so it confirms first and says exactly
// what is being discarded.
class ScrobblersPanel final : public QWidget {
    Q_OBJECT

public:
    // What the dialog needs from the application, kept as callbacks so it does
    // not reach into the main window or the database itself.
    struct Callbacks {
        std::function<QString(const QString &destinationId)> readToken;
        std::function<void(const QString &destinationId, const QString &token)> writeToken;
        std::function<void(const QString &destinationId)> removeToken;
        // Persist the set. Called on every change, and never applies it.
        std::function<void(const ScrobbleDestinationSet &destinations)> saveDestinations;
        // Whether Last.fm has a usable session; it authenticates through its own
        // settings dialog rather than a token field here.
        std::function<bool()> lastFmConfigured;
        // Fire and forget: the result arrives back through reportTestResult.
        // `requestId` comes back with the result, so a row can tell which of its
        // own tests an answer belongs to.
        std::function<void(const QString &destinationId, quint64 requestId, const QString &apiRoot,
                           const QString &token)>
            testDestination;
        std::function<bool()> readOffline;
        std::function<void(bool offline)> writeOffline;
        std::function<void()> openLastFmSettings;
    };

    ScrobblersPanel(ScrobbleDestinationSet destinations, ListenHistoryStore *history, Callbacks callbacks,
                    QWidget *parent = nullptr);

    // The saved set, which omits any destination still missing its address.
    ScrobbleDestinationSet destinations() const;

signals:
    // The set was just saved. Anything showing destinations alongside this
    // panel is looking at a set that has moved.
    void destinationsChanged(const ScrobbleDestinationSet &destinations);

    // An edit has landed that the running scrobblers need to know about.
    // Emitted once per completed edit, never per keystroke.
    void applyRequested();

public slots:
    // Result of a `testDestination` call, routed back by the owner.
    void reportTestResult(const QString &destinationId, quint64 requestId, bool valid, const QString &username);

    // A destination's enabled state changed outside this panel, which happens
    // when Last.fm authenticates or disables itself while the window is open.
    // Adopted into the working copy so the next save does not write it back.
    void adoptEnabledState(const QString &destinationId, bool enabled);

    // Re-reads every row's pending count, which changes whenever a backlog is
    // queued, retried, or cleared elsewhere.
    void refreshPendingCounts();

private:
    friend class DestinationRow;

    void appendRow(const ScrobbleDestination &destination, bool alreadyConfigured);
    void addDestination();
    void removeRow(DestinationRow *row);
    void save();
    quint64 nextTestRequestId();
    void updateNotices();

    ListenHistoryStore *m_history = nullptr;
    Callbacks m_callbacks;

    QList<DestinationRow *> m_rows;
    QGridLayout *m_grid = nullptr;
    QWidget *m_rowHost = nullptr;
    QLabel *m_problem = nullptr;
    QLabel *m_relayWarning = nullptr;
    ToggleSwitch *m_offline = nullptr;
    int m_nextGridRow = 0;
};
