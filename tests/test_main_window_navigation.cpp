#include "app/AppCore.h"
#include "core/Artist.h"
#include "ui/ArtistSidebar.h"
#include "ui/MusicExplorerView.h"
#include "ui/PanelSearchController.h"

#define private public
#include "ui/MainWindow.h"
#include "ui/PlayerBar.h"
#undef private

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

private:
    QTemporaryDir m_stateRoot;
};

QTEST_MAIN(MainWindowNavigationTest)
#include "test_main_window_navigation.moc"
