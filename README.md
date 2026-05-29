# muzaiten

muzaiten is a native Linux music player focused on library browsing, queue-based playback, album artwork, and rating-oriented workflows.

The project is in early development. The implementation currently uses C++26, Qt 6 Widgets, SQLite, TagLib, Qt Multimedia, and Qt Network.

![muzaiten main window - light](docs/assets/screenshot-light.png#gh-light-mode-only)
![muzaiten main window - dark](docs/assets/screenshot-dark.png#gh-dark-mode-only)

## Features

- Album artist sidebar.
- Album grid with cached artwork.
- Artist track table with sortable ratings.
- DB-only user ratings in half-star increments.
- Queue sidebar with album art for the current track.
- ListenBrainz and Last.fm scrobbling with playing-now and completed-listen submissions.
- Persistent UI state for core panes, table columns, and splitters.

## Library Safety

muzaiten treats the music library as read-only for playback and indexing. Current rating edits and app state are stored in SQLite state files, not written back to audio files.

## Build

Dependencies:

- CMake
- Ninja
- C++26-capable compiler
- Qt 6 with Core, Gui, Multimedia, Network, Widgets, Sql, and Test modules
- TagLib

Recommended frontend:

```sh
make build
make test
make smoke
make run
```

This `Makefile` is a thin wrapper around CMake. It does not replace CMake as the build system; it provides stable convenience commands for common development flows.

Direct CMake usage remains supported:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
timeout 2s env QT_QPA_PLATFORM=offscreen ./build/muzaiten
```

## Runtime State

When run from `build/`, development state is kept under:

```text
build/dev-state/
```

Use normal XDG state paths with:

```sh
./build/muzaiten --xdg-state
```

or provide an explicit state root:

```sh
./build/muzaiten --state-root /tmp/muzaiten-state
```

Verbose diagnostics:

```sh
./build/muzaiten --verbose
MUZAITEN_VERBOSE=1 ./build/muzaiten
```
