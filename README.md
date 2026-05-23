# muzaiten

muzaiten is a native Linux music player in early development. The first version
is intentionally narrow: a read-only music library browser with a MusicBee-like
album artist workflow, album artwork, and rating-first track sorting.

Current target version: `2026.5.23`.

## First version scope

- C++26 and Qt 6 Widgets.
- SQLite-backed read-only library index.
- Album artist sidebar.
- Album grid with cover art.
- Artist track table with visible rating column.
- Folder artwork first, then embedded artwork, then generated cache.
- No playback and no tag writing yet.

## Safety rule

The music library is treated as sacred. Version `2026.5.23` opens music files
read-only, stores application state outside the library, and never writes,
moves, renames, deletes, or rewrites source files.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

