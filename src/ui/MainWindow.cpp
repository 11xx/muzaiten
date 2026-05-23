#include "ui/MainWindow.h"

#include "Version.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/TrackTable.h"

#include <QSplitter>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("muzaiten %1").arg(QStringLiteral(MUZAITEN_VERSION)));
    resize(1440, 900);
    setMinimumSize(1100, 700);

    auto *root = new QSplitter(Qt::Horizontal, this);
    m_artistSidebar = new ArtistSidebar(root);

    auto *center = new QSplitter(Qt::Vertical, root);
    m_albumGrid = new AlbumGrid(center);
    m_trackTable = new TrackTable(center);
    center->setStretchFactor(0, 55);
    center->setStretchFactor(1, 45);

    root->addWidget(m_artistSidebar);
    root->addWidget(center);
    root->setStretchFactor(0, 0);
    root->setStretchFactor(1, 1);
    root->setSizes({260, 1180});

    setCentralWidget(root);
}

