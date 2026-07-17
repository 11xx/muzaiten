#include "ui/ViewStatePersistence.h"
#include "ui/MainWindow.h"
#include "db/SettingsStore.h"
#include "player/PlayerCore.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/MusicExplorerView.h"
#include "ui/PanelSearchController.h"
#include "ui/PlayerBar.h"
#include "ui/PlaylistView.h"
#include "ui/QueueScreen.h"
#include "ui/RightSidebar.h"
#include "ui/SplitterPersistence.h"
#include "ui/TableNavigationScroll.h"
#include "ui/TrackTable.h"
#include <QDir>
#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QSplitter>
#include <algorithm>

namespace {
constexpr int kRootSplitterMinimumTotal = 800;
constexpr int kCenterSplitterMinimumTotal = 300;
constexpr int kArtistSidebarMinimumWidth = 180;
constexpr int kCenterPaneMinimumWidth = 500;
constexpr int kRightSidebarMinimumWidth = 220;
constexpr int kPanelMinimumHeight = 140;
QString mainViewName(MainView view)
{
    switch (view) {
    case MainView::LibraryPanels: return QStringLiteral("libraryPanels");
    case MainView::LibraryMusicExplorer: return QStringLiteral("libraryMusicExplorer");
    case MainView::LibraryFileExplorer: return QStringLiteral("libraryFileExplorer");
    case MainView::FreeRoamFileExplorer: return QStringLiteral("freeRoamFileExplorer");
    case MainView::Search: return QStringLiteral("search");
    case MainView::Queue: return QStringLiteral("queue");
    case MainView::Playlist: return QStringLiteral("playlist");
    }
    return QStringLiteral("libraryPanels");
}
MainView mainViewFromName(const QString &name)
{
    if (name == QStringLiteral("libraryMusicExplorer")) return MainView::LibraryMusicExplorer;
    if (name == QStringLiteral("libraryFileExplorer")) return MainView::LibraryFileExplorer;
    if (name == QStringLiteral("freeRoamFileExplorer")) return MainView::FreeRoamFileExplorer;
    if (name == QStringLiteral("search")) return MainView::Search;
    if (name == QStringLiteral("queue")) return MainView::Queue;
    if (name == QStringLiteral("playlist")) return MainView::Playlist;
    return MainView::LibraryPanels;
}
}

ViewStatePersistence::ViewStatePersistence(MainWindow &window) : QObject(&window), m_window(window) {}

void ViewStatePersistence::loadViewSettings()
{
    m_window.m_loadingViewSettings = true;
    m_window.m_trackTable->applyViewSettingsJson(m_window.m_state->setting(QStringLiteral("trackTable.view")));
    if (m_window.m_musicExplorerView != nullptr) {
        m_window.m_musicExplorerView->applyAlbumGridViewSettingsJson(m_window.m_state->setting(QStringLiteral("albumGrid.view")));
        m_window.m_musicExplorerView->applyTrackTableViewSettingsJson(m_window.m_state->setting(QStringLiteral("trackTable.view")));
    }
    const QString rightSidebarSettings = m_window.m_state->setting(QStringLiteral("rightSidebar.view"));
    m_window.m_rightSidebar->applyViewSettingsJson(rightSidebarSettings);
    m_window.m_playerBar->setTrackInfoPaneVisible(QJsonDocument::fromJson(rightSidebarSettings.toUtf8()).object().value(QStringLiteral("showTrackInfo")).toBool(true));
    const QJsonObject playerBar = QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("playerBar.view")).toUtf8()).object();
    m_window.m_playerBar->setCompactMenu(playerBar.value(QStringLiteral("compactMenu")).toBool(false));
    m_window.m_playerBar->setAlwaysShowTray(m_window.m_state->setting(QStringLiteral("tray.alwaysVisible"), QStringLiteral("false")) == QStringLiteral("true"));

    const int volume = std::clamp(m_window.m_state->setting(QStringLiteral("volume"), QStringLiteral("100")).toInt(), 0, 100);
    m_window.m_player->setVolume(static_cast<double>(volume) / 100.0);
    m_window.m_playerBar->setVolume(volume);
    m_window.m_albumGrid->applyViewSettingsJson(m_window.m_state->setting(QStringLiteral("albumGrid.view")));
    m_window.m_artistSidebar->applyViewSettingsJson(m_window.m_state->setting(QStringLiteral("artistSidebar.view")));
    const QJsonObject mainWindow = QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
    const QByteArray geometry = QByteArray::fromBase64(mainWindow.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geometry.isEmpty()) {
        m_window.restoreGeometry(geometry);
    }
    SplitterPersistence::restoreSplitterIfStable(m_window.m_rootSplitter,
                            mainWindow.value(QStringLiteral("rootSplitter")).toArray(),
                            {kArtistSidebarMinimumWidth, kCenterPaneMinimumWidth, kRightSidebarMinimumWidth},
                            kRootSplitterMinimumTotal);
    SplitterPersistence::restoreSplitterIfStable(m_window.m_centerSplitter,
                            mainWindow.value(QStringLiteral("centerSplitter")).toArray(),
                            {kPanelMinimumHeight, kPanelMinimumHeight},
                            kCenterSplitterMinimumTotal);
    m_window.m_mainView = mainViewFromName(mainWindow.value(QStringLiteral("mainView")).toString());
    m_window.m_libraryExplorerDirectory = mainWindow.value(QStringLiteral("libraryExplorerDirectory")).toString();
    m_window.m_freeRoamDirectory = mainWindow.value(QStringLiteral("freeRoamDirectory")).toString(QDir::homePath());

    const bool showUnsupported = m_window.m_state->setting(QStringLiteral("fileExplorer.showUnsupported")) == QStringLiteral("true");
    m_window.m_playerBar->setListUnsupportedFiles(showUnsupported);

    if (m_window.m_panelSearch != nullptr) {
        m_window.m_panelSearch->setKeyBindingProfileName(m_window.m_state->setting(QStringLiteral("mainPanel.keyBindingProfile"),
                                                                 defaultMainPanelKeyBindingProfileName()));
        const QJsonArray focusOrder = QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("mainPanel.focusOrder")).toUtf8()).array();
        m_window.m_panelSearch->setFocusOrder(mainPanelFocusOrderFromJson(focusOrder));
        m_window.m_panelSearch->setActivePanelFromString(m_window.m_state->setting(QStringLiteral("mainPanel.activePanel")));
    }
    const int mainPanelScrollPadding = std::clamp(m_window.m_state->setting(QStringLiteral("mainPanel.scrollPadding"),
                                                                   QString::number(TableNavigationScroll::kDefaultPaddingRows)).toInt(),
                                                  0, 20);
    m_window.m_rightSidebar->setNavigationScrollPadding(mainPanelScrollPadding);
    m_window.m_artistSidebar->setNavigationScrollPadding(mainPanelScrollPadding);
    m_window.m_trackTable->setNavigationScrollPadding(mainPanelScrollPadding);
    if (m_window.m_musicExplorerView != nullptr) {
        m_window.m_musicExplorerView->setNavigationScrollPadding(mainPanelScrollPadding);
    }

    m_window.switchMainView(m_window.m_mainView);
    m_window.applySharedTableSettings();
    m_window.m_loadingViewSettings = false;
}

void ViewStatePersistence::saveTrackTableViewSettings()
{
    if (m_window.m_loadingViewSettings || m_window.m_applyingTrackTableViewSettings) {
        return;
    }

    QObject *source = m_window.sender();
    const QString settings = source == m_window.m_musicExplorerView && m_window.m_musicExplorerView != nullptr
        ? m_window.m_musicExplorerView->trackTableViewSettingsJson()
        : m_window.m_trackTable->viewSettingsJson();
    m_window.m_state->setSetting(QStringLiteral("trackTable.view"), settings);

    QScopedValueRollback<bool> applying(m_window.m_applyingTrackTableViewSettings, true);
    if (source == m_window.m_musicExplorerView) {
        m_window.m_trackTable->applyViewSettingsJson(settings);
    } else if (m_window.m_musicExplorerView != nullptr) {
        m_window.m_musicExplorerView->applyTrackTableViewSettingsJson(settings);
    }
    m_window.applySharedTableSettings();
}

void ViewStatePersistence::saveAlbumGridViewSettings()
{
    const QString settings = m_window.m_albumGrid->viewSettingsJson();
    m_window.m_state->setSetting(QStringLiteral("albumGrid.view"), settings);
    if (m_window.m_musicExplorerView != nullptr) {
        m_window.m_musicExplorerView->applyAlbumGridViewSettingsJson(settings);
    }
}

void ViewStatePersistence::saveMusicExplorerAlbumGridViewSettings()
{
    if (m_window.m_musicExplorerView == nullptr) {
        return;
    }
    const QString settings = m_window.m_musicExplorerView->albumGridViewSettingsJson();
    m_window.m_state->setSetting(QStringLiteral("albumGrid.view"), settings);
    m_window.m_albumGrid->applyViewSettingsJson(settings);
}

void ViewStatePersistence::saveArtistSidebarViewSettings()
{
    m_window.m_state->setSetting(QStringLiteral("artistSidebar.view"), m_window.m_artistSidebar->viewSettingsJson());
}

void ViewStatePersistence::saveRightSidebarViewSettings()
{
    if (m_window.m_loadingViewSettings) {
        return;
    }
    m_window.m_state->setSetting(QStringLiteral("rightSidebar.view"), m_window.m_rightSidebar->viewSettingsJson());
    m_window.applySharedTableSettings();
}

void ViewStatePersistence::saveQueueScreenViewSettings()
{
    if (m_window.m_queueScreen == nullptr) {
        return;
    }
    m_window.m_state->setSetting(QStringLiteral("queueScreen.view"), m_window.m_queueScreen->viewSettingsJson());
}

void ViewStatePersistence::savePlaylistViewSettings()
{
    if (m_window.m_playlistView == nullptr) {
        return;
    }
    m_window.m_state->setSetting(QStringLiteral("playlistView.view"), m_window.m_playlistView->viewSettingsJson());
    m_window.applySharedTableSettings();
}

void ViewStatePersistence::saveMainWindowViewSettings(bool captureSplitterSizes)
{
    if (m_window.m_loadingViewSettings) {
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
    root.insert(QStringLiteral("geometry"), QString::fromLatin1(m_window.saveGeometry().toBase64()));
    if (captureSplitterSizes) {
        const QList<int> rootSizes = m_window.m_rootSplitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(rootSizes,
                                   {kArtistSidebarMinimumWidth, kCenterPaneMinimumWidth, kRightSidebarMinimumWidth},
                                   kRootSplitterMinimumTotal)) {
            root.insert(QStringLiteral("rootSplitter"), SplitterPersistence::splitterSizesToJson(rootSizes));
        }
        const QList<int> centerSizes = m_window.m_centerSplitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(centerSizes,
                                   {kPanelMinimumHeight, kPanelMinimumHeight},
                                   kCenterSplitterMinimumTotal)) {
            root.insert(QStringLiteral("centerSplitter"), SplitterPersistence::splitterSizesToJson(centerSizes));
        }
    }
    root.insert(QStringLiteral("mainView"), mainViewName(m_window.m_mainView));
    root.insert(QStringLiteral("libraryExplorerDirectory"), m_window.m_libraryExplorerDirectory);
    root.insert(QStringLiteral("freeRoamDirectory"), m_window.m_freeRoamDirectory);
    if (m_window.m_panelSearch != nullptr) {
        root.insert(QStringLiteral("activePanel"), mainPanelIdToString(m_window.m_panelSearch->activePanel()));
    }
    m_window.m_state->setSetting(QStringLiteral("mainWindow.view"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    if (m_window.m_panelSearch != nullptr) {
        m_window.m_state->setSetting(QStringLiteral("mainPanel.keyBindingProfile"), m_window.m_panelSearch->keyBindingProfileName());
        m_window.m_state->setSetting(QStringLiteral("mainPanel.focusOrder"),
                            QString::fromUtf8(QJsonDocument(mainPanelFocusOrderToJson(m_window.m_panelSearch->focusOrder())).toJson(QJsonDocument::Compact)));
        m_window.m_state->setSetting(QStringLiteral("mainPanel.activePanel"), mainPanelIdToString(m_window.m_panelSearch->activePanel()));
    }
}

void ViewStatePersistence::saveAllViewSettings()
{
    saveTrackTableViewSettings();
    saveAlbumGridViewSettings();
    saveArtistSidebarViewSettings();
    saveRightSidebarViewSettings();
    if (m_window.m_queueScreen != nullptr) {
        saveQueueScreenViewSettings();
    }
    if (m_window.m_playlistView != nullptr) {
        savePlaylistViewSettings();
    }
    saveMainWindowViewSettings();
}
