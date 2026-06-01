#include "ui/AlbumGrid.h"

#include <QSignalSpy>
#include <QTest>

#include <memory>

class AlbumGridTest final : public QObject {
    Q_OBJECT

private slots:
    void mouseClickClearsExistingNarrowingBeforeRenarrowing();

private:
    static QVector<Album> makeAlbums();
    static AlbumGrid *makeGrid(const QVector<Album> &albums);
    static void clickRow(AlbumGrid *grid, int row);
};

QVector<Album> AlbumGridTest::makeAlbums()
{
    return {
        {.title = QStringLiteral("One"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2001"), .trackCount = 1},
        {.title = QStringLiteral("Two"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2002"), .trackCount = 1},
        {.title = QStringLiteral("Three"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2003"), .trackCount = 1},
    };
}

AlbumGrid *AlbumGridTest::makeGrid(const QVector<Album> &albums)
{
    auto *grid = new AlbumGrid;
    grid->setFixedSize(700, 360);
    grid->setAlbums(albums, false);
    grid->show();
    const bool exposed = QTest::qWaitForWindowExposed(grid);
    Q_UNUSED(exposed)
    return grid;
}

void AlbumGridTest::clickRow(AlbumGrid *grid, int row)
{
    const QModelIndex index = grid->model()->index(row, 0);
    QVERIFY(index.isValid());
    const QRect rect = grid->visualRect(index);
    QVERIFY(rect.isValid());
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
}

void AlbumGridTest::mouseClickClearsExistingNarrowingBeforeRenarrowing()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    QSignalSpy toggled(grid.get(), &AlbumGrid::albumSelectionToggled);
    QSignalSpy cleared(grid.get(), &AlbumGrid::albumSelectionCleared);

    const QString firstClickedTitle = grid->model()->index(1, 0).data(Qt::UserRole).toString();
    clickRow(grid.get(), 1);
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toString(), firstClickedTitle);
    QCOMPARE(cleared.count(), 0);

    grid->setSelectedAlbumTitle(firstClickedTitle);
    grid->setAlbums(albums, false);
    QCOMPARE(grid->currentAlbumTitle(), firstClickedTitle);

    connect(grid.get(), &AlbumGrid::albumSelectionCleared, grid.get(), [&albums, grid = grid.get()]() {
        grid->setSelectedAlbumTitle(QString());
        grid->setAlbums(albums, false);
    });

    const QString secondClickedTitle = grid->model()->index(2, 0).data(Qt::UserRole).toString();
    clickRow(grid.get(), 2);
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(toggled.count(), 0);
    QCOMPARE(grid->currentAlbumTitle(), secondClickedTitle);

    clickRow(grid.get(), 2);
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toString(), secondClickedTitle);
}

QTEST_MAIN(AlbumGridTest)

#include "test_album_grid.moc"
