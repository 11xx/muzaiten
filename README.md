# muzaiten

muzaiten is a native Linux music player focused on local-library browsing, queue-based playback, album artwork, ratings, and scrobbling.

The project is in early development. It currently uses C++26, Qt 6 Widgets, SQLite, TagLib, GStreamer, Qt DBus, Qt Network, and zstd.

![muzaiten main window - light](docs/assets/screenshot-light.png#gh-light-mode-only)
![muzaiten main window - dark](docs/assets/screenshot-dark.png#gh-dark-mode-only)

## Features

- Local library scanner with source-directory management, incremental rescans, and missing-file marking.
- Album artist sidebar, album grid, artist track table, and queue sidebar.
- Library file explorer and free-roam file explorer, with keyboard-oriented navigation profiles.
- Cached album artwork from folder images and embedded artwork.
- Queue playback through a GStreamer backend, with configurable output/resume behavior.
- User ratings with optional MusicBee-compatible tag sync back to audio files.
- ListenBrainz and Last.fm scrobbling, including now-playing updates and completed-listen submissions.
- MPD metadata import for browsing MPD libraries alongside local sources.
- Persistent UI state for splitters, table columns, row heights, sorting, queue state, playback resume, and explorer state.

## Library Safety

muzaiten indexes and plays local files without moving, renaming, deleting, or writing artwork into the music library.

Rating edits are stored in the application database immediately. For local files, muzaiten can also write those ratings back to tags through `File > Rating tags` or the automatic pending-write path after a rating change. Tag writes are narrow in scope: they update the rating field, verify the result by re-reading the file, and keep failed or unwritable writes pending for retry.

Scans do not traverse symlinks. Missing files are marked missing rather than deleted from the database until you explicitly choose `File > Remove missing tracks`.

## Build

Dependencies:

- CMake 4.x
- Ninja
- C++26-capable compiler
- Qt 6.11+ with Core, DBus, Gui, Multimedia, Network, Widgets, Sql, and Test modules
- TagLib 2.x
- GStreamer 1.x with audio and pbutils modules
- zstd

Recommended development commands:

```sh
make build
make test
make smoke
make run
```

The `Makefile` is a thin wrapper around CMake. `make run`, `make test`, and `make smoke` use the existing build; run `make build` explicitly after code changes. `make dev` is the build-and-run convenience target for an isolated development profile.

Direct CMake usage is also supported:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/muzaiten
```

For an isolated development profile under `./dev-state`:

```sh
make dev
# or
./build/muzaiten --dev-state
```

## Install

On Arch Linux (AUR):

- `muzaiten-bin` — prebuilt binary with bundled default Last.fm credentials; no
  compilation required.
- `muzaiten-git` — builds the latest commit from source; ships without a default
  Last.fm key (add your own in the app).

From source, into a prefix:

```sh
make build CMAKE_BUILD_TYPE=Release
sudo make install PREFIX=/usr      # PREFIX defaults to whatever was configured (/usr/local)
```

Installing drops the `muzaiten` binary plus a desktop entry, scalable icon, and
AppStream metadata into the prefix. (`make` here is only a thin alias for the
fixed CMake commands — use `cmake`/`cmake --install` directly if you need finer
control.) Release packaging, the AUR packages, and the Last.fm credential threat
model are documented in [docs/distribution.md](docs/distribution.md).

## Runtime State

By default, muzaiten uses XDG paths:

- data: `$XDG_DATA_HOME/muzaiten` or `~/.local/share/muzaiten`
- state: `$XDG_STATE_HOME/muzaiten` or `~/.local/state/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten` or `~/.cache/muzaiten`
- config: `$XDG_CONFIG_HOME/muzaiten` or `~/.config/muzaiten`

Useful overrides:

```sh
./build/muzaiten --state-root /tmp/muzaiten-state
./build/muzaiten --data-dir /path/to/data
./build/muzaiten --state-dir /path/to/state
./build/muzaiten --cache-dir /path/to/cache
./build/muzaiten --config-dir /path/to/config
```

Environment equivalents are available:

```sh
MUZAITEN_STATE_ROOT=/tmp/muzaiten-state ./build/muzaiten
MUZAITEN_DEV_STATE=1 ./build/muzaiten
MUZAITEN_DATA_DIR=/path/to/data ./build/muzaiten
MUZAITEN_STATE_DIR=/path/to/state ./build/muzaiten
MUZAITEN_CACHE_DIR=/path/to/cache ./build/muzaiten
MUZAITEN_CONFIG_DIR=/path/to/config ./build/muzaiten
```

On first run, muzaiten writes a commented config template at:

```text
$XDG_CONFIG_HOME/muzaiten/muzaiten.conf
```

The config file can set `paths.data`, `paths.state`, and `paths.cache`. CLI flags and environment variables take precedence.

Verbose diagnostics:

```sh
./build/muzaiten --verbose
MUZAITEN_VERBOSE=1 ./build/muzaiten
```

## Common Controls

- `1`: library panels view.
- `2`: toggle between library explorer and free-roam explorer.
- `3`: library search view (fzf-style, searches entire library including MPD tracks).
- `o`: find the current track in the active library view.
- Queue context menu: play a row, find that row in the library, open its containing directory, remove rows, clear play-next priority, or clear the queue.
- `Ctrl+scroll` over the queue or file explorers: adjust row height.
- Context menus on tables, album grid, queue header, and explorer rows expose sorting, visibility, rating, and queue actions.

## Library Search

Press `3` to open the search view. Type to filter interactively; all terms AND together in any order.

**Search keybindings:**

| Key | Action |
|-----|--------|
| `Enter` | Add selected (or cursor) results to queue |
| `Alt+Enter` | Add to queue and play immediately |
| `↑` / `↓` / `Ctrl+P` / `Ctrl+N` | Move the result cursor |
| `PgUp` / `PgDn` / `Home` / `End` | Jump the cursor |
| `Tab` | Mark current row, advance cursor |
| `Ctrl+Space` | Toggle mark on current row |
| `Ctrl+A` | Mark all results |
| `Ctrl+F` | Toggle fuzzy mode (default: exact orderless substring) |
| Result order | Configurable via `Settings > Search ranking…` (see below) |
| `Esc` / `Ctrl+G` | Clear the query; press again to leave text-input mode |
| `/` | Return focus to the search box (from browse mode) |
| `F5` (or re-press `3`) | Rebuild the search index |
| `Ctrl+scroll` | Adjust result row height |
| Double-click | Play now |
| Right-click | Context menu: play / queue / play next / find in library / open directory |

The result list always keeps a highlighted cursor you move with the arrow or
`Ctrl+P`/`Ctrl+N` keys while the search box keeps focus, fzf-style. `Esc`/`Ctrl+G`
first clears the query, then (when already empty) releases the text box so `1`
and `2` switch views; `/` jumps back into the box. The library index is loaded
into memory on first use and kept resident, so re-opening search is instant.

**Query syntax:**

| Pattern | Meaning |
|---------|---------|
| `miles blue` | Both "miles" AND "blue" anywhere in the record (orderless) |
| `!classical` | Negate: exclude matches |
| `^miles` | Prefix anchor: field must start with "miles" |
| `blue$` | Suffix anchor: field must end with "blue" |
| `'exact` | Force exact match in fuzzy mode |
| `artist:davis` | Match only in artist/album-artist field |
| `album:blue` | Match only in album title |
| `title:what` | Match only in track title |
| `path:/gak` | Match only in file path |
| `ext:flac` | Exact extension/codec filter |
| `khz:>=96` | Sample rate ≥ 96 kHz |
| `hz:44100` | Sample rate = 44100 Hz |
| `kbps:>320` | Bitrate > 320 kbps |
| `ch:2` | Stereo (2 channels) |
| `rating:>=80` | User rating ≥ 80/100 |
| `year:>=2000` | Release year ≥ 2000 |
| `dur:>3:30` | Duration > 3 min 30 sec |

Combine freely: `miles ext:flac rating:>=80 !live`

### Ranking and exclusions

`Settings > Search ranking…` opens a panel that controls how results are
ordered and what is filtered out:

- **Ranking criteria** — an ordered list (top = highest priority). Each row is a
  criterion with a direction: Relevance, Audio quality, Preferred directory
  (with a path), Library order (the same grouping as the rest of the app), or a
  single sort field. The default is **Relevance → Audio quality → Library
  order**, so the most relevant matches come first and, among equally relevant
  ones, higher-quality audio (lossless / higher sample rate) floats up. Move a
  criterion above Relevance to make it dominate the matched results.
- **Audio quality** scores lossless codecs first, then sample rate, then
  bitrate/channels. (Bit depth is not scanned yet; it will factor in once it is.)
- **Exclude patterns** — glob patterns (`*`, `?`) that drop results entirely,
  each scoped to the file path or to any field. Excluded tracks are skipped
  before ranking, so excluding large folders can make search faster. Examples:
  `*/Podcasts/*` (Path), `*.m4b` (Path), `*live*` (Any field).

Ranking and exclusion changes apply live and persist across restarts.

## File Explorer Notes

The file explorers support selectable keybinding profiles from the explorer context menu:

- Vim-style: `j/k/h/l`, `Space` play, `a` add, `p` play next, `i` import, `f` open containing directory, `b` jump to start folder, `~` home.
- Emacs/Dired-style: `n/p`, `Space` play, `s` add, `!` play next, `i` import, `f` open containing directory, `b` jump to start folder, `~` home.

Backspace remains "up/back directory". The free-roam explorer can set a start folder from its context menu; `b` jumps back to it.

Unsupported files are hidden by default. Enable `Settings > List unsupported files in explorer` while in a file-explorer view to show them.

## Less Obvious Settings

- `File > Source directories...`: configure scan-enabled and library-visible roots.
- `File > Force full rescan`: ignore fingerprints and re-read enabled source directories.
- `File > Rating tags`: sync current, current artist, all saved ratings, or retry pending tag writes.
- `File > Link roots...`: map stored library paths to readable/writable local paths. This is useful when files are mounted at a different location than the indexed path.
- `Playback > Output profile...`: configure playback output behavior, including software volume, resampling, sink release on pause, and preload settings.
- `Playback > Resume behavior...`: configure whether position and playback state are restored.
- `Settings > Track information panel...`: choose the fields shown in the right-side track information pane.
- `Settings > Album art resolution...`: change the cached artwork size for future artwork cache entries.
- `Scrobblers > Last.fm API settings...`: connect or disconnect Last.fm. Builds may embed default Last.fm credentials, but users can provide their own in the dialog.

## Notes

- The app ID is `org.11xx.muzaiten` when a matching desktop entry is installed. Local development launches without an installed desktop file skip portal app-ID registration.
- Last.fm default credentials, if used for distribution builds, are injected at build time with `MUZAITEN_LASTFM_API_KEY` and `MUZAITEN_LASTFM_SHARED_SECRET`; they are never stored in source and are XOR-obfuscated in the binary (obfuscation, not real secrecy — see [docs/distribution.md](docs/distribution.md)).
