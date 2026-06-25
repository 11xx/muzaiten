# Changelog

## [Unreleased]

### Added

- Playlist tracklists now retain their scroll position and selection when
  returning to the view. While playing a playlist, `o` jumps directly to its
  current row; playlist edits, imports, and resolved matches update the live
  playlist-backed queue without interrupting the current track.
- Drag-dropping playlist files now queues onto an import already in progress
  instead of being rejected — drop more files at any time and they fill in turn.
- A running drop-import can be stopped per playlist via a transient "Stop import"
  right-click action; deleting an importing playlist also interrupts its import.
- The playlist view shows a dashed "Drop to import" overlay while an importable
  file is dragged over it, so the page's accept-drop behavior is discoverable.

### Changed

- Scrobbler backlog-clearing actions in the menu now appear only while that
  service has pending scrobbles, with the pending count shown in the action.
- Listening history now labels manual resubmission actions as "Scrobble to
  Last.fm" and "Scrobble to ListenBrainz"; row state remains visible in the
  service columns.

### Fixed

- Closing the window to the tray (or quitting) while a playlist drop-import was
  running no longer crashes: the worker is kept alive on hide and cleanly stopped
  and joined on quit, leaving the tracks matched so far persisted.

- Shuffle now remembers playback order in both directions: after stepping back
  with Previous, Next (or auto-advance) replays the exact tracks you just
  retraced instead of re-rolling, and only resumes fresh randomness once the
  remembered trail is spent. Picking a track, reordering, or editing the queue
  still clears the trail.
- Under shuffle, the rows between the current track and a higher row played
  earlier are no longer mislabelled as "play next": any shuffle jump (forward,
  backward, or repeat-all wrap) now collapses the play-next region.
- The right sidebar's queue / track-info / album-art split is persisted reliably:
  sizes are saved only from real user drags (never transient or panel-hidden
  distributions), so the track-info pane's height survives restarts and toggles.
- The queue tables (full-screen and the right-sidebar panel) no longer draw a
  faint rounded outline on the cell you last clicked.

### Packaging

- `make build` no longer echoes build-time Last.fm credential values from `.env`
  while configuring CMake.
- AUR package metadata now includes the Python runtime required by the installed
  `muzaiten-import` helper, and lists `yt-dlp` as the optional dependency for
  YouTube playlist import enrichment.

## [2026.06.23]

### Added

- Playlist items can be reordered by mouse drag, using the same single
  insertion-line cue as the queue (now a shared, reusable behavior).
- Undo (`u`) in the playlist items pane reverts the last reorder or removal,
  re-adding removed rows; scoped to the playlist view so it doesn't leak.
- Dropping playlist files onto the playlist view now imports without freezing:
  placeholder playlists appear immediately with a spinner by their track count
  and fill live as matching runs in the background. Matching is interruptible
  with `Esc` (partial playlists are kept). Drops are accepted only in the
  playlist view.
- When the queue is playing a playlist and that playlist is open, its tracklist
  highlights the currently-playing row, linking the view to playback.
- Search results (and the add/edit modal) show a codec · sample-rate · bitrate
  badge next to each result's duration.
- Double-clicking a result in the add/edit modal confirms it, like Return.
- The listening-history modal remembers its `Ctrl`+wheel row height.
- A `muzaiten-import` companion CLI is installed alongside the binaries, with two
  subcommands: `convert` (offline conversion of CSV playlist exports to the
  import JSONL) and `youtube` (yt-dlp enrichment of a Google Takeout CSV, a
  playlist URL, or a list of links — inputs are auto-detected by kind).
- The playlist context menu offers "Resolve multi-matches (best guess)" when a
  list has unresolved multi-match rows: it picks each row's top candidate and
  marks it Approximate, flagged for a quick glance rather than a silent match.
- The playlist add/edit modal shows a reference line above the search box for
  what the row being edited expects — "Imported as:" while an import is
  unresolved, "Missing track was:" for a missing row, "Editing:" otherwise.

### Changed

- Playlist item reorder moved from `Alt`+n/p to `Shift`+n/p; the keyboard cursor
  now stays on the moved row so reorders chain.
- Playlist delete is now `Ctrl`+D (was `d`); the `Delete` key still works.
- The add/edit search modal opens nearly as wide as the main window, and returns
  the cursor to the just-edited row on close.
- Playback → Release device is available whenever a held card can be returned
  (DSD takeover or a bit-perfect profile), not only after a DSD takeover; it
  hands the card back to PipeWire and switches to shared output. The idle
  auto-release timer now applies to bit-perfect holds too.
- The Output profile dialog's two toggles are now labelled "Shared" and
  "Bit-perfect" (the trailing "mode" was redundant next to a mode selector).

### Fixed

- Restoring a tray-hidden instance with a bare `muzaiten` command (no arguments,
  same state root) no longer crashes; it raises the existing window.
- The playlist add/edit search box's built-in clear button no longer overlaps
  the right frame border.
- The track-count badge beside a drop-import's spinner now ticks up live as
  matches stream in, instead of appearing frozen until the import finished.
- Scrolling the tracklist with the mouse during a live drop-import no longer
  snaps back to the keyboard cursor on every new row.
- Removed a stray focus rectangle that appeared on clicked track-table cells.

## [2026.06.21.1]

### Fixed

- Output-profile changes now use readiness-gated PipeWire hand-offs: bit-perfect
  playback stops before a card is returned, and shared playback targets the
  freshly rebuilt physical sink instead of a transient fallback or loopback.
- Native DSD takeover from shared output promotes the session to bit-perfect,
  keeping subsequent PCM playback on the already-acquired device.
- Restoring an output profile preserves the current track position and
  play/pause intent, including a delayed Take over after a busy-device error.

## [2026.06.21]

### Added

- Added DSD library scanning for `.dsf` and `.dff` files.
- Added native bit-perfect DSF playback to a direct ALSA device, including a
  shared-output PipeWire-takeover prompt and a Playback → Release device action.

### Changed

- DSD plays through the normal GStreamer path as PCM when resampling is enabled,
  so shared output remains seamless without taking over the device.
- A declined or timed-out takeover skips the contiguous native-DSD block once,
  then prompts again after another track successfully starts.

### Fixed

- Tray-hidden playback retains the takeover controller until it can return an
  owned device to PipeWire, rather than leaking or interrupting native output.

### Packaging

- Documented `gst-plugins-bad` and `gst-libav` as optional DSD dependencies.

## [2026.06.20]

### Added

- Playlist import can create one playlist per dropped file, or import into the
  playlist currently open.
- The import preview now streams matching results while they are found and
  offers an exact-only mode.

### Changed

- Multi-match triage can explicitly clear an unwanted match, and Add Song uses
  the saved search-ranking configuration.
- Matching now preserves source strings, timestamps, external IDs, provenance,
  and valid duplicate library copies for safe repeat imports.

### Fixed

- Fuzzy import matching better rejects confident-but-wrong matches while
  recognizing title separators such as `&`, `and`, and `+`.

## [2026.06.18]

### Changed

- Refined responsive table restoration, headers, panel borders, separators,
  and live theme updates for a more consistent native Qt appearance.
- Playlist sidebar sizing is now persisted without proportional-stretch drift.

### Fixed

- Table relayout and player-bar stylesheet changes no longer leave stale visual
  state behind.

### Added

- Added deterministic offscreen screenshot and typed-search demo capture tools
  used for publishing documentation.

## [2026.06.17]

### Changed

- Moved playback, MPRIS, IPC, scrobbling, tray ownership, and window lifetime
  under `AppCore`, allowing them to operate without a live widget tree.
- Closing to the tray now releases and rebuilds the main window safely while
  preserving active scan and playback state.

### Fixed

- Restoring the window correctly resumes playback and persists state on tray
  quit; the tray icon appears reliably after construction.

### Performance

- SQLite caches are shrunk after the window is released.

## [2026.06.15]

### Added

- Added per-output-mode device selection and bit-perfect PipeWire takeover.
- Added configurable track-information fields, order, display conditions, and
  resettable settings.

### Changed

- Extracted reusable track-info, reorderable-table, and settings components;
  album-grid sizing now packs cleanly at exact viewport widths.

### Fixed

- Fixed track-info condition editing, queue metadata enrichment, album-grid
  wrapping/flicker, and playback resume after bit-perfect takeover.

## [2026.06.13]

### Added

- Added repeat and shuffle modes, including library-wide shuffle, MPRIS state,
  and a gapless-safe next-track policy.
- Added fold-aware search across diacritics, non-Latin scripts, and Japanese
  readings, with a persistent on-disk index cache.
- Added `muzaitenctl search`, including streaming output and an interactive fzf
  picker, plus an IPC command for enqueuing multiple tracks.

### Changed

- Search indexing now streams and loads incrementally, keeping large-library
  startup responsive.

### Fixed

- Queue and playlist playback now keep their source identity and UI state in
  sync during play and restore.

## [2026.06.11]

### Added

- Added a dedicated playlist database and view, song search, sorting,
  M3U8/CSV export, saved-queue snapshots, and playlist-aware queue actions.
- Added playlist import from track lists and YouTube/YT Music via `yt-dlp`, with
  candidate matching, comments, and bulk import UI.
- Added local listen history, shared offline scrobble buffering, a local command
  socket, and the `muzaitenctl` client.

### Changed

- System-tray behavior, single-instance handling, transport state, and queue
  persistence were consolidated around the new playback core.

### Fixed

- Fixed playlist menu availability, progress updates during transitions, IPC
  startup races, external-volume persistence, and stale queue metadata rows.

## [2026.06.06]

### Added

- Added a faster scanner pipeline: parallel enumeration, placeholder-first
  inserts, lazy background tag filling, adaptive throttling, bit-depth capture,
  and path-metadata guessing.
- Added scan progress and safer missing-track handling, including queue
  play-next behavior and broader lazy folder-art fallback.

### Changed

- Tuned SQLite for burst scans and queries, batching scan updates and removing
  redundant per-track work.

### Fixed

- Rating updates now patch only the affected UI rows, and manual queue jumps no
  longer incorrectly mark skipped tracks as play-next.

## [2026.06.02]

### Added

- Added freedesktop installation, release packaging, AUR package definitions,
  user-space install/uninstall support, and distribution documentation.
- Added a full-screen queue, responsive table-column policies and configuration,
  incremental queue search, and a drag-and-drop panel-order editor.

### Changed

- Replaced RAM-copy audio preload with kernel read-ahead and reduced memory use
  in the search index.

### Fixed

- Hardened GStreamer preload/resume, MPD protocol parsing and cancellation,
  artwork decoding, scrobbling failures, and cross-thread scan cancellation.

## [2026.05.31]

### Added

- Added an fzf-style library search view with ranking, glob exclusions,
  keyboard-driven navigation, and configurable main-panel key bindings.
- Added live Qt scheme switching and improved album-grid/table navigation.

### Changed

- Search was refactored around a reusable matcher and optimized for lazy,
  low-allocation highlighting.

### Fixed

- Fixed restored paused-session audio blips, Last.fm authentication and form
  submission edge cases, queue play-next state, and popup-menu styling crashes.

## [2026.05.29]

### Added

- Added Last.fm scrobbling alongside ListenBrainz, with authentication, tests,
  now-playing updates, and persisted session behavior.
- Added a parallel incremental scan pipeline, compressed full-tag storage,
  SQLite artwork cache, lazy embedded-art extraction, and missing-track state.
- Added a keyboard-oriented file explorer with bookmarks, metadata columns,
  sorting, and configurable row density.

### Changed

- Queue layout, sorting, play-next controls, album-grid ordering, and overlay
  scrollbars were substantially refined.
- Adopted XDG-compliant data/state/cache/config storage with explicit path
  overrides and an INI configuration layer.

### Fixed

- Playback reliably releases an output device on pause or queue end and avoids
  stale queue-state drift during structural changes.

## [2026.05.25]

### Added

- Added MPRIS integration, configurable local source roots, source-directory
  settings, a reusable file explorer, queue drag reordering, and click-to-seek.
- Added explicit models for track and queue tables, Ctrl+wheel sizing, and
  persistent playback resume state.

### Changed

- Improved library scan responsiveness through deferred artwork loading and
  incremental album-grid population.

### Fixed

- Fixed pending rating visibility/synchronization, album-grid selection flicker,
  queue drop boundaries, and stale fallback artwork.

## [2026.05.24]

### Added

- Added a GStreamer playback backend with selectable output profiles.
- Added read-only MPD configuration parsing, protocol client, background library
  metadata import, and source-path lookup UI.
- Added track-information and compact-menu modes, queue context actions, and
  persisted explorer/queue layout state.

### Fixed

- Hardened prepared playback transitions and restored queue switching while
  exposing the imported MPD library.

## [2026.05.23]

### Added

- Established the application scaffold, SQLite library schema, tests, and a
  read-only local-library browser with folder artwork.
- Added background library scanning, an album grid, a persistent player bar,
  basic playback, a global queue, and rating controls.
- Added initial ListenBrainz scrobbling support.
