cmake_minimum_required(VERSION 4.0)

project(muzaiten
    VERSION 2026.5.23.6
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
    src/scanner/ScanWorker.cpp
    src/scanner/ArtworkResolver.cpp
    src/scanner/LibraryScanner.cpp
    src/scanner/TagReader.cpp
    src/scrobble/ListenBrainzWorker.cpp
    src/ui/AlbumGrid.cpp
    src/ui/AlbumGridDelegate.cpp
    src/ui/ArtistSidebar.cpp
    src/ui/MainWindow.cpp
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
    src/scanner/ArtworkResolver.h
    src/scanner/LibraryScanner.h
    src/scanner/ScanWorker.h
    src/scanner/TagReader.h
    src/scrobble/ListenBrainzWorker.h
    src/ui/AlbumGrid.h
    src/ui/AlbumGridDelegate.h
    src/ui/ArtistSidebar.h
    src/ui/MainWindow.h
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
)

target_compile_options(muzaiten PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
