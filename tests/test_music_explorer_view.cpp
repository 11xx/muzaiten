#include "ui/MusicExplorerView.h"

#include <QApplication>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

class MusicExplorerViewTest final : public QObject {
    Q_OBJECT

private slots:
    void firstShowUsesAlbumGridColumns();
    void albumKeysMoveByAlbumAndNFocusesTracks();
    void inlineTrackKeysNavigateAndReturnFocus();
    void expandsExactlyOneAlbumAndShowsAllTracks();
    void mouseClickExpandsAndShowsVisibleTracks();
    void selectionMovesExpandedPanel();
    void albumActionsForwardSignals();

private:
    static QVector<Album> makeAlbums();
    static QVector<Track> makeTracks(const QString &album);
};

QVector<Album> MusicExplorerViewTest::makeAlbums()
{
    return {
        {.title = QStringLiteral("One"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2001"), .trackCount = 2},
        {.title = QStringLiteral("Two"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2002"), .trackCount = 3},
        {.title = QStringLiteral("Three"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2003"), .trackCount = 1},
    };
}

QVector<Track> MusicExplorerViewTest::makeTracks(const QString &album)
{
    const int count = album == QStringLiteral("Two") ? 3 : album == QStringLiteral("Three") ? 1 : 2;
    QVector<Track> tracks;
    tracks.reserve(count);
    for (int i = 0; i < count; ++i) {
        Track track;
        track.path = QStringLiteral("/music/Artist/%1/%2.flac").arg(album).arg(i + 1, 2, 10, QLatin1Char('0'));
        track.parentDir = QStringLiteral("/music/Artist/%1").arg(album);
        track.filename = QStringLiteral("%1.flac").arg(i + 1, 2, 10, QLatin1Char('0'));
        track.title = QStringLiteral("Track %1").arg(i + 1);
        track.artistName = QStringLiteral("Artist");
        track.albumArtistName = QStringLiteral("Artist");
        track.albumTitle = album;
        track.trackNumber = i + 1;
        track.durationMs = 60000;
        tracks.push_back(track);
    }
    return tracks;
}

void MusicExplorerViewTest::expandsExactlyOneAlbumAndShowsAllTracks()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("Two"), true);
    QCOMPARE(view.expandedAlbumTitle(), QStringLiteral("Two"));
    QCOMPARE(view.expandedPanelCountForTests(), 1);
    QVERIFY(view.inlineTrackTableForTests() != nullptr);
    QCOMPARE(view.inlineTrackTableForTests()->rowCount(), 3);
    QVERIFY(view.pointerXForTests() > 0);
}

void MusicExplorerViewTest::firstShowUsesAlbumGridColumns()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QTRY_VERIFY(view.columnCountForTests() >= 3);
    QTRY_VERIFY(!view.cardGeometryForTests(0).isEmpty());
    QTRY_VERIFY(!view.cardGeometryForTests(1).isEmpty());
    QTRY_COMPARE(view.cardGeometryForTests(1).top(), view.cardGeometryForTests(0).top());
    QTRY_VERIFY(view.cardGeometryForTests(1).left() > view.cardGeometryForTests(0).left());
}

void MusicExplorerViewTest::albumKeysMoveByAlbumAndNFocusesTracks()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QCOMPARE(view.currentRow(), 0);
    QTest::keyClick(&view, Qt::Key_J);
    QCOMPARE(view.currentRow(), 1);
    QTest::keyClick(&view, Qt::Key_K);
    QCOMPARE(view.currentRow(), 0);

    QTest::keyClick(&view, Qt::Key_N);
    QTRY_COMPARE(view.expandedPanelCountForTests(), 1);
    QVERIFY(view.inlineTrackTableForTests()->hasFocus());
    QVERIFY(view.inlineTrackTableForTests()->isVisible());
    QVERIFY(view.inlineTrackTableForTests()->rowCount() > 0);
}

void MusicExplorerViewTest::inlineTrackKeysNavigateAndReturnFocus()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("Two"), true);
    QVERIFY(view.inlineTrackTableForTests()->hasFocus());
    QCOMPARE(view.inlineTrackTableForTests()->currentRow(), 0);

    QTest::keyClick(view.inlineTrackTableForTests(), Qt::Key_J);
    QCOMPARE(view.inlineTrackTableForTests()->currentRow(), 1);
    QTest::keyClick(view.inlineTrackTableForTests(), Qt::Key_H);
    QVERIFY(view.hasFocus());
}

void MusicExplorerViewTest::mouseClickExpandsAndShowsVisibleTracks()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    const QRect card = view.cardGeometryForTests(1);
    QVERIFY(!card.isEmpty());
    QWidget *target = view.cardWidgetForTests(1);
    QVERIFY(target != nullptr);
    QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, target->rect().center());

    QTRY_COMPARE(view.expandedPanelCountForTests(), 1);
    QVERIFY(view.inlineTrackTableForTests() != nullptr);
    QVERIFY(view.inlineTrackTableForTests()->isVisible());
    QVERIFY(view.inlineTrackTableForTests()->rowCount() > 0);
}

void MusicExplorerViewTest::selectionMovesExpandedPanel()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("One"), false);
    QCOMPARE(view.expandedPanelCountForTests(), 1);
    QCOMPARE(view.inlineTrackTableForTests()->rowCount(), 2);

    view.selectAlbumTitle(QStringLiteral("Three"), false);
    QCOMPARE(view.expandedAlbumTitle(), QStringLiteral("Three"));
    QCOMPARE(view.expandedPanelCountForTests(), 1);
    QCOMPARE(view.inlineTrackTableForTests()->rowCount(), 1);
}

void MusicExplorerViewTest::albumActionsForwardSignals()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.setCurrentRow(1);
    QSignalSpy addSpy(&view, &MusicExplorerView::albumAddToQueueRequested);

    QKeyEvent event(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);

    QCOMPARE(addSpy.count(), 1);
    QCOMPARE(addSpy.takeFirst().at(0).toString(), QStringLiteral("Two"));
}

QTEST_MAIN(MusicExplorerViewTest)

#include "test_music_explorer_view.moc"
