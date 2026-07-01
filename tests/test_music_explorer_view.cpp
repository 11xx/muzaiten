#include "ui/MusicExplorerView.h"

#include <QApplication>
#include <QHeaderView>
#include <QtTest/QtTest>

#include <memory>

class MusicExplorerViewTest final : public QObject {
    Q_OBJECT

private slots:
    void firstShowUsesAlbumGridColumns();
    void albumKeysMoveByAlbumAndNFocusesTracks();
    void lNarrowsAndHUnnarrows();
    void arrowsStillMoveAlbumsHorizontally();
    void inlineTrackKeysScrollParentArea();
    void expandedTrackTableUsesNeutralPaletteWithoutArtwork();
    void expandedPanelSurvivesResizeAndRefocus();
    void inlineTrackKeysNavigateAndReturnFocus();
    void expandsExactlyOneAlbumAndShowsAllTracks();
    void mouseClickExpandsAndShowsVisibleTracks();
    void selectionMovesExpandedPanel();
    void switchingExpandedAlbumReusesCardsAndKeepsFocus();
    void expandAndCollapseReuseCards();
    void redundantSetAlbumsKeepsCards();
    void albumDoubleClickDoesNotPlay();
    void currentCardUsesFullActiveHighlight();
    void albumActionsForwardSignals();

private:
    static QVector<Album> makeAlbums();
    static QVector<Track> makeTracks(const QString &album, int count = -1);
};

QVector<Album> MusicExplorerViewTest::makeAlbums()
{
    return {
        {.title = QStringLiteral("One"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2001"), .trackCount = 2},
        {.title = QStringLiteral("Two"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2002"), .trackCount = 3},
        {.title = QStringLiteral("Three"), .albumArtistName = QStringLiteral("Artist"), .date = QStringLiteral("2003"), .trackCount = 1},
    };
}

QVector<Track> MusicExplorerViewTest::makeTracks(const QString &album, int count)
{
    if (count < 0) {
        count = album == QStringLiteral("Two") ? 3 : album == QStringLiteral("Three") ? 1 : 2;
    }
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

void MusicExplorerViewTest::lNarrowsAndHUnnarrows()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QTest::keyClick(&view, Qt::Key_L);
    QTRY_COMPARE(view.expandedPanelCountForTests(), 1);
    QVERIFY(view.inlineTrackTableForTests()->hasFocus());

    QTest::keyClick(view.inlineTrackTableForTests(), Qt::Key_H);
    QTRY_COMPARE(view.expandedPanelCountForTests(), 0);
    QVERIFY(view.hasFocus());
}

void MusicExplorerViewTest::inlineTrackKeysScrollParentArea()
{
    MusicExplorerView view;
    view.resize(520, 320);
    view.setNavigationScrollPadding(2);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title, 36); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("One"), true);
    QTRY_COMPARE(view.expandedPanelCountForTests(), 1);
    auto *table = view.inlineTrackTableForTests();
    QVERIFY(table != nullptr);
    QVERIFY(table->hasFocus());
    const int startScroll = view.scrollValueForTests();

    for (int i = 0; i < 30; ++i) {
        QTest::keyClick(table, Qt::Key_J);
    }

    QCOMPARE(table->currentRow(), 30);
    QVERIFY2(view.scrollValueForTests() > startScroll,
             qPrintable(QStringLiteral("scroll did not advance: start=%1 end=%2")
                            .arg(startScroll)
                            .arg(view.scrollValueForTests())));
}

void MusicExplorerViewTest::arrowsStillMoveAlbumsHorizontally()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QCOMPARE(view.currentRow(), 0);
    QTest::keyClick(&view, Qt::Key_Right);
    QCOMPARE(view.currentRow(), 1);
    QTest::keyClick(&view, Qt::Key_Left);
    QCOMPARE(view.currentRow(), 0);
}

void MusicExplorerViewTest::expandedTrackTableUsesNeutralPaletteWithoutArtwork()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("Two"), true);
    auto *table = view.inlineTrackTableForTests();
    QVERIFY(table != nullptr);
    const QPalette appPalette = QApplication::palette();
    const QPalette viewportPalette = table->viewport()->palette();
    QCOMPARE(viewportPalette.color(QPalette::Base).rgb(), appPalette.color(QPalette::Base).rgb());
    QVERIFY(viewportPalette.color(QPalette::Base).rgb() != appPalette.color(QPalette::Highlight).rgb());
    QVERIFY(viewportPalette.color(QPalette::Base) != viewportPalette.color(QPalette::AlternateBase));
    QVERIFY(table->horizontalHeader()->palette().color(QPalette::Button).rgb() != appPalette.color(QPalette::Highlight).rgb());
    // The header reuses the even-row zebra shade rather than a bespoke color.
    QCOMPARE(table->horizontalHeader()->palette().color(QPalette::Button).rgb(),
             viewportPalette.color(QPalette::AlternateBase).rgb());
}

void MusicExplorerViewTest::expandedPanelSurvivesResizeAndRefocus()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.selectAlbumTitle(QStringLiteral("Two"), true);
    QTRY_COMPARE(view.expandedPanelCountForTests(), 1);
    view.resize(760, 620);
    QCoreApplication::processEvents();
    view.clearFocus();
    QCoreApplication::processEvents();
    view.setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();

    QCOMPARE(view.expandedPanelCountForTests(), 1);
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
    QCOMPARE(view.expandedPanelCountForTests(), 0);
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

void MusicExplorerViewTest::switchingExpandedAlbumReusesCardsAndKeepsFocus()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    view.selectAlbumTitle(QStringLiteral("One"), true);
    QCOMPARE(view.expandedAlbumTitle(), QStringLiteral("One"));
    QVERIFY(view.inlineTrackTableForTests()->hasFocus());

    QVector<QWidget *> before;
    for (int row = 0; row < 3; ++row) {
        before.push_back(view.cardWidgetForTests(row));
    }

    view.selectAlbumTitle(QStringLiteral("Three"), false);
    QCOMPARE(view.expandedAlbumTitle(), QStringLiteral("Three"));
    QCOMPARE(view.expandedPanelCountForTests(), 1);

    // Switching the expanded album reuses the existing cards instead of tearing
    // the grid down, and leaves the tracklist's keyboard focus untouched.
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(view.cardWidgetForTests(row), before.at(row));
    }
    QVERIFY(view.inlineTrackTableForTests()->hasFocus());
}

void MusicExplorerViewTest::expandAndCollapseReuseCards()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    QVector<QWidget *> before;
    for (int row = 0; row < 3; ++row) {
        before.push_back(view.cardWidgetForTests(row));
    }

    // Expanding from the collapsed grid creates the panel in place, reusing the
    // existing cards (and their artwork) instead of rebuilding the grid.
    view.selectAlbumTitle(QStringLiteral("Two"), false);
    QCOMPARE(view.expandedPanelCountForTests(), 1);
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(view.cardWidgetForTests(row), before.at(row));
    }

    // Collapsing removes just the panel and keeps the same cards.
    view.collapseExpandedAlbum();
    QCOMPARE(view.expandedPanelCountForTests(), 0);
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(view.cardWidgetForTests(row), before.at(row));
    }
}

void MusicExplorerViewTest::redundantSetAlbumsKeepsCards()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    QVector<QWidget *> before;
    for (int row = 0; row < 3; ++row) {
        before.push_back(view.cardWidgetForTests(row));
    }

    // Re-applying an identical album set must not rebuild the grid (which would
    // clear artwork and flash every card).
    view.setAlbums(makeAlbums());
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(view.cardWidgetForTests(row), before.at(row));
    }
}

void MusicExplorerViewTest::albumDoubleClickDoesNotPlay()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    QWidget *card = view.cardWidgetForTests(1);
    QVERIFY(card != nullptr);
    QSignalSpy playSpy(&view, &MusicExplorerView::albumPlayReplaceRequested);
    // Double click must not replace the queue (matching the library album grid).
    QTest::mouseDClick(card, Qt::LeftButton, Qt::NoModifier, card->rect().center());
    QCOMPARE(playSpy.count(), 0);
}

void MusicExplorerViewTest::currentCardUsesFullActiveHighlight()
{
    MusicExplorerView view;
    view.resize(720, 640);
    view.setTrackProvider([](const Album &album) { return makeTracks(album.title); });
    view.setAlbums(makeAlbums());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.columnCountForTests() >= 3);

    // A plain selection (no expansion) is the "filled" case that must match the
    // library album grid's selected item rather than a dimmed translucent tint.
    view.setCurrentRow(1);
    QCOMPARE(view.expandedPanelCountForTests(), 0);
    // Marking the panel active drives isActiveMainPanel() and repaints the card.
    view.setProperty("mainPanelActive", true);

    QWidget *card = view.cardWidgetForTests(1);
    QVERIFY(card != nullptr);
    const QImage rendered = card->grab().toImage();
    // Sample the selection fill above the artwork (art starts at y=10).
    const QColor sampled = rendered.pixelColor(card->width() / 2, 5);
    QCOMPARE(sampled.rgb(), view.palette().color(QPalette::Highlight).rgb());
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
