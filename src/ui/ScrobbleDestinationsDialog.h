#pragma once

#include "scrobble/ScrobbleDestination.h"

#include <QDialog>
#include <QHash>
#include <QString>

#include <functional>

class ListenHistoryStore;
class QLabel;
class QPushButton;
class QTableWidget;

// The one place scrobbling destinations are managed: Last.fm, official
// ListenBrainz, and any number of ListenBrainz-compatible servers, each with
// its enabled state, credentials, connection status, and pending count.
//
// The dialog edits a working copy and hands it back through destinations() when
// accepted, so cancelling changes nothing. Side effects it cannot take back
// (deleting a removed destination's delivery rows and token) run only on
// accept, and only after the user has confirmed what is being discarded.
class ScrobbleDestinationsDialog final : public QDialog {
    Q_OBJECT

public:
    // What the dialog needs from the application, kept as callbacks so it does
    // not reach into the main window or the database itself.
    struct Callbacks {
        std::function<QString(const QString &destinationId)> readToken;
        std::function<void(const QString &destinationId, const QString &token)> writeToken;
        std::function<void(const QString &destinationId)> removeToken;
        // Whether Last.fm has a usable session; it authenticates through its own
        // settings dialog rather than a token field here.
        std::function<bool()> lastFmConfigured;
        // Fire and forget: the result arrives back through reportTestResult.
        std::function<void(const QString &destinationId, const QString &apiRoot, const QString &token)> testDestination;
    };

    ScrobbleDestinationsDialog(ScrobbleDestinationSet destinations, ListenHistoryStore *history,
                               Callbacks callbacks, QWidget *parent = nullptr);

    ScrobbleDestinationSet destinations() const { return m_destinations; }

public slots:
    // Result of a `testDestination` call, routed back by the owner.
    void reportTestResult(const QString &destinationId, bool valid, const QString &username);

private:
    void reloadTable();
    void updateButtons();
    void updateDuplicateRelayWarning();
    QString selectedId() const;

    void addDestination();
    void editSelected();
    void removeSelected();
    void testSelected();
    void toggleSelected();
    void applyRemovals();

    ScrobbleDestinationSet m_destinations;
    ListenHistoryStore *m_history = nullptr;
    Callbacks m_callbacks;

    // Destinations removed in this session, applied only if the dialog is
    // accepted; and the tokens typed for each destination, likewise.
    QStringList m_removedIds;
    QHash<QString, QString> m_pendingTokens;
    QHash<QString, QString> m_statusById;

    QTableWidget *m_table = nullptr;
    QLabel *m_warning = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
};
