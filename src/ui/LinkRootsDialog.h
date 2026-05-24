#pragma once

#include "fs/LinkRoot.h"

#include <QDialog>
#include <QVector>

class QTableWidget;

class LinkRootsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit LinkRootsDialog(QWidget *parent = nullptr);

    void setLinkRoots(const QVector<LinkRoot> &roots);
    QVector<LinkRoot> linkRoots() const;

private:
    void addRootRow(const LinkRoot &root);
    void addBlankRow();
    void removeSelectedRows();

    QTableWidget *m_table = nullptr;
};
