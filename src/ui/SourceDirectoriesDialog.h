#pragma once

#include "core/ScanRoot.h"

#include <QDialog>
#include <QVector>

class QLabel;
class QTableWidget;

class SourceDirectoriesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SourceDirectoriesDialog(QWidget *parent = nullptr);

    void setScanRoots(const QVector<ScanRoot> &roots);
    QVector<ScanRoot> scanRoots() const;

signals:
    void scanRootsRequested(const QVector<ScanRoot> &roots);

private:
    void addDirectory();
    void removeSelectedRows();
    void scanSelectedRows();
    void scanEnabledRows();
    void addRootRow(const ScanRoot &root);
    void updateStatuses();

    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};
