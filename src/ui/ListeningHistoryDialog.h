#pragma once

#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QComboBox;
class QLabel;
class QPushButton;
class QAbstractTableModel;
class QTableView;
class ResponsiveColumnLayout;

class ListeningHistoryDialog final : public QDialog {
    Q_OBJECT

public:
    ListeningHistoryDialog(ListenHistoryStore *store, ScrobbleDestinationSet destinations,
                           QWidget *parent = nullptr);

    // Restores the persisted Ctrl+wheel row height (the dialog is recreated per
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
    void queueSelected(const QString &service);
    void clearPending(const QString &service);
    void forgetSelectedBehavior();
    void updateActions();
    QList<qint64> selectedIds() const;
    // The chosen destination, or empty for the aggregate view.
    QString selectedDestinationId() const;
    QString destinationName(const QString &destinationId) const;
    std::optional<ListenHistoryStore::HistoryRow> selectedHistoryRow() const;

    ListenHistoryStore *m_store = nullptr;
    QAbstractTableModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_summary = nullptr;
    QComboBox *m_destinationSelector = nullptr;
    QPushButton *m_queueSelected = nullptr;
    QPushButton *m_forgetBehavior = nullptr;
    QPushButton *m_retryPending = nullptr;
    QPushButton *m_clearBacklog = nullptr;
    ScrobbleDestinationSet m_destinations;
};
