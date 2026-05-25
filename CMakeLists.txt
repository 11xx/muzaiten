cmake_minimum_required(VERSION 4.0)

project(muzaiten
    VERSION 2026.5.24.1
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

include(CheckPIESupported)
check_pie_supported()

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Multimedia Network Widgets Sql Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
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
    src/app/MuzaitenApplication.cpp
    src/core/Rating.cpp
    src/db/Database.cpp
    src/fs/PathResolver.cpp
    src/mpd/MpdConfig.cpp
    src/mpd/MpdClient.cpp
    src/mpd/MpdImportWorker.cpp
    src/playback/GStreamerPlaybackBackend.cpp
    src/playback/QtPlaybackBackend.cpp
    src/scanner/ScanWorker.cpp
    src/scanner/ArtworkResolver.cpp
    src/scanner/LibraryScanner.cpp
    src/scanner/RatingTagSyncWorker.cpp
    src/scanner/TagRating.cpp
    src/scanner/TagReader.cpp
    src/scanner/TagRatingWriter.cpp
    src/scrobble/ListenBrainzScrobbler.cpp
    src/ui/AlbumGrid.cpp
    src/ui/AlbumGridDelegate.cpp
    src/ui/AlbumArtFallback.cpp
    src/ui/ArtistSidebar.cpp
    src/ui/DenseTableDelegate.cpp
    src/ui/LinkRootsDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/PlaybackProfileDialog.cpp
    src/ui/PlayerBar.cpp
    src/ui/RightSidebar.cpp
    src/ui/StarRating.cpp
    src/ui/StarRatingDelegate.cpp
    src/ui/TrackTable.cpp
    resources/resources.qrc
)

set(MUZAITEN_HEADERS
    src/app/MuzaitenApplication.h
    src/core/Album.h
    src/core/Artist.h
    src/core/MusicBrainzIds.h
    src/core/Rating.h
    src/core/Track.h
    src/db/Database.h
    src/db/Schema.h
    src/fs/LinkRoot.h
    src/mpd/MpdConfig.h
    src/mpd/MpdClient.h
    src/mpd/MpdTrack.h
    src/mpd/MpdImportWorker.h
    src/playback/PlaybackBackend.h
    src/playback/PlaybackTypes.h
    src/playback/GStreamerPlaybackBackend.h
    src/playback/QtPlaybackBackend.h
    src/scanner/ArtworkResolver.h
    src/scanner/LibraryScanner.h
    src/scanner/RatingTagSyncWorker.h
    src/scanner/ScanWorker.h
    src/scanner/TagRating.h
    src/scanner/TagReader.h
    src/scanner/TagRatingWriter.h
    src/scrobble/ListenBrainzScrobbler.h
    src/ui/AlbumGrid.h
    src/ui/AlbumGridDelegate.h
    src/ui/AlbumArtFallback.h
    src/ui/ArtistSidebar.h
    src/ui/DenseTableDelegate.h
    src/ui/LinkRootsDialog.h
    src/ui/MainWindow.h
    src/ui/PlaybackProfileDialog.h
    src/ui/PlayerBar.h
    src/ui/RightSidebar.h
    src/ui/StarRating.h
    src/ui/StarRatingDelegate.h
    src/ui/TrackTable.h
)

add_executable(muzaiten ${MUZAITEN_SOURCES} ${MUZAITEN_HEADERS})

target_include_directories(muzaiten PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(muzaiten PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Multimedia
    Qt6::Widgets
    Qt6::Sql
    PkgConfig::TAGLIB
    PkgConfig::GSTREAMER
)

target_compile_options(muzaiten PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
