# muzaiten

A native Linux music player for people with a local library and opinions
about it. Fast script-agnostic search, careful queue-first playback, and a
**fully offline recommendation engine** that learns your taste without
sending a byte anywhere.

Built with C++26, Qt 6 Widgets, SQLite, TagLib, and GStreamer. In active
development — see [CHANGELOG.md](CHANGELOG.md) for the detailed record.

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

## What it does

**Search that ignores encoding.** Queries match by sound/shape across
scripts: `cafe` finds `Café`, `sanshin no hana` finds `三線の花`,
`σωκρατης` finds `sokrates`. fzf-style in-app, and from the terminal via
`muzaitenctl search` (with an actual fzf picker). Field filters, quality
filters, configurable ranking. → [docs/search.md](docs/search.md)

**Radio, entirely yours.** Start Radio from any track or artist, taste-aware
Radio shuffle, Rediscovery and Deep-cuts mixes — scored from your genres,
eras, ratings, and listening history, and every pick can explain itself.
Analyze your library's audio (one menu action) and picks also follow tempo,
energy, and how tracks actually *sound* (CLAP embeddings), with duplicate
copies resolved to the best quality. Tune the weights, curate genres, or
let it learn from your skips — suggestions only, never behind your back.
→ [docs/radio.md](docs/radio.md)

**A queue you can trust.** Play-next priority, saved/restorable queues,
playlist-linked mode with both-ways edits, and missing files marked instead
of silently dropped. Playlists import from pasted tracklists, m3u/csv/jsonl,
or YouTube playlists via `yt-dlp`, resolving against your library with live
match preview and triage. → [docs/playlists.md](docs/playlists.md)

**And the rest:** two file explorers with Vim/Dired key profiles
([docs/controls.md](docs/controls.md)) · ratings with MusicBee-compatible
tag sync · ListenBrainz + Last.fm scrobbling with full history backfill ·
MPD metadata import · bit-perfect DSD (`.dsf`) to direct ALSA devices ·
persistent UI state throughout.

**Your library is safe.** muzaiten never moves, renames, deletes, or writes
artwork into your music. The one deliberate exception — opt-in rating tag
writes — is narrow, verified, and documented in
[docs/data-safety.md](docs/data-safety.md).

## Install

Arch Linux (AUR): `muzaiten-bin` (prebuilt, bundled Last.fm key) or
`muzaiten-git` (builds latest, bring your own key).

From source — user-space by default, no root needed:

```sh
make build CMAKE_BUILD_TYPE=Release
make install                       # -> ~/.local
sudo make install PREFIX=/usr      # system-wide instead
make uninstall                     # reverse it (mirrors the PREFIX used)
```

This installs the app, the `muzaitenctl` CLI, the `muzaiten-features` audio
analyzer, and the `muzaiten-import` playlist tool, plus desktop entry and
icon. Packaging details and the Last.fm credential model:
[docs/distribution.md](docs/distribution.md).

Semantic/audio-similarity analysis is disabled by default and separately
installed. There is no Arch/AUR provider package yet; Arch and other Linux
users install the published Python tool directly. The app never runs uv or
downloads a model without consent:

```sh
uv tool install 'muzaiten-features-clap[model]' --torch-backend auto
```

Setup, model provenance, CPU/CUDA choices, and refresh behavior:
[docs/semantic-analysis.md](docs/semantic-analysis.md).

## Build

Dependencies: CMake 4.x, Ninja, a C++26 compiler, Qt 6.11+ (Core, DBus,
Gui, Multimedia, Network, Widgets, Sql, Test), TagLib 2.x, GStreamer 1.x
(audio + pbutils), zstd. Optional for DSD: `gst-plugins-bad` + `gst-libav`.

```sh
make build        # configure + compile (wraps CMake/Ninja)
make test         # full suite
make dev          # build + run against an isolated ./dev-state profile
```

Plain CMake works too: `cmake -S . -B build -G Ninja && cmake --build build`.

## Quick orientation

Views on number keys: `1` queue · `2` library · `3` file explorers ·
`4` search · `5` playlists. `o` finds the playing track; `r`/`s` cycle
repeat/shuffle. Right-click anything — tracks, albums, artists, and empty
space all have menus; keys are rebindable (`Settings > Keybinds…`).
Full tour: [docs/controls.md](docs/controls.md).

`muzaitenctl` drives a running instance (transport, queue, rating) and
works standalone for search, radio tuning, audio-analysis inspection, and
scrobble backfill. Full reference: [docs/cli.md](docs/cli.md).

## Documentation

| Doc | Covers |
|-----|--------|
| [docs/search.md](docs/search.md) | Search view, query syntax, ranking, CLI search |
| [docs/radio.md](docs/radio.md) | The recommendation engine: sessions, tuning, curation, audio analysis |
| [docs/playlists.md](docs/playlists.md) | Playlist view, importing, queue linking |
| [docs/controls.md](docs/controls.md) | Keys, context menus, explorer profiles |
| [docs/cli.md](docs/cli.md) | `muzaitenctl` reference and related binaries |
| [docs/state-and-paths.md](docs/state-and-paths.md) | XDG paths, overrides, config file |
| [docs/data-safety.md](docs/data-safety.md) | The library-safety contract |
| [docs/features-schema.md](docs/features-schema.md) | `features.sqlite` layout (analysis data) |
| [docs/semantic-analysis.md](docs/semantic-analysis.md) | Optional CLAP provider, model consent, provenance |
| [docs/distribution.md](docs/distribution.md) | Packaging, releases, credential model |

## Notes

- App ID is `org.11xx.muzaiten` when a matching desktop entry is installed;
  development launches without one skip portal app-ID registration.
- Distribution builds may embed default Last.fm credentials at build time
  (`MUZAITEN_LASTFM_API_KEY` / `MUZAITEN_LASTFM_SHARED_SECRET`); they are
  XOR-obfuscated, never in source, and users can always supply their own —
  see [docs/distribution.md](docs/distribution.md).
