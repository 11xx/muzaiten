#include "app/AppCore.h"

#define private public
#include "ui/MainWindow.h"
#undef private

#include <QTemporaryDir>
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

        window.persistViewState();
        window.switchMainView(MainView::LibraryPanels);
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
        QVERIFY(window.m_searchView != nullptr);
        QVERIFY(window.m_queueScreen != nullptr);
        QVERIFY(window.m_playlistView != nullptr);
    }

private:
    QTemporaryDir m_stateRoot;
};

QTEST_MAIN(MainWindowNavigationTest)
#include "test_main_window_navigation.moc"
