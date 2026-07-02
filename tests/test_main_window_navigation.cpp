#include "app/AppCore.h"
#include "core/Artist.h"
#include "db/SettingsStore.h"
#include "player/PlayerCore.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/MusicExplorerView.h"
#include "ui/PanelSearchController.h"
#include "ui/PlaylistView.h"
#include "ui/SelectionColors.h"

#define private public
#include "ui/MainWindow.h"
#include "ui/PlayerBar.h"
#undef private

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest/QtTest>

class MainWindowNavigationTest final : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_stateRoot.isValid());
        qputenv("MUZAITEN_STATE_ROOT", m_stateRoot.path().toUtf8());
        qputenv("MUZAITEN_DEMO_SILENT_AUDIO", "1");
    }

    void cleanup()
    {
        qunsetenv("MUZAITEN_STATE_ROOT");
        qunsetenv("MUZAITEN_DEMO_SILENT_AUDIO");
    }

    void constructsAndNavigatesEveryMainView()
    {
        AppCore core;
        MainWindow window(&core);

        QVERIFY(window.m_queueScreen == nullptr);
        QVERIFY(window.m_searchView == nullptr);
        QVERIFY(window.m_libraryFileExplorer == nullptr);
        QVERIFY(window.m_freeRoamFileExplorer == nullptr);
        QVERIFY(window.m_musicExplorerView == nullptr);
        QVERIFY(window.m_playlistView == nullptr);

        window.persistViewState();
        window.switchMainView(MainView::LibraryPanels);
        window.switchMainView(MainView::LibraryMusicExplorer);
        window.switchMainView(MainView::LibraryFileExplorer);
        window.switchMainView(MainView::FreeRoamFileExplorer);
        window.switchMainView(MainView::Search);
        window.switchMainView(MainView::Queue);
        window.switchMainView(MainView::Playlist);
        window.saveAllViewSettings();
        window.resetViewPreferences();
        window.persistViewState();

        QVERIFY(window.m_rootSplitter != nullptr);
        QVERIFY(window.m_libraryFileExplorer != nullptr);
        QVERIFY(window.m_freeRoamFileExplorer != nullptr);
        QVERIFY(window.m_musicExplorerView != nullptr);
        QVERIFY(window.m_searchView != nullptr);
        QVERIFY(window.m_queueScreen != nullptr);
        QVERIFY(window.m_playlistView != nullptr);
    }

    void playerBarGraysVolumeWhenControlIsDisabled()
    {
        PlayerBar bar;
        QVERIFY(bar.m_volumeButton != nullptr);
        QVERIFY(bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume"));

        bar.setVolumeControlEnabled(false);
        QVERIFY(!bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume disabled by the output profile"));

        bar.setVolumeControlEnabled(true);
        QVERIFY(bar.m_volumeButton->isEnabled());
        QCOMPARE(bar.m_volumeButton->toolTip(), QStringLiteral("Volume"));
    }

    void savedQueueLimitsDefaultToFifteenUnlessUnlimited()
    {
        AppCore core;
        MainWindow window(&core);

        QCOMPARE(window.savedQueueLimitSetting(), 15);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 15);
        QVERIFY(!window.savedQueueUnlimitedSetting());
        QVERIFY(!window.radioSavedQueueUnlimitedSetting());

        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("1"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("true"));
        QCOMPARE(window.savedQueueLimitSetting(), 0);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 0);
        QVERIFY(window.savedQueueUnlimitedSetting());
        QVERIFY(window.radioSavedQueueUnlimitedSetting());

        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("0"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("0"));
        QCOMPARE(window.savedQueueLimitSetting(), 15);
        QCOMPARE(window.radioSavedQueueLimitSetting(), 15);
    }

    void radioSnapshotsAreTaggedAndLabeled()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track track;
        track.path = QStringLiteral("/music/radio-seed.flac");
        track.title = QStringLiteral("Radio Seed");
        track.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({track}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:radio-test"));

        window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(root.value(QStringLiteral("backlog")).toArray().size(), 0);
        QCOMPARE(radioBacklog.size(), 1);
        const QJsonObject snapshot = radioBacklog.at(0).toObject();
        QCOMPARE(snapshot.value(QStringLiteral("source")).toString(), QStringLiteral("radio"));
        QCOMPARE(snapshot.value(QStringLiteral("name")).toString(), QString());

        const QString timestamp = QDateTime::fromSecsSinceEpoch(snapshot.value(QStringLiteral("savedAt")).toVariant().toLongLong())
                                      .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
        const QVector<SavedQueuePlaylistEntry> entries = window.savedQueuePlaylistEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).name, QStringLiteral("Radio session %1").arg(timestamp));
    }

    void radioSnapshotsUseSeparateFifteenEntryBucket()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));
        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("0"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("0"));

        for (int i = 0; i < 17; ++i) {
            Track track;
            track.path = QStringLiteral("/music/regular-%1.flac").arg(i);
            track.title = QStringLiteral("Regular %1").arg(i);
            track.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({track}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:regular-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious();
        }
        for (int i = 0; i < 17; ++i) {
            Track track;
            track.path = QStringLiteral("/music/radio-%1.flac").arg(i);
            track.title = QStringLiteral("Radio %1").arg(i);
            track.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({track}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:radio-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
        }

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray backlog = root.value(QStringLiteral("backlog")).toArray();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(backlog.size(), 15);
        QCOMPARE(radioBacklog.size(), 15);
        QCOMPARE(backlog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:regular-16"));
        QCOMPARE(backlog.at(14).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:regular-2"));
        QCOMPARE(radioBacklog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:radio-16"));
        QCOMPARE(radioBacklog.at(14).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:radio-2"));
        for (const QJsonValue &value : radioBacklog) {
            QCOMPARE(value.toObject().value(QStringLiteral("source")).toString(), QStringLiteral("radio"));
        }
    }

    void unlimitedSavedQueueTogglesDisableTrimming()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));
        window.m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), QStringLiteral("1"));
        window.m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), QStringLiteral("1"));

        for (int i = 0; i < 17; ++i) {
            Track regularTrack;
            regularTrack.path = QStringLiteral("/music/unlimited-regular-%1.flac").arg(i);
            regularTrack.title = QStringLiteral("Unlimited Regular %1").arg(i);
            regularTrack.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({regularTrack}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:unlimited-regular-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious();

            Track radioTrack;
            radioTrack.path = QStringLiteral("/music/unlimited-radio-%1.flac").arg(i);
            radioTrack.title = QStringLiteral("Unlimited Radio %1").arg(i);
            radioTrack.artistName = QStringLiteral("Artist");
            window.m_player->resetQueue({radioTrack}, 0);
            window.markQueueAsSpontaneous(QStringLiteral("queue:unlimited-radio-%1").arg(i));
            window.snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
        }

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        QCOMPARE(root.value(QStringLiteral("backlog")).toArray().size(), 17);
        QCOMPARE(root.value(QStringLiteral("radioBacklog")).toArray().size(), 17);
    }

    void legacyRadioSnapshotsMoveOutOfRegularBacklog()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track radioTrack;
        radioTrack.path = QStringLiteral("/music/legacy-radio.flac");
        radioTrack.title = QStringLiteral("Legacy Radio");
        radioTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({radioTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-radio"));
        const QJsonObject legacyRadio = window.queueSnapshotObject(QString(), QStringLiteral("radio"));

        Track regularTrack;
        regularTrack.path = QStringLiteral("/music/legacy-regular.flac");
        regularTrack.title = QStringLiteral("Legacy Regular");
        regularTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({regularTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-regular"));
        const QJsonObject legacyRegular = window.queueSnapshotObject(QString());

        QJsonArray legacyBacklog;
        legacyBacklog.append(legacyRadio);
        legacyBacklog.append(legacyRegular);
        QJsonObject legacyRoot;
        legacyRoot.insert(QStringLiteral("backlog"), legacyBacklog);
        window.saveQueueSnapshotsRoot(legacyRoot);

        Track newRegularTrack;
        newRegularTrack.path = QStringLiteral("/music/new-regular.flac");
        newRegularTrack.title = QStringLiteral("New Regular");
        newRegularTrack.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({newRegularTrack}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:new-regular"));
        window.snapshotCurrentQueueAsPrevious();

        const QJsonObject root = window.loadQueueSnapshotsRoot();
        const QJsonArray backlog = root.value(QStringLiteral("backlog")).toArray();
        const QJsonArray radioBacklog = root.value(QStringLiteral("radioBacklog")).toArray();
        QCOMPARE(backlog.size(), 2);
        QCOMPARE(radioBacklog.size(), 1);
        QCOMPARE(backlog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:new-regular"));
        QCOMPARE(backlog.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:legacy-regular"));
        QCOMPARE(radioBacklog.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("queue:legacy-radio"));
    }

    void legacySnapshotsKeepFallbackLabel()
    {
        AppCore core;
        MainWindow window(&core);
        window.m_state->removeSetting(QStringLiteral("queue.snapshots"));

        Track track;
        track.path = QStringLiteral("/music/legacy.flac");
        track.title = QStringLiteral("Legacy");
        track.artistName = QStringLiteral("Artist");
        window.m_player->resetQueue({track}, 0);
        window.markQueueAsSpontaneous(QStringLiteral("queue:legacy-test"));

        QJsonObject snapshot = window.queueSnapshotObject(QString());
        snapshot.insert(QStringLiteral("id"), QStringLiteral("queue:legacy-test"));
        snapshot.insert(QStringLiteral("savedAt"), 1'234'567'890);
        QJsonArray backlog;
        backlog.append(snapshot);
        QJsonObject root;
        root.insert(QStringLiteral("backlog"), backlog);
        window.saveQueueSnapshotsRoot(root);

        const QVector<SavedQueuePlaylistEntry> entries = window.savedQueuePlaylistEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).name, QStringLiteral("saved queue 1"));
    }

    void musicExplorerKeepsMainPanelNavigationActive()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.m_artistSidebar->setArtists({
            {.name = QStringLiteral("Artist One"), .albumCount = 1},
            {.name = QStringLiteral("Artist Two"), .albumCount = 1},
        });
        window.switchMainView(MainView::LibraryMusicExplorer);
        QVERIFY(window.m_musicExplorerView != nullptr);

        window.m_panelSearch->setActivePanel(MainPanelId::Artists, true);
        auto *artistList = window.m_artistSidebar->navigationWidget();
        QVERIFY(artistList->hasFocus());
        QCOMPARE(window.m_artistSidebar->currentRow(), 0);

        QTest::keyClick(artistList, Qt::Key_J);
        QCOMPARE(window.m_artistSidebar->currentRow(), 1);

        QTest::keyClick(artistList, Qt::Key_L);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Albums);
        QVERIFY(window.m_musicExplorerView->hasFocus());

        QTest::keyClick(window.m_musicExplorerView, Qt::Key_H);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Artists);
        QVERIFY(artistList->hasFocus());
    }

    void musicExplorerHCollapsesTracksBeforeMovingPanels()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.switchMainView(MainView::LibraryMusicExplorer);
        QVERIFY(window.m_musicExplorerView != nullptr);
        window.m_musicExplorerView->setTrackProvider([](const Album &album) {
            QVector<Track> tracks;
            for (int i = 0; i < 3; ++i) {
                Track track;
                track.path = QStringLiteral("/music/%1/%2.flac").arg(album.title).arg(i + 1);
                track.title = QStringLiteral("Track %1").arg(i + 1);
                track.artistName = QStringLiteral("Artist");
                track.albumArtistName = QStringLiteral("Artist");
                track.albumTitle = album.title;
                track.trackNumber = i + 1;
                tracks.push_back(track);
            }
            return tracks;
        });
        window.m_musicExplorerView->setAlbums({
            {.title = QStringLiteral("One"), .albumArtistName = QStringLiteral("Artist"), .trackCount = 3},
            {.title = QStringLiteral("Two"), .albumArtistName = QStringLiteral("Artist"), .trackCount = 3},
        });

        window.m_panelSearch->setActivePanel(MainPanelId::Albums, true);
        QVERIFY(window.m_musicExplorerView->hasFocus());
        QTest::keyClick(window.m_musicExplorerView, Qt::Key_L);
        QTRY_COMPARE(window.m_panelSearch->activePanel(), MainPanelId::Tracks);
        QTRY_COMPARE(window.m_musicExplorerView->expandedPanelCountForTests(), 1);
        auto *trackTable = window.m_musicExplorerView->trackNavigationWidget();
        QVERIFY(trackTable->hasFocus());

        QTest::keyClick(trackTable, Qt::Key_H);
        QTRY_COMPARE(window.m_panelSearch->activePanel(), MainPanelId::Albums);
        QCOMPARE(window.m_musicExplorerView->expandedPanelCountForTests(), 0);
        QVERIFY(window.m_musicExplorerView->hasFocus());

        QTest::keyClick(window.m_musicExplorerView, Qt::Key_H);
        QCOMPARE(window.m_panelSearch->activePanel(), MainPanelId::Artists);
        QVERIFY(window.m_artistSidebar->navigationWidget()->hasFocus());
    }

    void focusedLibraryAlbumGridUsesActiveSelectionColors()
    {
        AppCore core;
        MainWindow window(&core);
        window.resize(900, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.switchMainView(MainView::LibraryMusicExplorer);
        window.switchMainView(MainView::LibraryPanels);
        QVERIFY(window.m_albumGrid != nullptr);

        window.m_panelSearch->setActivePanel(MainPanelId::Albums, true);
        QTRY_VERIFY(window.m_albumGrid->hasFocus());
        window.m_albumGrid->viewport()->setProperty("mainPanelActive", false);

        QVERIFY(SelectionColors::isActiveMainPanel(window.m_albumGrid->viewport()));
    }

private:
    QTemporaryDir m_stateRoot;
};

QTEST_MAIN(MainWindowNavigationTest)
#include "test_main_window_navigation.moc"
