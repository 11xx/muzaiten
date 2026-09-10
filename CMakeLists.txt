cmake_minimum_required(VERSION 4.0)

include(cmake/MuzaitenVersion.cmake)

project(muzaiten
    VERSION ${MUZAITEN_PROJECT_VERSION}
    DESCRIPTION "Native Linux music player inspired by MusicBee and ncmpcpp"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Embedded default Last.fm credentials, injected at build time so secrets
# never live in the source tree. Distributed builds pass real values with
# -DMUZAITEN_LASTFM_API_KEY=... -DMUZAITEN_LASTFM_SHARED_SECRET=...; an empty
# default leaves Last.fm inactive until the user configures their own.
set(MUZAITEN_LASTFM_API_KEY "" CACHE STRING "Embedded default Last.fm API key")
set(MUZAITEN_LASTFM_SHARED_SECRET "" CACHE STRING "Embedded default Last.fm shared secret")

include(CheckPIESupported)
check_pie_supported()

find_package(Qt6 6.11 REQUIRED COMPONENTS Concurrent Core DBus Gui Multimedia Network Widgets Sql Test)
find_package(PkgConfig REQUIRED)

pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(Chromaprint REQUIRED IMPORTED_TARGET libchromaprint)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-audio-1.0
    gstreamer-pbutils-1.0
    glib-2.0
    gobject-2.0
)

# Object files carry the ABI of the compiler and the Qt headers they saw, but a
# build tool decides what to recompile by comparing timestamps, and a package
# manager restores headers with their upstream build time. Upgrade Qt or the
# compiler and the new headers can be *older* than the objects built from the
# previous ones, so nothing recompiles, the stale objects link against the new
# libraries, and the result crashes inside untouched code. Recording the
# toolchain each configure turns that into a detectable fact: the marker file
# tells the build wrapper to recompile from scratch, and CMake reruns here
# whenever it reconfigures. The packaged dependencies are part of the identity
# because their headers carry the same timestamp hazard. Full version strings
# are deliberate: a spurious from-scratch build costs minutes, a stale-object
# crash costs a debugging session. A multi-module pkg-config lookup exposes each
# module's version as <PREFIX>_<module>_VERSION.
string(CONCAT MUZAITEN_TOOLCHAIN_ID
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-Qt${Qt6_VERSION}"
    "-TagLib${TAGLIB_VERSION}"
    "-zstd${ZSTD_VERSION}"
    "-Chromaprint${Chromaprint_VERSION}"
    "-GStreamer${GSTREAMER_gstreamer-1.0_VERSION}"
    "-GStreamerAudio${GSTREAMER_gstreamer-audio-1.0_VERSION}"
    "-GStreamerPbutils${GSTREAMER_gstreamer-pbutils-1.0_VERSION}"
    "-GLib${GSTREAMER_glib-2.0_VERSION}"
    "-GObject${GSTREAMER_gobject-2.0_VERSION}")
set(MUZAITEN_TOOLCHAIN_MARKER "${CMAKE_BINARY_DIR}/toolchain-changed.stamp")
if(DEFINED CACHE{MUZAITEN_TOOLCHAIN_CONFIGURED}
   AND NOT "$CACHE{MUZAITEN_TOOLCHAIN_CONFIGURED}" STREQUAL "${MUZAITEN_TOOLCHAIN_ID}")
    message(STATUS
        "Toolchain changed ($CACHE{MUZAITEN_TOOLCHAIN_CONFIGURED} -> ${MUZAITEN_TOOLCHAIN_ID}); "
        "objects in ${CMAKE_BINARY_DIR} are stale and will be rebuilt from scratch.")
    file(WRITE "${MUZAITEN_TOOLCHAIN_MARKER}" "${MUZAITEN_TOOLCHAIN_ID}\n")
endif()
set(MUZAITEN_TOOLCHAIN_CONFIGURED "${MUZAITEN_TOOLCHAIN_ID}" CACHE INTERNAL
    "Compiler, Qt, and packaged dependency versions the objects in this build directory were compiled against")

configure_file(
    src/core/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h
    @ONLY
)

set(MUZAITEN_SOURCES
    src/app/main.cpp
    src/app/AppCore.cpp
    src/app/StartupStorage.cpp
    src/app/AppPaths.cpp
    src/app/DemoScreens.cpp
    src/app/MuzaitenApplication.cpp
    src/core/GenreTags.cpp
    src/core/HumanQuantity.cpp
    src/core/MetadataBlob.cpp
    src/core/MusicSort.cpp
    src/core/Rating.cpp
    src/core/TrackDisplay.cpp
    src/db/Database.cpp
    src/db/PlaylistDatabase.cpp
    src/db/SettingsStore.cpp
    src/db/SqlUtil.cpp
    src/features/FeatureStore.cpp
    src/features/QualityRank.cpp
    src/features/QueryVectorCache.cpp
    src/features/SongIdentity.cpp
    src/fs/MediaProbe.cpp
    src/fs/PathResolver.cpp
    src/mpd/MpdConfig.cpp
    src/mpd/MpdClient.cpp
    src/mpd/MpdImportWorker.cpp
    src/ipc/IpcServer.cpp
    src/ipc/IpcSocket.cpp
    src/mpris/MprisService.cpp
    src/player/PlayerCore.cpp
    src/reco/AffinityPool.cpp
    src/reco/ArtistRadio.cpp
    src/reco/RadioFilters.cpp
    src/reco/RadioMix.cpp
    src/reco/RadioProfile.cpp
    src/reco/RadioSession.cpp
    src/reco/ReasonText.cpp
    src/reco/GenreCuration.cpp
    src/reco/TrackScorer.cpp
    src/reco/WeightLearner.cpp
    src/reco/WeightLearnerData.cpp
    src/playback/AudioDeviceControl.cpp
    src/playback/GStreamerPlaybackBackend.cpp
    src/playback/QtPlaybackBackend.cpp
    src/scanner/DirectoryWalker.cpp
    src/scanner/PathMetadataGuesser.cpp
    src/scanner/ScanPipeline.cpp
    src/scanner/ArtworkCache.cpp
    src/scanner/LibraryScanner.cpp
    src/scanner/RatingTagSyncWorker.cpp
    src/scanner/TagRating.cpp
    src/scanner/TagReader.cpp
    src/scanner/TagRatingWriter.cpp
    src/scrobble/BackfillParse.cpp
    src/scrobble/LastFmApi.cpp
    src/scrobble/LastFmScrobbler.cpp
    src/scrobble/ListenBrainzHub.cpp
    src/scrobble/ListenBrainzScrobbler.cpp
    src/scrobble/ListenBrainzUrl.cpp
    src/scrobble/ListenHistoryStore.cpp
    src/scrobble/ListenTracker.cpp
    src/scrobble/PlayEventRecorder.cpp
    src/scrobble/ScrobbleBackfill.cpp
    src/scrobble/ScrobbleDestination.cpp
    src/search/Exclusion.cpp
    src/search/fold/Fold.cpp
    src/search/FuzzyMatch.cpp
    src/search/IndexCache.cpp
    src/search/RankConfig.cpp
    src/search/ResultRanker.cpp
    src/search/SearchIndex.cpp
    src/search/SearchMatcher.cpp
    src/search/SearchQuery.cpp
    src/search/SearchWorker.cpp
    src/ui/AlbumGrid.cpp
    src/ui/AlbumArtView.cpp
    src/ui/AudioAnalysisData.cpp
    src/ui/AudioAnalysisDialogs.cpp
    src/ui/ConditionEditor.cpp
    src/ui/NeighborColumnResizer.cpp
    src/ui/ResponsiveColumnLayout.cpp
    src/ui/ResponsiveColumnOptionsDialog.cpp
    src/ui/OverlayScrollBar.cpp
    src/ui/AlbumGridDelegate.cpp
    src/ui/AlbumArtFallback.cpp
    src/ui/ArtistSidebar.cpp
    src/ui/DenseTableDelegate.cpp
    src/ui/FileExplorerView.cpp
    src/ui/FileExplorerKeybindings.cpp
    src/ui/IdleReleaseController.cpp
    src/ui/KeybindingsDialog.cpp
    src/ui/GenreCurationDialog.cpp
    src/ui/LinkRootsDialog.cpp
    src/ui/ListeningHistoryPanel.cpp
    src/ui/MainWindow.cpp
    src/ui/ScanController.cpp
    src/ui/ScrobblersPanel.cpp
    src/ui/ScrobblingDialog.cpp
    src/ui/MenuHighlightStyle.cpp
    src/ui/StickyMenu.cpp
    src/ui/ToggleSwitch.cpp
    src/ui/ScrobbleUploadDispatcher.cpp
    src/ui/RatingSyncController.cpp
    src/ui/ViewStatePersistence.cpp
    src/ui/QueueSnapshotStore.cpp
    src/ui/MusicExplorerView.cpp
    src/playlist/PlaylistImport.cpp
    src/playlist/PlaylistImportWorker.cpp
    src/playlist/PlaylistDropImportWorker.cpp
    src/playlist/PlaylistMatchIndex.cpp
    src/playlist/PlaylistMatcher.cpp
    src/playlist/import/YouTubeImport.cpp
    src/ui/MainPanelKeybindings.cpp
    src/ui/NavigableTableView.cpp
    src/ui/PanelOrderDialog.cpp
    src/ui/PlaybackProfileDialog.cpp
    src/ui/PlaybackResumeDialog.cpp
    src/ui/StopAfterDialog.cpp
    src/ui/PanelSearchBar.cpp
    src/ui/PanelSearchController.cpp
    src/ui/PlayerBar.cpp
    src/ui/RadioCustomizationDialog.cpp
    src/ui/PlaylistAddDialog.cpp
    src/ui/PlaylistImportDialog.cpp
    src/ui/PlaylistView.cpp
    src/ui/QueueKeybindings.cpp
    src/ui/QueueScreen.cpp
    src/ui/QueueStore.cpp
    src/ui/QueueTable.cpp
    src/ui/RankingDialog.cpp
    src/ui/ReorderableTableWidget.cpp
    src/ui/RowReorderSupport.cpp
    src/ui/RightSidebar.cpp
    src/ui/SettingsDialogSupport.cpp
    src/ui/SearchResultDelegate.cpp
    src/ui/SearchResultsModel.cpp
    src/ui/SearchView.cpp
    src/ui/SemanticSearchData.cpp
    src/ui/SemanticSearchDialog.cpp
    src/features/QueryEmbedding.cpp
    src/ui/SourceDirectoriesDialog.cpp
    src/ui/StarRating.cpp
    src/ui/StarRatingDelegate.cpp
    src/ui/TableViewState.cpp
    src/ui/TrackMenuSections.cpp
    src/ui/TrackPropertiesDialog.cpp
    src/ui/TrackTable.cpp
    src/ui/ViewKeyReferences.cpp
    src/ui/trackinfo/TrackInfoPanel.cpp
    src/ui/trackinfo/TrackInfoSettings.cpp
    src/ui/trackinfo/TrackInfoSettingsDialog.cpp
    resources/resources.qrc
)

set(MUZAITEN_HEADERS
    src/app/AppCore.h
    src/app/AppPaths.h
    src/app/DemoScreens.h
    src/app/MuzaitenApplication.h
    src/core/Album.h
    src/core/Artist.h
    src/core/GenreTags.h
    src/core/HumanQuantity.h
    src/core/MetadataBlob.h
    src/core/MusicBrainzIds.h
    src/core/MusicSort.h
    src/core/Playlist.h
    src/core/Rating.h
    src/core/ScanRoot.h
    src/core/Track.h
    src/db/Database.h
    src/db/PlaylistDatabase.h
    src/db/SettingsStore.h
    src/db/Schema.h
    src/features/FeatureStore.h
    src/features/QualityRank.h
    src/features/SongIdentity.h
    src/fs/LinkRoot.h
    src/mpd/MpdConfig.h
    src/mpd/MpdClient.h
    src/mpd/MpdTrack.h
    src/mpd/MpdImportWorker.h
    src/ipc/IpcServer.h
    src/ipc/IpcSocket.h
    src/mpris/MprisService.h
    src/player/PlayerCore.h
    src/playback/AudioDeviceControl.h
    src/playback/PlaybackBackend.h
    src/playback/PlaybackTypes.h
    src/playback/GStreamerPlaybackBackend.h
    src/playback/QtPlaybackBackend.h
    src/reco/AffinityPool.h
    src/reco/ArtistRadio.h
    src/reco/GenreCuration.h
    src/reco/RadioMix.h
    src/reco/TrackScorer.h
    src/reco/WeightLearner.h
    src/reco/WeightLearnerData.h
    src/scanner/ArtworkCache.h
    src/scanner/DirectoryWalker.h
    src/scanner/LibraryScanner.h
    src/scanner/PathMetadataGuesser.h
    src/scanner/RatingTagSyncWorker.h
    src/scanner/ScanPipeline.h
    src/scanner/TagRating.h
    src/scanner/TagReader.h
    src/scanner/TagRatingWriter.h
    src/playlist/PlaylistImport.h
    src/playlist/PlaylistImportWorker.h
    src/playlist/PlaylistMatcher.h
    src/playlist/import/YouTubeImport.h
    src/scrobble/BackfillParse.h
    src/scrobble/LastFmApi.h
    src/scrobble/LastFmCredentials.h
    src/scrobble/LastFmScrobbler.h
    src/scrobble/ListenBrainzHub.h
    src/scrobble/ListenBrainzScrobbler.h
    src/scrobble/ListenBrainzUrl.h
    src/scrobble/ScrobbleDestination.h
    src/ui/ScrobblersPanel.h
    src/ui/ScrobblingDialog.h
    src/ui/MenuHighlightStyle.h
    src/ui/StickyMenu.h
    src/ui/ToggleSwitch.h
    src/ui/ScrobbleUploadDispatcher.h
    src/scrobble/ListenHistoryStore.h
    src/scrobble/ListenTracker.h
    src/scrobble/ScrobbleBackfill.h
    src/search/Exclusion.h
    src/search/FuzzyMatch.h
    src/search/IndexCache.h
    src/search/RankConfig.h
    src/search/ResultRanker.h
    src/search/SearchIndex.h
    src/search/SearchMatcher.h
    src/search/SearchQuery.h
    src/search/SearchRecord.h
    src/search/SearchWorker.h
    src/ui/AlbumGrid.h
    src/ui/AlbumGridDelegate.h
    src/ui/AlbumArtView.h
    src/ui/AudioAnalysisData.h
    src/ui/AudioAnalysisDialogs.h
    src/ui/ConditionEditor.h
    src/ui/NeighborColumnResizer.h
    src/ui/ResponsiveColumnLayout.h
    src/ui/ResponsiveColumnOptionsDialog.h
    src/ui/OverlayScrollBar.h
    src/ui/AlbumArtFallback.h
    src/ui/ArtistSidebar.h
    src/ui/DenseTableDelegate.h
    src/ui/FileExplorerView.h
    src/ui/FileExplorerKeybindings.h
    src/ui/IdleReleaseController.h
    src/ui/KeybindingsDialog.h
    src/ui/GenreCurationDialog.h
    src/ui/LinkRootsDialog.h
    src/ui/ListeningHistoryPanel.h
    src/ui/KeyBindingTypes.h
    src/ui/MainWindow.h
    src/ui/MusicExplorerView.h
    src/ui/MainPanelKeybindings.h
    src/ui/NavigableTableView.h
    src/ui/PanelSearchBar.h
    src/ui/PanelSearchController.h
    src/ui/PanelOrderDialog.h
    src/ui/PlaybackProfileDialog.h
    src/ui/PlaybackResumeDialog.h
    src/ui/StopAfterDialog.h
    src/ui/PlayerBar.h
    src/ui/RadioCustomizationDialog.h
    src/reco/RadioProfile.h
    src/ui/PlaylistAddDialog.h
    src/ui/HeaderLabelStyle.h
    src/ui/PanelBorderStyle.h
    src/ui/PlaylistImportDialog.h
    src/ui/PlaylistView.h
    src/ui/QueueKeybindings.h
    src/ui/QueueScreen.h
    src/ui/QueueStore.h
    src/ui/QueueTable.h
    src/ui/RankingDialog.h
    src/ui/ReorderableTableWidget.h
    src/ui/RightSidebar.h
    src/ui/SettingsDialogSupport.h
    src/ui/SearchResultDelegate.h
    src/ui/SearchResultsModel.h
    src/ui/SearchView.h
    src/ui/SourceDirectoriesDialog.h
    src/ui/SplitterPersistence.h
    src/ui/StarRating.h
    src/ui/StarRatingDelegate.h
    src/ui/TableViewState.h
    src/ui/TrackPropertiesDialog.h
    src/ui/TrackTable.h
    src/ui/UiMetrics.h
    src/ui/trackinfo/TrackInfoPanel.h
    src/ui/trackinfo/TrackInfoSettings.h
    src/ui/trackinfo/TrackInfoSettingsDialog.h
)

set(MUZAITEN_CORE_SOURCES
    src/app/AppPaths.cpp
    src/core/GenreTags.cpp
    src/core/HumanQuantity.cpp
    src/core/MetadataBlob.cpp
    src/core/MusicSort.cpp
    src/core/Rating.cpp
    src/core/TrackDisplay.cpp
    src/fs/MediaProbe.cpp
    src/fs/PathResolver.cpp
)

set(MUZAITEN_SEARCH_SOURCES
    src/search/Exclusion.cpp
    src/search/fold/Fold.cpp
    src/search/FuzzyMatch.cpp
    src/search/IndexCache.cpp
    src/search/RankConfig.cpp
    src/search/ResultRanker.cpp
    src/search/SearchIndex.cpp
    src/search/SearchMatcher.cpp
    src/search/SearchQuery.cpp
    src/search/SearchWorker.cpp
)

set(MUZAITEN_DB_SOURCES
    src/db/Database.cpp
    src/db/PlaylistDatabase.cpp
    src/db/SettingsStore.cpp
)

set(MUZAITEN_SQL_SOURCES src/db/SqlUtil.cpp)

set(MUZAITEN_FEATURES_SOURCES
    src/features/FeatureStore.cpp
    src/features/ProviderClient.cpp
    src/features/QualityRank.cpp
    src/features/QueryEmbedding.cpp
    src/features/QueryVectorCache.cpp
    src/features/SongIdentity.cpp
)

set(MUZAITEN_RECO_SOURCES
    src/reco/AffinityPool.cpp
    src/reco/ArtistRadio.cpp
    src/reco/GenreCuration.cpp
    src/reco/RadioFilters.cpp
    src/reco/RadioMix.cpp
    src/reco/RadioProfile.cpp
    src/reco/RadioSession.cpp
    src/reco/ReasonText.cpp
    src/reco/TrackScorer.cpp
    src/reco/WeightLearner.cpp
    src/reco/WeightLearnerData.cpp
)

set(MUZAITEN_SCROBBLE_SOURCES
    src/scrobble/BackfillParse.cpp
    src/scrobble/LastFmApi.cpp
    src/scrobble/LastFmScrobbler.cpp
    src/scrobble/ListenBrainzHub.cpp
    src/scrobble/ListenBrainzScrobbler.cpp
    src/scrobble/ListenBrainzUrl.cpp
    src/scrobble/ListenHistoryStore.cpp
    src/scrobble/ListenTracker.cpp
    src/scrobble/PlayEventRecorder.cpp
    src/scrobble/ScrobbleBackfill.cpp
    src/scrobble/ScrobbleDestination.cpp
)

set(MUZAITEN_PLAYLIST_SOURCES
    src/playlist/PlaylistDropImportWorker.cpp
    src/playlist/PlaylistImport.cpp
    src/playlist/PlaylistImportWorker.cpp
    src/playlist/PlaylistMatchIndex.cpp
    src/playlist/PlaylistMatcher.cpp
    src/playlist/import/YouTubeImport.cpp
)

set(MUZAITEN_PLAYER_SOURCES
    src/player/PlayerCore.cpp
)

set(MUZAITEN_PLAYBACK_SOURCES
    src/playback/AudioDeviceControl.cpp
    src/playback/GStreamerPlaybackBackend.cpp
    src/playback/PlaybackBackend.h
    src/playback/QtPlaybackBackend.cpp
)

set(MUZAITEN_SCANNER_SOURCES
    src/scanner/ArtworkCache.cpp
    src/scanner/DirectoryWalker.cpp
    src/scanner/LibraryScanner.cpp
    src/scanner/PathMetadataGuesser.cpp
    src/scanner/RatingTagSyncWorker.cpp
    src/scanner/ScanPipeline.cpp
    src/scanner/TagRating.cpp
    src/scanner/TagRatingWriter.cpp
    src/scanner/TagReader.cpp
)

set(MUZAITEN_IPC_SOURCES
    src/ipc/IpcServer.cpp
    src/ipc/IpcSocket.cpp
)

set(MUZAITEN_MPD_SOURCES
    src/mpd/MpdClient.cpp
    src/mpd/MpdConfig.cpp
    src/mpd/MpdImportWorker.cpp
)

set(MUZAITEN_MPRIS_SOURCES
    src/mpris/MprisService.cpp
)

# These sources have narrow, reusable UI dependencies. Keeping them separate
# lets widget tests link delegates, navigation helpers, and data adapters
# without pulling in the application's view hierarchy.
set(MUZAITEN_UI_SUPPORT_SOURCES
    src/ui/AlbumArtFallback.cpp
    src/ui/ConditionEditor.cpp
    src/ui/DenseTableDelegate.cpp
    src/ui/FileExplorerKeybindings.cpp
    src/ui/IdleReleaseController.cpp
    src/ui/MainPanelKeybindings.cpp
    src/ui/MenuHighlightStyle.cpp
    src/ui/NavigableTableView.cpp
    src/ui/NeighborColumnResizer.cpp
    src/ui/OverlayScrollBar.cpp
    src/ui/QueueKeybindings.cpp
    src/ui/ReorderableTableWidget.cpp
    src/ui/ResponsiveColumnLayout.cpp
    src/ui/ResponsiveColumnOptionsDialog.cpp
    src/ui/RowReorderSupport.cpp
    src/ui/ScrobbleUploadDispatcher.cpp
    src/ui/SemanticSearchData.cpp
    src/ui/SettingsDialogSupport.cpp
    src/ui/StarRating.cpp
    src/ui/StarRatingDelegate.cpp
    src/ui/StickyMenu.cpp
    src/ui/TableViewState.cpp
    src/ui/ToggleSwitch.cpp
    src/ui/trackinfo/TrackInfoSettings.cpp
    src/ui/trackinfo/TrackInfoSettingsDialog.cpp
    src/ui/TrackMenuSections.cpp
)

set(MUZAITEN_UI_DATA_SOURCES
    src/ui/AudioAnalysisData.cpp
)

set(MUZAITEN_UI_LIBRARY_SOURCES
    src/ui/AlbumGrid.cpp
    src/ui/AlbumGridDelegate.cpp
    src/ui/MusicExplorerView.cpp
)

set(MUZAITEN_UI_TABLE_SOURCES
    src/ui/PlaylistView.cpp
    src/ui/QueueScreen.cpp
    src/ui/QueueStore.cpp
    src/ui/QueueTable.cpp
    src/ui/TrackTable.cpp
)

set(MUZAITEN_UI_SEARCH_SOURCES
    src/ui/PanelSearchBar.cpp
    src/ui/PanelSearchController.cpp
    src/ui/SearchResultDelegate.cpp
    src/ui/SearchResultsModel.cpp
    src/ui/SearchView.cpp
)

set(MUZAITEN_UI_SCROBBLE_SOURCES
    src/ui/ListeningHistoryPanel.cpp
    src/ui/ScrobblersPanel.cpp
    src/ui/ScrobblingDialog.cpp
)

set(MUZAITEN_UI_DIALOG_SOURCES
    src/ui/AudioAnalysisDialogs.cpp
    src/ui/GenreCurationDialog.cpp
    src/ui/KeybindingsDialog.cpp
    src/ui/LinkRootsDialog.cpp
    src/ui/PanelOrderDialog.cpp
    src/ui/PlaybackProfileDialog.cpp
    src/ui/PlaybackResumeDialog.cpp
    src/ui/PlaylistAddDialog.cpp
    src/ui/PlaylistImportDialog.cpp
    src/ui/RankingDialog.cpp
    src/ui/SemanticSearchDialog.cpp
    src/ui/SourceDirectoriesDialog.cpp
    src/ui/StopAfterDialog.cpp
    src/ui/TrackPropertiesDialog.cpp
    src/ui/ViewKeyReferences.cpp
    src/ui/trackinfo/TrackInfoPanel.cpp
)

set(MUZAITEN_UI_SOURCES)
set(MUZAITEN_UI_APP_SOURCES
    src/ui/AlbumArtView.cpp
    src/ui/ArtistSidebar.cpp
    src/ui/FileExplorerView.cpp
    src/ui/MainWindow.cpp
    src/ui/PlayerBar.cpp
    src/ui/QueueSnapshotStore.cpp
    src/ui/RadioCustomizationDialog.cpp
    src/ui/RatingSyncController.cpp
    src/ui/RightSidebar.cpp
    src/ui/ScanController.cpp
    src/ui/ViewStatePersistence.cpp
)
foreach(_source IN LISTS MUZAITEN_SOURCES)
    if(_source MATCHES "^src/ui/.*\\.cpp$")
        list(APPEND MUZAITEN_UI_SOURCES "${_source}")
    endif()
endforeach()
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_SUPPORT_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_DATA_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_LIBRARY_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_TABLE_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_SEARCH_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_SCROBBLE_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_DIALOG_SOURCES})
list(REMOVE_ITEM MUZAITEN_UI_SOURCES ${MUZAITEN_UI_APP_SOURCES})
if(MUZAITEN_UI_SOURCES)
    message(FATAL_ERROR "UI sources have no library owner: ${MUZAITEN_UI_SOURCES}")
endif()

set(MUZAITEN_APPLICATION_SOURCES
    src/app/AppCore.cpp
    src/app/StartupStorage.cpp
    src/app/DemoScreens.cpp
    src/app/MuzaitenApplication.cpp
)

set(MUZAITEN_UNASSIGNED_SOURCES ${MUZAITEN_SOURCES})
list(REMOVE_ITEM MUZAITEN_UNASSIGNED_SOURCES
    src/app/main.cpp
    resources/resources.qrc
    ${MUZAITEN_CORE_SOURCES}
    ${MUZAITEN_SEARCH_SOURCES}
    ${MUZAITEN_DB_SOURCES}
    ${MUZAITEN_SQL_SOURCES}
    ${MUZAITEN_FEATURES_SOURCES}
    ${MUZAITEN_RECO_SOURCES}
    ${MUZAITEN_SCROBBLE_SOURCES}
    ${MUZAITEN_PLAYLIST_SOURCES}
    ${MUZAITEN_PLAYER_SOURCES}
    ${MUZAITEN_PLAYBACK_SOURCES}
    ${MUZAITEN_SCANNER_SOURCES}
    ${MUZAITEN_IPC_SOURCES}
    ${MUZAITEN_MPD_SOURCES}
    ${MUZAITEN_MPRIS_SOURCES}
    ${MUZAITEN_UI_SUPPORT_SOURCES}
    ${MUZAITEN_UI_DATA_SOURCES}
    ${MUZAITEN_UI_LIBRARY_SOURCES}
    ${MUZAITEN_UI_TABLE_SOURCES}
    ${MUZAITEN_UI_SEARCH_SOURCES}
    ${MUZAITEN_UI_SCROBBLE_SOURCES}
    ${MUZAITEN_UI_DIALOG_SOURCES}
    ${MUZAITEN_UI_APP_SOURCES}
    ${MUZAITEN_APPLICATION_SOURCES}
)
if(MUZAITEN_UNASSIGNED_SOURCES)
    message(FATAL_ERROR "Application sources have no library owner: ${MUZAITEN_UNASSIGNED_SOURCES}")
endif()

function(muzaiten_configure_library target)
    target_include_directories(${target} PUBLIC
        src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_compile_definitions(${target} PUBLIC
        MUZAITEN_LASTFM_API_KEY="${MUZAITEN_LASTFM_API_KEY}"
        MUZAITEN_LASTFM_SHARED_SECRET="${MUZAITEN_LASTFM_SHARED_SECRET}"
    )
    target_compile_features(${target} PUBLIC cxx_std_26)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
    )
endfunction()

function(muzaiten_configure_executable target)
    target_include_directories(${target} PRIVATE
        src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_compile_features(${target} PRIVATE cxx_std_26)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
    )
endfunction()

add_library(muzaiten_core STATIC ${MUZAITEN_CORE_SOURCES})
muzaiten_configure_library(muzaiten_core)
target_link_libraries(muzaiten_core PUBLIC Qt6::Core PRIVATE PkgConfig::ZSTD)

add_library(muzaiten_search STATIC ${MUZAITEN_SEARCH_SOURCES})
muzaiten_configure_library(muzaiten_search)
target_link_libraries(muzaiten_search PUBLIC muzaiten_core Qt6::Core PRIVATE Qt6::Sql)

add_library(muzaiten_sql STATIC ${MUZAITEN_SQL_SOURCES})
muzaiten_configure_library(muzaiten_sql)
target_link_libraries(muzaiten_sql PUBLIC Qt6::Core Qt6::Sql)

add_library(muzaiten_db STATIC ${MUZAITEN_DB_SOURCES})
muzaiten_configure_library(muzaiten_db)
target_link_libraries(muzaiten_db PUBLIC muzaiten_core muzaiten_search muzaiten_sql Qt6::Core Qt6::Sql)

add_library(muzaiten_features STATIC ${MUZAITEN_FEATURES_SOURCES})
muzaiten_configure_library(muzaiten_features)
target_link_libraries(muzaiten_features PUBLIC muzaiten_core Qt6::Core Qt6::Sql)

add_library(muzaiten_reco STATIC ${MUZAITEN_RECO_SOURCES})
muzaiten_configure_library(muzaiten_reco)
target_link_libraries(muzaiten_reco PUBLIC muzaiten_core Qt6::Core PRIVATE muzaiten_db Qt6::Sql)

add_library(muzaiten_scrobble STATIC ${MUZAITEN_SCROBBLE_SOURCES})
muzaiten_configure_library(muzaiten_scrobble)
target_link_libraries(muzaiten_scrobble PUBLIC muzaiten_core muzaiten_sql Qt6::Core Qt6::Network Qt6::Sql)

add_library(muzaiten_playlist STATIC ${MUZAITEN_PLAYLIST_SOURCES})
muzaiten_configure_library(muzaiten_playlist)
target_link_libraries(muzaiten_playlist PUBLIC
    muzaiten_core
    muzaiten_search
    muzaiten_db
    Qt6::Core
    Qt6::Network
)

add_library(muzaiten_player STATIC ${MUZAITEN_PLAYER_SOURCES})
muzaiten_configure_library(muzaiten_player)
target_link_libraries(muzaiten_player PUBLIC muzaiten_core Qt6::Core)

add_library(muzaiten_playback STATIC ${MUZAITEN_PLAYBACK_SOURCES})
muzaiten_configure_library(muzaiten_playback)
target_link_libraries(muzaiten_playback PUBLIC Qt6::Core PRIVATE Qt6::Multimedia PkgConfig::GSTREAMER)

add_library(muzaiten_scanner STATIC ${MUZAITEN_SCANNER_SOURCES})
muzaiten_configure_library(muzaiten_scanner)
target_link_libraries(muzaiten_scanner PUBLIC
    muzaiten_core
    muzaiten_db
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    PRIVATE PkgConfig::TAGLIB
)

add_library(muzaiten_ipc STATIC ${MUZAITEN_IPC_SOURCES})
muzaiten_configure_library(muzaiten_ipc)
target_link_libraries(muzaiten_ipc PUBLIC muzaiten_core Qt6::Core Qt6::Network)

add_library(muzaiten_mpd STATIC ${MUZAITEN_MPD_SOURCES})
muzaiten_configure_library(muzaiten_mpd)
target_link_libraries(muzaiten_mpd PUBLIC muzaiten_core Qt6::Core Qt6::Network PRIVATE muzaiten_db)

add_library(muzaiten_mpris STATIC ${MUZAITEN_MPRIS_SOURCES})
muzaiten_configure_library(muzaiten_mpris)
target_link_libraries(muzaiten_mpris PUBLIC muzaiten_core muzaiten_player Qt6::Core Qt6::DBus PRIVATE muzaiten_db)

add_library(muzaiten_ui_support STATIC ${MUZAITEN_UI_SUPPORT_SOURCES})
muzaiten_configure_library(muzaiten_ui_support)
target_link_libraries(muzaiten_ui_support PUBLIC
    muzaiten_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

add_library(muzaiten_ui_data STATIC ${MUZAITEN_UI_DATA_SOURCES})
muzaiten_configure_library(muzaiten_ui_data)
target_link_libraries(muzaiten_ui_data PUBLIC
    muzaiten_core
    muzaiten_features
    Qt6::Core
    Qt6::Sql
    PRIVATE muzaiten_db
)

add_library(muzaiten_ui_search STATIC ${MUZAITEN_UI_SEARCH_SOURCES})
muzaiten_configure_library(muzaiten_ui_search)
target_link_libraries(muzaiten_ui_search PUBLIC
    muzaiten_ui_support
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PRIVATE muzaiten_db muzaiten_search
)

add_library(muzaiten_ui_tables STATIC ${MUZAITEN_UI_TABLE_SOURCES})
muzaiten_configure_library(muzaiten_ui_tables)
target_link_libraries(muzaiten_ui_tables PUBLIC
    muzaiten_ui_support
    muzaiten_ui_search
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Widgets
    PRIVATE muzaiten_db
)

add_library(muzaiten_ui_library STATIC ${MUZAITEN_UI_LIBRARY_SOURCES})
muzaiten_configure_library(muzaiten_ui_library)
target_link_libraries(muzaiten_ui_library PUBLIC
    muzaiten_ui_support
    muzaiten_ui_tables
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

add_library(muzaiten_ui_scrobble STATIC ${MUZAITEN_UI_SCROBBLE_SOURCES})
muzaiten_configure_library(muzaiten_ui_scrobble)
target_link_libraries(muzaiten_ui_scrobble PUBLIC
    muzaiten_ui_support
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PRIVATE muzaiten_scrobble
)

add_library(muzaiten_ui_dialogs STATIC ${MUZAITEN_UI_DIALOG_SOURCES})
muzaiten_configure_library(muzaiten_ui_dialogs)
target_link_libraries(muzaiten_ui_dialogs PUBLIC
    muzaiten_ui_support
    muzaiten_ui_tables
    muzaiten_ui_search
    muzaiten_ui_data
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PRIVATE
        muzaiten_db
        muzaiten_features
        muzaiten_playback
        muzaiten_playlist
)

add_library(muzaiten_ui_app STATIC ${MUZAITEN_UI_APP_SOURCES})
muzaiten_configure_library(muzaiten_ui_app)
target_link_libraries(muzaiten_ui_app PUBLIC
    muzaiten_ui_library
    muzaiten_ui_tables
    muzaiten_ui_search
    muzaiten_ui_scrobble
    muzaiten_ui_dialogs
    muzaiten_ui_data
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    PRIVATE
        muzaiten_core
        muzaiten_db
        muzaiten_features
        muzaiten_ipc
        muzaiten_mpd
        muzaiten_mpris
        muzaiten_player
        muzaiten_playback
        muzaiten_playlist
        muzaiten_reco
        muzaiten_scanner
        muzaiten_scrobble
        muzaiten_search
)

add_library(muzaiten_app STATIC ${MUZAITEN_APPLICATION_SOURCES})
muzaiten_configure_library(muzaiten_app)
target_link_libraries(muzaiten_app PUBLIC
    muzaiten_core
    muzaiten_db
    muzaiten_features
    muzaiten_ipc
    muzaiten_mpd
    muzaiten_mpris
    muzaiten_player
    muzaiten_playback
    muzaiten_playlist
    muzaiten_reco
    muzaiten_scanner
    muzaiten_scrobble
    muzaiten_search
    Qt6::Concurrent
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    PkgConfig::GSTREAMER
    PkgConfig::TAGLIB
    PkgConfig::ZSTD
)

add_executable(muzaiten src/app/main.cpp resources/resources.qrc)
muzaiten_configure_executable(muzaiten)
target_link_libraries(muzaiten PRIVATE muzaiten_app muzaiten_ui_app)

# Command-line client. Shares the state-path resolution (AppPaths) so it always
# finds the instance matching its environment. `search` runs standalone: it
# opens the library DB and the folded-index cache directly (no running instance
# needed), so the binary links the DB and search engine alongside the IPC socket.
add_library(muzaiten_cli STATIC src/cli/SearchCli.cpp)
muzaiten_configure_library(muzaiten_cli)
target_link_libraries(muzaiten_cli PUBLIC muzaiten_core muzaiten_db muzaiten_search Qt6::Core)

add_executable(muzaitenctl src/cli/MuzaitenCtl.cpp)
muzaiten_configure_executable(muzaitenctl)
target_link_libraries(muzaitenctl PRIVATE
    muzaiten_cli
    muzaiten_features
    muzaiten_ipc
    muzaiten_reco
    muzaiten_scrobble
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

# The scalar analyzer is its own target so every consumer (indexer, tests,
# bench) compiles ONE translation unit with pinned floating-point behavior:
# -ffp-contract=off forbids FMA contraction, which would otherwise let a
# user's ambient -march=native (AUR muzaiten-git/-dev builds honor their
# CXXFLAGS) produce different scalar bits for the same audio at the same
# dsp_version. Determinism here is a correctness property of the versioned
# feature store, not an optimization preference.
add_library(muzaiten_dsp STATIC
    src/indexer/Dsp.cpp
    src/indexer/Fft.cpp
)
target_include_directories(muzaiten_dsp PUBLIC src)
target_compile_features(muzaiten_dsp PUBLIC cxx_std_26)
target_compile_options(muzaiten_dsp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off -Wall -Wextra -Wpedantic -Wconversion -Wshadow>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:-O3>
)

add_executable(muzaiten-features src/indexer/FeaturesMain.cpp)
muzaiten_configure_executable(muzaiten-features)
target_link_libraries(muzaiten-features PRIVATE
    muzaiten_core
    muzaiten_dsp
    muzaiten_features
    Qt6::Core
    Qt6::Sql
    PkgConfig::Chromaprint
)

# Installation (freedesktop integration). Lets distro packaging and `make dev`
# alike drop the binary plus a desktop entry, icon, and AppStream metadata into
# the standard prefix.
include(GNUInstallDirs)

install(TARGETS muzaiten muzaitenctl muzaiten-features
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
# The standalone playlist-import helper (convert/youtube subcommands). Deployed
# to bin/ alongside the binaries so it shares the muzaiten* namespace and turns
# up under `muzaiten<Tab>`. PROGRAMS (not FILES) preserves its executable bit; a
# pure Python single-file tool, so no build target — just copied as-is.
install(PROGRAMS tools/playlist-import/muzaiten-import
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES packaging/org.11xx.muzaiten.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)
install(FILES packaging/org.11xx.muzaiten.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
)
install(FILES packaging/org.11xx.muzaiten.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)

# Uninstall: reverse the last `cmake --install` by deleting the files it
# recorded in install_manifest.txt. CMake ships no uninstall target, so wire up
# the conventional manifest-driven one. Prefix-agnostic — cleans whatever prefix
# was installed into (the user-space ~/.local default, /usr, a custom PREFIX).
if(NOT TARGET uninstall)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        IMMEDIATE @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        COMMENT "Uninstalling files recorded in install_manifest.txt"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
