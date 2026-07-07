# muzaiten

muzaiten is a native Linux music player focused on local-library browsing, queue-based playback, album artwork, ratings, and scrobbling.

The project is in early development. It currently uses C++26, Qt 6 Widgets, SQLite, TagLib, GStreamer, Qt DBus, Qt Network, and zstd.

See [CHANGELOG.md](CHANGELOG.md) for release notes and reconstructed development milestones.

## Screenshots

<table>
  <tr>
    <td align="center" width="50%">
      <img alt="Library browser" src="demo-screens/KvGnome/01-library.png#gh-light-mode-only" width="380">
      <img alt="Library browser" src="demo-screens/KvGnomeDark/01-library.png#gh-dark-mode-only" width="380">
      <br><sub><b>Library</b> — artist sidebar, album grid, track table</sub>
    </td>
    <td align="center" width="50%">
      <img alt="Search demo" src="demo-screens/KvGnome/02-search.png#gh-light-mode-only" width="380">
      <img alt="Search demo" src="demo-screens/KvGnomeDark/02-search.png#gh-dark-mode-only" width="380">
      <br><sub><b>Search</b> — fzf-style, romaji ↔ kana/kanji, live results</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img alt="Playback queue" src="demo-screens/KvGnome/03-queue.png#gh-light-mode-only" width="380">
      <img alt="Playback queue" src="demo-screens/KvGnomeDark/03-queue.png#gh-dark-mode-only" width="380">
      <br><sub><b>Queue</b> — reorderable, play-next priority, missing-track marks</sub>
    </td>
    <td align="center" width="50%">
      <img alt="Playlists" src="demo-screens/KvGnome/04-playlists.png#gh-light-mode-only" width="380">
      <img alt="Playlists" src="demo-screens/KvGnomeDark/04-playlists.png#gh-dark-mode-only" width="380">
      <br><sub><b>Playlists</b> — import &amp; match, drag-drop batch import</sub>
    </td>
  </tr>
</table>

## Features

- Local library scanner with source-directory management, incremental rescans, and missing-file marking.
- Fast library search that matches by sound/shape across scripts — diacritics (`cafe`↔`Café`), Greek/Cyrillic/Turkish, and Japanese romaji↔kana/kanji (`sanshin no hana`→`三線の花`) — backed by an on-disk cache, in-app and via the `muzaitenctl` CLI (with an fzf picker).
- Local, fully offline recommendation engine: Start Radio from any track (or
  artist), a taste-aware Radio shuffle mode, Rediscovery and Deep-cuts mixes,
  and explainable picks — every queue entry can show why it was chosen.
  Scoring blends genres (alias-curated, rarity-weighted), era, ratings, and
  listening history, plus content-aware signals when `features.sqlite` is
  built: tempo/energy proximity from clean-room DSP analysis and CLAP
  audio-embedding similarity, with duplicate copies resolved to the best
  quality. Genre aliases/ignored genres can be curated from the Radio menu.
  Weights are runtime-tunable, saveable as profiles, and
  `radio-learn` can suggest a profile learned from your own skips.
- Album artist sidebar, album grid, artist track table, and queue sidebar.
- Playlists in a dedicated database: import from a pasted tracklist, `m3u`/`m3u8`, `csv`, or `jsonl`/`ndjson`, or fetch a YouTube / YT Music playlist via `yt-dlp`; tracks resolve by sound/shape matching with a live-streaming match preview and multi-candidate triage. Entries keep a metadata snapshot so playlists survive rescans and remember tracks that go missing.
- Library file explorer and free-roam file explorer, with keyboard-oriented navigation profiles.
- Cached album artwork from folder images and embedded artwork.
- Queue playback through a GStreamer backend, with configurable output/resume behavior. DSF DSD files play natively and bit-perfect to a selected direct ALSA device when resampling is off; enabling resampling decodes DSD to PCM for normal shared output.
- User ratings with optional MusicBee-compatible tag sync back to audio files.
- ListenBrainz and Last.fm scrobbling, including now-playing updates and completed-listen submissions.
- MPD metadata import for browsing MPD libraries alongside local sources.
- Persistent UI state for splitters, table columns, row heights, sorting, queue state, playback resume, and explorer state.

## Library Safety

muzaiten indexes and plays local files without moving, renaming, deleting, or writing artwork into the music library.

Rating edits are stored in the application database immediately. For local files, muzaiten can also write those ratings back to tags through `Library > Rating tags` or the automatic pending-write path after a rating change. Tag writes are narrow in scope: they update the rating field, verify the result by re-reading the file, and keep failed or unwritable writes pending for retry.

Scans do not traverse symlinks. Missing files are marked missing rather than deleted from the database until you explicitly choose `Library > Remove missing tracks`. Tracks that go missing are shown with a red `×` wherever they appear in the queue and in playlists; `Library > Remove missing tracks` drops them from the library and the queue but keeps (and marks) playlist entries, so a playlist still remembers them. Right-clicking a missing row offers both a scoped "Remove missing track(s)" (from the queue/playlist you're in) and the global "Remove all missing tracks from library".

## Build

Dependencies:

- CMake 4.x
- Ninja
- C++26-capable compiler
- Qt 6.11+ with Core, DBus, Gui, Multimedia, Network, Widgets, Sql, and Test modules
- TagLib 2.x
- GStreamer 1.x with audio and pbutils modules
- zstd

Optional for DSD playback: `gst-plugins-bad` and `gst-libav`. Only the `.dsf`
container is playable (no `.dff`/DSDIFF demuxer ships with GStreamer/ffmpeg).
Native DSF output requires selecting a direct-capable device in `Playback >
Output profile`; shared output with **Allow resampling** decodes DSD to PCM.

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

From source. `make install` is user-space by default — it installs into
`~/.local` and needs no root:

```sh
make build CMAKE_BUILD_TYPE=Release
make install                       # -> ~/.local (no sudo)
sudo make install PREFIX=/usr      # system-wide instead

make uninstall                     # reverse it (mirrors the PREFIX you used)
```

Installing drops the `muzaiten` binary, the `muzaitenctl` CLI, and the
`muzaiten-import` playlist-conversion tool (`convert`/`youtube` subcommands),
plus a desktop entry, scalable icon, and AppStream metadata into the prefix.
`make uninstall` removes exactly those files
(it reads `install_manifest.txt`, so it cleans whichever prefix you installed
into; use `sudo` for a system prefix). (`make` here is only a thin alias for the
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

- `1`: queue view (press again to reveal the currently playing row).
- `2`: library panels view (artist sidebar, album grid, track table).
- `3`: toggle between the library explorer and free-roam file explorer.
- `4`: library search view (fzf-style, searches the entire library including MPD tracks; press again to rebuild the index).
- `5`: playlist management view.
- `o`: find the current track in the active library view.
- `r` / `s`: cycle repeat / shuffle modes.
- Queue context menu: play a row, find that row in the library, open its containing directory, remove rows, clear play-next priority, or clear the queue.
- `Ctrl+scroll` over the queue or file explorers: adjust row height.
- Context menus on tables, album grid, queue header, and explorer rows expose sorting, visibility, rating, and queue actions.

## Library Search

Press `4` to open the search view. Type to filter interactively; all terms AND together in any order.

Matching is by **sound/shape, not encoding**: both the query and the library are folded to a lowercase ASCII-leaning form before matching, so accents and scripts don't get in the way. `cafe` finds `Café`, `bjork` finds `Björk`, Greek/Cyrillic/Turkish transliterate (`σωκρατης`↔`sokrates`), and Japanese matches by romaji — kana and common kanji romanize (`sanshin no hana` or `san shin no ha na` → `三線の花`), with Picard/MusicBrainz `*sort` reading tags filling in proper-noun readings (`utada` → 宇多田ヒカル). Typing the original script still matches too.

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
| `F5` (or re-press `4`) | Rebuild the search index |
| `Ctrl+scroll` | Adjust result row height |
| Double-click | Play now |
| Right-click | Context menu: play / queue / play next / find in library / open directory |

The result list always keeps a highlighted cursor you move with the arrow or
`Ctrl+P`/`Ctrl+N` keys while the search box keeps focus, fzf-style. `Esc`/`Ctrl+G`
first clears the query, then (when already empty) releases the text box so `1`
and `2` switch views; `/` jumps back into the box. The folded index is cached on
disk, so opening search is near-instant on a warm cache; a cold or stale cache
streams results in as it builds (and refreshes quietly in the background, shown
by a small "updating index" note). Deleting the cache file is harmless — it
rebuilds on next open.

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

## Playlists

Press `5` for the playlist management view: a playlist list on the left, the
selected playlist's items on the right, keyboard-first (`h`/`l` switch panes,
`j`/`k`/`n`/`p` move, `Enter` plays the focused playlist or item). Playlists live
in their own database, separate from the scanned library, and each item carries a
metadata snapshot plus the search query that produced it, so a playlist survives
rescans and keeps remembering tracks even after they go missing.

**Importing.** Add to an existing playlist or create a new one from:

- a pasted tracklist (one `Artist - Title` per line),
- an `m3u` / `m3u8`, `csv`, or `jsonl` / `ndjson` file (drag-drop a batch of files
  to get one playlist per file), or
- a YouTube / YT Music playlist URL, fetched with `yt-dlp` (no account or paid
  middleman; the row is greyed out with an explanation when `yt-dlp` is missing).

Each line is resolved against the library by the same sound/shape matcher used by
search, so `Artist - Title` lines match regardless of script or accents. Matches
**stream into the preview live** as they resolve. An **Exact only** toggle
disables fuzzy/relaxed guessing for stricter imports. Rows that resolve to several
candidates (or to a low-confidence best guess) are flagged for a quick triage
pick — leave as-is, choose one of the close candidates, or clear the match —
before committing. The original import text is preserved verbatim, so a row can be
re-matched later without losing where it came from.

## Command-line client (`muzaitenctl`)

`muzaitenctl` is a companion CLI installed alongside the app. Transport and
queue commands talk to a running instance over its IPC socket (resolved from the
same `MUZAITEN_*` environment as the app, so they target the matching instance):

```sh
muzaitenctl status            # current track + player state (--json for raw JSON)
muzaitenctl play | pause | play-pause | next | prev | stop
muzaitenctl seek +30          # relative seconds, or absolute mm:ss
muzaitenctl volume 70         # absolute 0-100, or +/-N
muzaitenctl rate 4            # 0-5 stars (rate raw <0-100>, rate clear)
muzaitenctl queue             # list the queue; `queue <n>` jumps to a row
muzaitenctl enqueue [--play|--next] <path...>   # add files to the queue
muzaitenctl play-file <path>  # append a file and play it
muzaitenctl scrobble-backfill <listenbrainz|lastfm>   # import history / sync play counts
muzaitenctl scrobble-backfill status | cancel         # progress, or stop the running import
muzaitenctl scrobble-backfill reset <listenbrainz|lastfm>   # re-walk history behind a completed import
muzaitenctl radio-weights get | set '<json>'                 # inspect or replace active radio scoring weights
muzaitenctl radio-weights save <name> | apply <name> | list  # named radio tuning profiles
muzaitenctl radio-learn --dry-run                            # suggest weights learned from your radio skips
muzaitenctl features-status                                  # features.sqlite coverage report
muzaitenctl duplicate-groups --min-size 2                    # inspect detected duplicate copies
muzaitenctl pin-copy <group-id> <path>                       # prefer one copy for radio (unpin-copy undoes)
```

The same backfill controls live under `History > Scrobblers` in the app. A
completed ListenBrainz import stops early once it pages into the already-imported
range, so listens later added *behind* that range are never revisited. Run
`scrobble-backfill reset listenbrainz` to clear the completed marker and force the
next import to re-walk full history (imported-listen dedup keeps re-walks safe);
`reset` is refused while a backfill is running.

`search` is different: it runs **entirely client-side**, opening the library
database and the shared folded-index cache directly, so it works whether or not
the app is running:

```sh
muzaitenctl search sanshin            # TSV: path  title  artist  album  date  ms  rating
muzaitenctl search --plain utada      # human-readable blocks
muzaitenctl search --limit 5 --json jazz
muzaitenctl search --fuzzy nhuage     # fuzzy instead of exact substring
muzaitenctl search --refresh          # rebuild the cache; --clear-cache to drop it
muzaitenctl semantic-search "melancholic shoegaze" --limit 10 --json
```

With no query in a terminal (and `fzf` installed) `search` launches an **fzf
picker** over the whole library — multi-line rows, romaji matches kanji, `Enter`
queues the selection and `Alt+Enter` plays it (via `enqueue`). Piped or without
fzf, a bare `search` dumps the whole library as TSV. First run builds the cache
(a few seconds); later runs are instant.

`semantic-search` also runs client-side, but requires `features.sqlite` to
exist. Build or refresh it with the C++ indexer that is built alongside the app:

```sh
muzaiten-index scan --library ~/.local/share/muzaiten/library.sqlite \
  --features ~/.local/share/muzaiten/features.sqlite --json
muzaiten-index status --features ~/.local/share/muzaiten/features.sqlite --json
```

The indexer computes decoded-audio identity, Chromaprint content groups, and
clean-room DSP scalars in one canonical decode pass. CLAP embeddings are added
separately by the optional Python tool in `tools/embedder`:

```sh
cd tools/embedder
uv run muzaiten-embed scan --features ~/.local/share/muzaiten/features.sqlite
uv run muzaiten-embed neighbors --features ~/.local/share/muzaiten/features.sqlite
```

`semantic-search` requires `features.sqlite` to contain those CLAP embeddings.
It embeds
the text query through `muzaiten-embed query`, ranks content groups by cosine,
and prints the preferred library copy for each group.

Radio tuning commands (`radio-weights`, `radio-genre`, `genre-alias`,
`genre-report`) also run client-side against the library database. Weight
changes are validated before writing and take effect on the next radio session.
The same weight editor and genre curation live in-app at
`Radio > Scoring weights...` and `Radio > Genre curation...`.
`radio-learn` fits a small model to your recorded radio picks and early skips
and is suggestion-only: it saves a `learned-YYYYMMDD` profile for review and
never touches the active weights (apply it explicitly with
`radio-weights apply`). It refuses to run until enough labeled listening data
exists (about 200 radio picks). The full `features.sqlite` layout — identity,
scalar, and embedding tables, with units and caveats — is documented in
`docs/features-schema.md`.

## File Explorer Notes

The file explorers support selectable keybinding profiles from the explorer context menu:

- Vim-style: `j/k/h/l`, `Space` play, `a` add, `p` play next, `i` import, `f` open containing directory, `b` jump to start folder, `~` home.
- Emacs/Dired-style: `n/p`, `Space` play, `s` add, `!` play next, `i` import, `f` open containing directory, `b` jump to start folder, `~` home.

Backspace remains "up/back directory". The free-roam explorer can set a start folder from its context menu; `b` jumps back to it.

Unsupported files are hidden by default. Enable `View > List unsupported files in explorer` while in a file-explorer view to show them.

## Less Obvious Settings

- `Library > Source directories...`: configure scan-enabled and library-visible roots.
- `Library > Force full rescan`: ignore fingerprints and re-read enabled source directories.
- `Library > Rating tags`: sync current, current artist, all saved ratings, or retry pending tag writes.
- `Library > Audio analysis`: inspect `features.sqlite` coverage and duplicate-copy groups.
- `Library > Link roots...`: map stored library paths to readable/writable local paths. This is useful when files are mounted at a different location than the indexed path.
- `Radio > Scoring weights...`: edit radio scoring weights, manage named profiles, or save a learned profile suggested from radio skip history.
- `Radio > Genre curation...`: edit genre aliases, ignored radio genres, and the folded vocabulary used by radio matching.
- `Playback > Output profile...`: configure playback output behavior, including software volume, resampling, sink release on pause, and preload settings.
- `Playback > Resume behavior...`: configure whether position and playback state are restored.
- `Settings > Track information panel...`: choose the fields shown in the right-side track information pane.
- `Settings > Album art resolution...`: change the cached artwork size for future artwork cache entries.
- `Scrobblers > Last.fm API settings...`: connect or disconnect Last.fm. Builds may embed default Last.fm credentials, but users can provide their own in the dialog.

## Notes

- The app ID is `org.11xx.muzaiten` when a matching desktop entry is installed. Local development launches without an installed desktop file skip portal app-ID registration.
- Last.fm default credentials, if used for distribution builds, are injected at build time with `MUZAITEN_LASTFM_API_KEY` and `MUZAITEN_LASTFM_SHARED_SECRET`; they are never stored in source and are XOR-obfuscated in the binary (obfuscation, not real secrecy — see [docs/distribution.md](docs/distribution.md)).
