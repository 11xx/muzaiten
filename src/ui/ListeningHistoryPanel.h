#pragma once

#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <optional>

class QAction;
class QLabel;
class QPushButton;
class QAbstractTableModel;
class QTableView;
class ResponsiveColumnLayout;

class ListeningHistoryPanel final : public QWidget {
    Q_OBJECT

public:
    ListeningHistoryPanel(ListenHistoryStore *store, ScrobbleDestinationSet destinations,
                           QWidget *parent = nullptr);

    // Adopts a destination set edited elsewhere while this panel is open,
    // keeping whichever picks survived the change.
    void setDestinations(const ScrobbleDestinationSet &destinations);

    // Restores the persisted Ctrl+wheel row height (the panel is recreated per
    // open, so the owner round-trips it through settings).
    void setRowHeight(int height);

signals:
    void backlogChanged(QString service, int changedCount);
    void statusMessageRequested(QString message, int timeoutMs);
    void rowHeightChanged(int height);
    void forgetBehaviorRequested(const Track &track, bool includeImportedListens);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reload();
    void queueSelected();
    void retryPending();
    void clearBacklogs();
    void forgetSelectedBehavior();
    void updateActions();
    void updateDestinationButton();
    QList<qint64> selectedIds() const;
    // The destinations the user picked. Empty is the read-only overview, which
    // is what the mutating actions test for.
    QStringList scopedDestinationIds() const;
    // The destinations the counts answer for: the picks, or every destination
    // when nothing is picked.
    QStringList effectiveScopeIds() const;
    QString destinationName(const QString &destinationId) const;
    std::optional<ListenHistoryStore::HistoryRow> selectedHistoryRow() const;

    ListenHistoryStore *m_store = nullptr;
    QAbstractTableModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_destinationButton = nullptr;
    QList<QAction *> m_destinationActions;
    QPushButton *m_queueSelected = nullptr;
    QPushButton *m_forgetBehavior = nullptr;
    QPushButton *m_retryPending = nullptr;
    QPushButton *m_clearBacklog = nullptr;
    ScrobbleDestinationSet m_destinations;
};
