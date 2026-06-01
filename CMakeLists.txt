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

find_package(Qt6 6.11 REQUIRED COMPONENTS Core DBus Gui Multimedia Network Widgets Sql Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-audio-1.0
    gstreamer-pbutils-1.0
    glib-2.0
    gobject-2.0
)

configure_file(
    src/core/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h
    @ONLY
)

set(MUZAITEN_SOURCES
    src/app/main.cpp
    src/app/AppPaths.cpp
    src/app/MuzaitenApplication.cpp
    src/core/MetadataBlob.cpp
    src/core/MusicSort.cpp
    src/core/Rating.cpp
    src/db/Database.cpp
    src/db/SettingsStore.cpp
    src/fs/PathResolver.cpp
    src/mpd/MpdConfig.cpp
    src/mpd/MpdClient.cpp
    src/mpd/MpdImportWorker.cpp
    src/mpris/MprisService.cpp
    src/playback/GStreamerPlaybackBackend.cpp
    src/playback/QtPlaybackBackend.cpp
    src/scanner/DirectoryWalker.cpp
    src/scanner/ScanPipeline.cpp
    src/scanner/ArtworkCache.cpp
    src/scanner/ArtworkResolver.cpp
    src/scanner/LibraryScanner.cpp
    src/scanner/RatingTagSyncWorker.cpp
    src/scanner/TagRating.cpp
    src/scanner/TagReader.cpp
    src/scanner/TagRatingWriter.cpp
    src/scrobble/LastFmApi.cpp
    src/scrobble/LastFmScrobbler.cpp
    src/scrobble/ListenBrainzScrobbler.cpp
    src/search/Exclusion.cpp
    src/search/FuzzyMatch.cpp
    src/search/RankConfig.cpp
    src/search/ResultRanker.cpp
    src/search/SearchIndex.cpp
    src/search/SearchMatcher.cpp
    src/search/SearchQuery.cpp
    src/search/SearchWorker.cpp
    src/ui/AlbumGrid.cpp
    src/ui/NeighborColumnResizer.cpp
    src/ui/OverlayScrollBar.cpp
    src/ui/AlbumGridDelegate.cpp
    src/ui/AlbumArtFallback.cpp
    src/ui/ArtistSidebar.cpp
    src/ui/DenseTableDelegate.cpp
    src/ui/FileExplorerView.cpp
    src/ui/FileExplorerKeybindings.cpp
    src/ui/LinkRootsDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/MainPanelKeybindings.cpp
    src/ui/PlaybackProfileDialog.cpp
    src/ui/PlaybackResumeDialog.cpp
    src/ui/PanelSearchController.cpp
    src/ui/PlayerBar.cpp
    src/ui/RankingDialog.cpp
    src/ui/RightSidebar.cpp
    src/ui/SearchResultDelegate.cpp
    src/ui/SearchResultsModel.cpp
    src/ui/SearchView.cpp
    src/ui/SourceDirectoriesDialog.cpp
    src/ui/StarRating.cpp
    src/ui/StarRatingDelegate.cpp
    src/ui/TrackTable.cpp
    resources/resources.qrc
)

set(MUZAITEN_HEADERS
    src/app/AppPaths.h
    src/app/MuzaitenApplication.h
    src/core/Album.h
    src/core/Artist.h
    src/core/MetadataBlob.h
    src/core/MusicBrainzIds.h
    src/core/MusicSort.h
    src/core/Rating.h
    src/core/ScanRoot.h
    src/core/Track.h
    src/db/Database.h
    src/db/SettingsStore.h
    src/db/Schema.h
    src/fs/LinkRoot.h
    src/mpd/MpdConfig.h
    src/mpd/MpdClient.h
    src/mpd/MpdTrack.h
    src/mpd/MpdImportWorker.h
    src/mpris/MprisService.h
    src/playback/PlaybackBackend.h
    src/playback/PlaybackTypes.h
    src/playback/GStreamerPlaybackBackend.h
    src/playback/QtPlaybackBackend.h
    src/scanner/ArtworkCache.h
    src/scanner/ArtworkResolver.h
    src/scanner/DirectoryWalker.h
    src/scanner/LibraryScanner.h
    src/scanner/RatingTagSyncWorker.h
    src/scanner/ScanPipeline.h
    src/scanner/TagRating.h
    src/scanner/TagReader.h
    src/scanner/TagRatingWriter.h
    src/scrobble/LastFmApi.h
    src/scrobble/LastFmCredentials.h
    src/scrobble/LastFmScrobbler.h
    src/scrobble/ListenBrainzScrobbler.h
    src/search/Exclusion.h
    src/search/FuzzyMatch.h
    src/search/RankConfig.h
    src/search/ResultRanker.h
    src/search/SearchIndex.h
    src/search/SearchMatcher.h
    src/search/SearchQuery.h
    src/search/SearchRecord.h
    src/search/SearchWorker.h
    src/ui/AlbumGrid.h
    src/ui/AlbumGridDelegate.h
    src/ui/NeighborColumnResizer.h
    src/ui/OverlayScrollBar.h
    src/ui/AlbumArtFallback.h
    src/ui/ArtistSidebar.h
    src/ui/DenseTableDelegate.h
    src/ui/FileExplorerView.h
    src/ui/FileExplorerKeybindings.h
    src/ui/LinkRootsDialog.h
    src/ui/KeyBindingTypes.h
    src/ui/MainWindow.h
    src/ui/MainPanelKeybindings.h
    src/ui/PanelSearchController.h
    src/ui/PlaybackProfileDialog.h
    src/ui/PlaybackResumeDialog.h
    src/ui/PlayerBar.h
    src/ui/RankingDialog.h
    src/ui/RightSidebar.h
    src/ui/SearchResultDelegate.h
    src/ui/SearchResultsModel.h
    src/ui/SearchView.h
    src/ui/SourceDirectoriesDialog.h
    src/ui/StarRating.h
    src/ui/StarRatingDelegate.h
    src/ui/TrackTable.h
)

add_executable(muzaiten ${MUZAITEN_SOURCES} ${MUZAITEN_HEADERS})

target_include_directories(muzaiten PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_definitions(muzaiten PRIVATE
    MUZAITEN_LASTFM_API_KEY="${MUZAITEN_LASTFM_API_KEY}"
    MUZAITEN_LASTFM_SHARED_SECRET="${MUZAITEN_LASTFM_SHARED_SECRET}"
)

target_link_libraries(muzaiten PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    Qt6::Multimedia
    Qt6::Widgets
    Qt6::Sql
    PkgConfig::TAGLIB
    PkgConfig::GSTREAMER
    PkgConfig::ZSTD
)

target_compile_options(muzaiten PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
