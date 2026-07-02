#pragma once

#include "scrobble/ListenHistoryStore.h"

#include <QDialog>
#include <QList>
#include <QString>

#include <optional>

class QLabel;
class QPushButton;
class QAbstractTableModel;
class QTableView;
class ResponsiveColumnLayout;

class ListeningHistoryDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ListeningHistoryDialog(ListenHistoryStore *store, QWidget *parent = nullptr);

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
    std::optional<ListenHistoryStore::HistoryRow> selectedHistoryRow() const;

    ListenHistoryStore *m_store = nullptr;
    QAbstractTableModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_queueLastFm = nullptr;
    QPushButton *m_queueListenBrainz = nullptr;
    QPushButton *m_forgetBehavior = nullptr;
    QPushButton *m_clearLastFm = nullptr;
    QPushButton *m_clearListenBrainz = nullptr;
};
