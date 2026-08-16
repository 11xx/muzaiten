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
// Editing is stateless: a change is saved the moment it is made, and there is
// no accept or cancel. What the panel deliberately does not do is apply the
// result while it is open, so retyping a server URL cannot make the app
// reconnect on every keystroke; the owner applies once, when the window closes.
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
        std::function<void(const QString &destinationId, const QString &apiRoot, const QString &token)> testDestination;
        std::function<bool()> readOffline;
        std::function<void(bool offline)> writeOffline;
        std::function<void()> openLastFmSettings;
    };

    ScrobblersPanel(ScrobbleDestinationSet destinations, ListenHistoryStore *history, Callbacks callbacks,
                    QWidget *parent = nullptr);

    // The saved set, which omits any destination still missing its address.
    ScrobbleDestinationSet destinations() const;

public slots:
    // Result of a `testDestination` call, routed back by the owner.
    void reportTestResult(const QString &destinationId, bool valid, const QString &username);

private:
    friend class DestinationRow;

    void appendRow(const ScrobbleDestination &destination);
    void addDestination();
    void removeRow(DestinationRow *row);
    void save();
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
