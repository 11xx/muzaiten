#pragma once

#include "scrobble/ListenHistoryStore.h"

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QPushButton;
class QAbstractTableModel;
class QTableView;
class ResponsiveColumnLayout;

class ListeningHistoryDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ListeningHistoryDialog(ListenHistoryStore *store, QWidget *parent = nullptr);

signals:
    void backlogChanged(QString service, int changedCount);
    void statusMessageRequested(QString message, int timeoutMs);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reload();
    void queueSelected(const QString &service);
    void clearPending(const QString &service);
    void updateActions();
    QList<qint64> selectedIds() const;

    ListenHistoryStore *m_store = nullptr;
    QAbstractTableModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_queueLastFm = nullptr;
    QPushButton *m_queueListenBrainz = nullptr;
    QPushButton *m_clearLastFm = nullptr;
    QPushButton *m_clearListenBrainz = nullptr;
};
