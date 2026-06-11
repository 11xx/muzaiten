#include "ui/AlbumGrid.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <memory>

namespace {
constexpr int AlbumTitleRole = Qt::UserRole;
constexpr int RememberedOutlineRole = Qt::UserRole + 14;
}

class AlbumGridTest final : public QObject {
    Q_OBJECT

private slots:
    void mouseClickClearsExistingNarrowingBeforeRenarrowing();
    void ctrlAndShiftClickUpdateMarkedAlbums();
    void dragSelectionMarksIntersectedAlbums();
    void keyboardMarksPersistAcrossGridMovement();
    void mouseMarksClearAndUnnarrowAcrossGridMovement();
    void shiftClickExtendsKeyboardMarks();
    void unmarkLastKeyboardMarkClearsNarrowing();
    void unmarkAllClearsNarrowing();

private:
    static QVector<Album> makeAlbums();
    static AlbumGrid *makeGrid(const QVector<Album> &albums);
    static void clickRow(AlbumGrid *grid, int row);
    static void clickRow(AlbumGrid *grid, int row, Qt::KeyboardModifiers modifiers);
    static void dragRows(AlbumGrid *grid, int startRow, int endRow, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    static QString titleAt(const AlbumGrid *grid, int row);
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
    clickRow(grid, row, Qt::NoModifier);
}

void AlbumGridTest::clickRow(AlbumGrid *grid, int row, Qt::KeyboardModifiers modifiers)
{
    const QModelIndex index = grid->model()->index(row, 0);
    QVERIFY(index.isValid());
    const QRect rect = grid->visualRect(index);
    QVERIFY(rect.isValid());
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, modifiers, rect.center());
}

void AlbumGridTest::dragRows(AlbumGrid *grid, int startRow, int endRow, Qt::KeyboardModifiers modifiers)
{
    const QRect startRect = grid->visualRect(grid->model()->index(startRow, 0));
    const QRect endRect = grid->visualRect(grid->model()->index(endRow, 0));
    QVERIFY(startRect.isValid());
    QVERIFY(endRect.isValid());
    const QPoint start = startRect.center();
    const QPoint end = endRect.center();
    const auto sendMouse = [grid, modifiers](QEvent::Type type, const QPoint &pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type,
                          QPointF(pos),
                          QPointF(pos),
                          QPointF(grid->viewport()->mapToGlobal(pos)),
                          button,
                          buttons,
                          modifiers);
        QApplication::sendEvent(grid->viewport(), &event);
    };

    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QString AlbumGridTest::titleAt(const AlbumGrid *grid, int row)
{
    return grid->model()->index(row, 0).data(AlbumTitleRole).toString();
}

void AlbumGridTest::mouseClickClearsExistingNarrowingBeforeRenarrowing()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    QSignalSpy toggled(grid.get(), &AlbumGrid::albumSelectionToggled);
    QSignalSpy cleared(grid.get(), &AlbumGrid::albumSelectionCleared);

    const QString firstClickedTitle = grid->model()->index(1, 0).data(AlbumTitleRole).toString();
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

    const QString secondClickedTitle = grid->model()->index(2, 0).data(AlbumTitleRole).toString();
    clickRow(grid.get(), 2);
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(toggled.count(), 0);
    QCOMPARE(grid->currentAlbumTitle(), secondClickedTitle);
    QVERIFY(grid->currentIndex().data(RememberedOutlineRole).toBool());

    grid->setCurrentRow(1);
    QVERIFY(!grid->currentIndex().data(RememberedOutlineRole).toBool());
    grid->setCurrentRow(2);

    clickRow(grid.get(), 2);
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toString(), secondClickedTitle);
}

void AlbumGridTest::ctrlAndShiftClickUpdateMarkedAlbums()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    clickRow(grid.get(), 0, Qt::ControlModifier);
    QCOMPARE(grid->albumTitlesForAction(), QStringList{titleAt(grid.get(), 0)});

    clickRow(grid.get(), 2, Qt::ControlModifier);
    QCOMPARE(grid->albumTitlesForAction(), QStringList({titleAt(grid.get(), 0), titleAt(grid.get(), 2)}));

    grid->unmarkAllAlbums();
    grid->setCurrentRow(0);
    clickRow(grid.get(), 2, Qt::ShiftModifier);
    QCOMPARE(grid->albumTitlesForAction(), QStringList({titleAt(grid.get(), 0), titleAt(grid.get(), 1), titleAt(grid.get(), 2)}));
}

void AlbumGridTest::dragSelectionMarksIntersectedAlbums()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));
    QSignalSpy toggled(grid.get(), &AlbumGrid::albumSelectionToggled);
    QSignalSpy cleared(grid.get(), &AlbumGrid::albumSelectionCleared);

    dragRows(grid.get(), 0, 2);
    QCOMPARE(toggled.count(), 0);
    QCOMPARE(cleared.count(), 0);
    QCOMPARE(grid->albumTitlesForAction(), QStringList({titleAt(grid.get(), 0), titleAt(grid.get(), 1), titleAt(grid.get(), 2)}));
}

void AlbumGridTest::keyboardMarksPersistAcrossGridMovement()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    grid->setCurrentRow(0);
    grid->markCurrentAlbum();
    QSignalSpy narrowed(grid.get(), &AlbumGrid::albumNarrowFollowRequested);

    grid->moveCurrentByGrid(+1, 0);

    QCOMPARE(grid->currentRow(), 1);
    QCOMPARE(grid->albumTitlesForAction(), QStringList{titleAt(grid.get(), 0)});
    QCOMPARE(narrowed.count(), 0);
}

void AlbumGridTest::mouseMarksClearAndUnnarrowAcrossGridMovement()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    clickRow(grid.get(), 0, Qt::ControlModifier);
    clickRow(grid.get(), 2, Qt::ControlModifier);
    QCOMPARE(grid->albumTitlesForAction(), QStringList({titleAt(grid.get(), 0), titleAt(grid.get(), 2)}));

    QSignalSpy narrowed(grid.get(), &AlbumGrid::albumNarrowFollowRequested);
    grid->moveCurrentByGrid(-1, 0);

    QCOMPARE(grid->currentRow(), 1);
    QCOMPARE(grid->albumTitlesForAction(), QStringList{titleAt(grid.get(), 1)});
    QCOMPARE(narrowed.count(), 1);
    QCOMPARE(qvariant_cast<QStringList>(narrowed.takeFirst().at(0)), QStringList{});
}

void AlbumGridTest::shiftClickExtendsKeyboardMarks()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    grid->setCurrentRow(0);
    grid->markCurrentAlbum();
    grid->setCurrentRow(1);

    clickRow(grid.get(), 2, Qt::ShiftModifier);

    QCOMPARE(grid->albumTitlesForAction(), QStringList({titleAt(grid.get(), 0), titleAt(grid.get(), 1), titleAt(grid.get(), 2)}));
}

void AlbumGridTest::unmarkLastKeyboardMarkClearsNarrowing()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    grid->setCurrentRow(1);
    grid->markCurrentAlbum();
    QSignalSpy narrowed(grid.get(), &AlbumGrid::albumNarrowFollowRequested);

    grid->unmarkCurrentAlbum();

    QCOMPARE(narrowed.count(), 1);
    QCOMPARE(qvariant_cast<QStringList>(narrowed.takeFirst().at(0)), QStringList{});
    QCOMPARE(grid->albumTitlesForAction(), QStringList{titleAt(grid.get(), 1)});
}

void AlbumGridTest::unmarkAllClearsNarrowing()
{
    const QVector<Album> albums = makeAlbums();
    std::unique_ptr<AlbumGrid> grid(makeGrid(albums));

    grid->setCurrentRow(0);
    grid->markCurrentAlbum();
    grid->setCurrentRow(2);
    grid->markCurrentAlbum();
    QSignalSpy narrowed(grid.get(), &AlbumGrid::albumNarrowFollowRequested);

    grid->unmarkAllAlbums();

    QCOMPARE(narrowed.count(), 1);
    QCOMPARE(qvariant_cast<QStringList>(narrowed.takeFirst().at(0)), QStringList{});
    QCOMPARE(grid->albumTitlesForAction(), QStringList{titleAt(grid.get(), 2)});
}

QTEST_MAIN(AlbumGridTest)

#include "test_album_grid.moc"
