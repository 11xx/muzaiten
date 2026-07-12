# Changelog

## [Unreleased]

### Added

- `muzaitenctl semantic-search` now memoizes text query vectors in a
  persistent cache (`semantic-query.sqlite` under the cache directory) keyed
  by the active semantic generation's model identity, so repeating a query
  skips the CLAP provider process and its cold ONNX text-session load
  entirely. A new `--no-cache` flag forces a fresh provider embedding.

### Changed

- Semantic text queries no longer run a separate provider capability
  handshake before the query itself: the query trusts the first resolvable
  provider candidate and falls back to full discovery only on failure,
  removing one Python provider process (and its startup latency) from every
  uncached search.

### Fixed

- Semantic search ranking now caps how many candidate groups it probes for
  library membership. A stale `features.sqlite` paired with a pruned library
  could previously send one search into minutes of full-store probing; a
  healthy store fills the requested limit within the first few candidates and
  is unaffected.

- Gapless playback now commits each queue advance from the new stream's
  serialized event at the audio sink instead of waiting for a successful
  position query. A slow or temporarily unqueryable source can no longer leave
  the UI on the outgoing row, skip a queue row at the following boundary, or
  make the successor appear to start many seconds into its timeline. Very
  short or fully cached tracks also preserve the first pending handoff when
  playbin asks for another URI before that successor becomes audible, and a
  sink-confirmed start is committed before any immediately following EOS or
  recovery event. Output-pipeline rebuilds and native-DSD switches now
  invalidate old handoff callbacks, seek watchdogs, source identities, and
  read-ahead state before tearing the retired pipeline down. Pause, seek,
  error, and EOS handling first commit any successor already confirmed at the
  sink; pausing during the earlier armed-only window restores the outgoing
  source and preserves the prepared successor instead of resuming the wrong
  URI at an inherited position.
- An unresolvable queue row now advances through the existing bounded skip
  path instead of leaving the prior file audible under the new row's
  selection. Consecutive missing rows are skipped without recursion, an
  all-unresolvable repeat queue stops, and pressing Next on the final row no
  longer restarts that same track.
- Removing a different queue row or clearing the queue while keeping the
  current track no longer re-presents unchanged playback and resets the
  progress bar and MPRIS position to zero. Row removal also ignores invalid
  indices without blocking valid ones, and deleting the playing row reports
  its successor as an automatic transition before playback starts.
- Play and Play/Pause now restart the current row through a fresh pipeline
  after an unrecoverable backend error instead of trying to resume the failed
  GStreamer graph indefinitely.
- Non-gapless natural advances, repeat-one restarts, and unplayable-row skips
  now report themselves as automatic instead of user-initiated. Queue views no
  longer reveal those rows as if clicked, and playback telemetry no longer
  mislabels them as manual starts.
- Queue reordering, removal, Play Next insertion, clearing, and repeat/shuffle
  policy changes now stabilize an in-progress gapless boundary first. An
  already-audible successor is committed before indices move; an armed-only
  successor is canceled back to the outgoing source, so GStreamer can never
  start a file that PlayerCore now assigns to a different row.

## [2026.07.12]

- The optional CLAP provider now serves the same stable embeddings through
  fp32 ONNX Runtime audio and text graphs instead of loading PyTorch,
  torchvision, librosa, numba, and LAION-CLAP at runtime. A one-time isolated
  conversion keeps the pinned checkpoint identity, saves the exact tokenizer,
  verifies a hashed artifact manifest, and preserves existing semantic stores
  without a rescan; model preparation now has its own live progress phase.
  Status checks and text queries validate the artifact manifest structurally
  instead of re-hashing hundreds of megabytes per invocation. Model download
  now fetches the hosted, hash-verified bundle from
  `muzaiten/clap-htsat-base-onnx` on Hugging Face (about 790 MB of converted
  artifacts), so end users need neither the 2.35 GB checkpoint nor the
  conversion stack; converting locally from the pinned checkpoint remains
  supported through the `[convert]` extra.
- The semantic provider is a real Arch package: with provider 2026.7.12 on
  PyPI (ONNX runtime) and the hosted bundle live, the rewritten distro gate
  reports candidate, `muzaiten-features-clap` is published on the AUR from
  the PyPI sdist (clean-chroot built, namcap-reviewed, model never
  packaged), and `muzaiten-bin`/`muzaiten-git` advertise it again as an
  optional dependency. The gate now measures the ONNX reality (runtime
  dependencies resolvable from official repositories, provides variants, or
  the AUR; an ONNX `[model]` extra on the published provider; a configured
  hosted bundle) instead of the retired NumPy/Numba/LAION-CLAP chain.

## [2026.07.11]

- UI copy sweep: em-dashes are gone from status messages, tooltips, dialog
  text, headers, and CLI errors in favor of plain punctuation; the "—"
  empty-value placeholder in tables and the import-format separators are
  unchanged. The playlist view's empty-state key cheat line now matches the
  real bindings (Ctrl+D/Delete to delete, Shift+j/k to reorder items, the new
  z fold toggle) instead of advertising keys that never worked.
- New radio glyphs, vectorized from reference artwork into palette-aware
  drawing code: the radio-session indicator in the player bar shows
  headphones over a spinning vinyl instead of the generic wifi arcs, and the
  Radio shuffle mode (the cycle stop after Library shuffle) combines that mark
  with crossing arrows instead of reusing Library shuffle's icon.
- Playlist view polish: the playlist list supports multi-selection
  (Shift/Ctrl+Click, `Shift+j`/`Shift+k`) with a bulk context menu — play,
  enqueue, play next, or delete several playlists and saved queues at once
  behind a single combined confirmation (`Ctrl+D`/`Delete` works on the whole
  selection too). Saved queues are now grouped into three foldable sections —
  manual saves, auto-saved snapshots, and radio sessions, in that order —
  folded by default; click a header or press `z` to toggle, and unfolded
  groups persist. The sidebar uses the same overlay scrollbar as the
  tracklist, and the redundant heading above the panes now appears only when
  the selected name is too long to read un-elided in the sidebar.
- The Back button no longer counts as a radio "early skip": pressing Next and
  then Back could cross the three-early-skips threshold and reroll — i.e.
  destroy and regenerate — the not-yet-played radio picks, even though going
  back to re-hear a track is navigation, not rejection. Only genuine forward
  skips feed the reroll streak now.
- Stepping back with the Back button no longer marks the song just departed
  with a spurious "play next" badge; like an explicit jump, Back clears any
  pending play-next batch instead of mis-spanning it over rows it stepped over.
- Native and AUR packages now declare the required Chromaprint runtime used by
  `muzaiten-features`, so audio analysis does not depend on an undeclared host
  library.
- Prebuilt release executables are fully RELRO-hardened and stripped of
  build-only symbol tables before archive creation.
- The audio-analysis executable is now `muzaiten-features`, and the optional
  CLAP provider is packaged independently as `muzaiten-features-clap` from
  `tools/features-clap`. The retired executable, distribution, and import names
  are not installed or retained as compatibility aliases.
- The CLAP provider now speaks a versioned one-request JSON/JSONL protocol and
  exposes a stable feature revision. Model inspection and download are separate:
  scans and text queries never download weights implicitly, while explicit
  downloads stream byte progress, verify SHA-256, and install atomically.
- `muzaiten-features refresh` now owns path resolution, persisted opt-in policy,
  provider discovery/handshake, serial native and semantic phases, cancellation,
  and `features.sqlite.lock`. The GUI consumes structured JSONL, exposes the
  semantic opt-in, provider setup, and explicit model-download consent, and
  reports missing-provider/model, cancellation, and operational failures.
- `features.sqlite` schema v5 scopes embeddings and neighbors to one active
  provenance generation. Readers hide stale generations and reject stale
  neighbors; matching legacy CLAP corpora migrate in place while mixed or
  unknown semantic rows remain inactive. Semantic search now obtains query
  vectors through `muzaiten-features` and verifies the generation fingerprint.
- Interrupted schema-v5 migrations now resume transactionally without duplicate
  generations or abandoned legacy tables. Provider status allows enough time
  for checkpoint verification and device probing, and third-party model-loading
  messages are kept off the provider's JSONL stdout channel.
- Python provider releases use a manual, environment-scoped Trusted Publishing
  workflow: unprivileged jobs build and audit the distributions, while only the
  selected TestPyPI or PyPI publication job receives a short-lived OIDC token.
  The installation guide also explains how to replace a checkout-backed uv tool
  with the published package without removing model or feature data. The
  distribution runbook records a deterministic, build-once promotion contract
  for future automation while keeping production approval human-gated.
- The Trusted Publishing workflow now pins current Node.js 24 releases of the
  GitHub checkout and artifact actions, removing the runner's Node.js 20
  deprecation warnings without changing the package or publication boundary.
- When a Python provider release accompanies a native release, it shares the
  native release's UTC date using the PEP 440-normalized spelling without
  leading zeroes (`2026.7.11` for native tag `2026.07.11`). Native-only releases
  keep using the latest compatible provider instead of republishing it.

## [2026.07.10]

### Added

- The CLAP provider now makes its inference device explicit: scan/query accept
  `auto`, `cuda`, or `cpu`, report the selected device, and fail fast when CUDA
  is explicitly requested but unavailable.
- Feature-fill progress is now first-class: after `phase features`,
  `muzaiten-features` reports stale group n/m progress (with phase-local rate/
  ETA), JSON counters `feature_groups_processed` / `features_written` /
  `feature_groups_failed`, and the app menu + Analysis status dialog show a
  feature-domain progress line and Phase row instead of a frozen
  “Writing features…” label on finished file counters.
- Stopping analysis during the features phase now cooperates with the indexer
  cancel path (with a longer GUI kill backstop while writing features) so
  completed group feature rows stay durable and the next run resumes a smaller
  stale set.
- Audio analysis now persists versioned per-file scalar results in schema v4.
  Group feature rows can be rebuilt from those compact rows without reopening
  the audio, while a missing or stale fallback decodes once and backfills the
  cache for subsequent retries.

- `muzaiten-features refresh` now reports elapsed time, per-stage timing aggregates,
  rich progress/ETA lines, phase markers, and optional per-file `--verbose`
  timing lines while keeping JSON output on stdout.
- `muzaiten-features refresh` now streams completed analysis rows to `features.sqlite`
  during long runs and exits cleanly with `"canceled": true` after SIGTERM or
  SIGINT, so canceling a scan preserves completed work for the next run.
- `muzaiten-features refresh` is substantially faster on large libraries: mel
  analysis now skips zero filterbank weights, reuses per-thread filterbanks,
  groups tracks with a duration sliding window, and computes chromaprints
  in-process after a conformance check against fpcalc-era fingerprints.
- Audio analysis now has explicit power levels. `muzaiten-features refresh --power`
  accepts `background`, `balanced`, and `turbo`, reports the effective power
  and job count in JSON, and the app persists `Library > Audio analysis >
  Analysis power` with a background default.
- Audio analysis progress in the app now shows count, rate, ETA, elapsed time,
  and final elapsed/per-track summaries; the status dialog also shows live scan
  state and the last successful analysis run.

### Changed

- `CLAP provider scan operation` now submits bounded audio batches to CLAP (eight files
  by default, configurable with `--batch-size`) instead of starving CUDA with
  one-file model calls. Each completed batch is committed as a durable resume
  point, so a later decode/model failure does not discard earlier work. Since
  non-fusion CLAP consumes one 10-second window, scans now seek directly to a
  stable uniformly distributed window instead of decoding and discarding the
  rest of every track, with up to four bounded decodes running concurrently;
  an empty beyond-EOF seek caused by inaccurate duration metadata falls back
  to the first bounded window, as does a seek that stalls for 30 seconds on a
  poorly indexed container.
- Scalar analysis is now `muzaiten-dsp-v2`: a first-party, allocation-free
  fixed-2048 real FFT replaces the complex-double STFT while keeping
  double-precision power and reductions. A four-minute analysis fixture drops
  from about 893 ms to 141 ms on the reference development machine. Existing
  v1 scalar rows refresh through the normal progress/cancel/resume feature
  phase; decoded identity, Chromaprint groups, tempo, energy, loudness, onset
  rate, and zero-crossing semantics are unchanged, with only tightly bounded
  spectral floating-point deltas. Post-integration streaming (gated loudness
  through a block ring, virtual reflected padding) brings the fixture to
  about 131 ms with bit-identical output.
- Analysis workers hold far less memory: whole-track double arrays and the
  duplicate raw PCM byte copy are gone, so each in-flight track costs one
  float PCM copy instead of roughly five duration-scaled buffers. On a
  deliberately hostile corpus (DSD, 96/192 kHz, 79-93 minute tracks) an
  8-worker feature refresh peaks at ~2.7 GiB instead of ~4.8 GiB — and
  instead of the ~20 GiB observed in the field on the v1 spectrogram path.
- Scalar features are now consumed only when their per-row DSP version matches
  the running build, so radio never treats an untouched old store or a
  cancel/resume mixed-version store as current. The analysis status dialog,
  `muzaitenctl features-status`, and `muzaiten-features` JSON now expose the stale
  portion without redefining the existing total featured count.
- Feature fill now uses the configured analysis worker count to decode and
  analyze stale group representatives concurrently while keeping
  `features.sqlite` writes serialized and resumable.
- Feature-only refreshes copy fresh representative scalars and skip redundant
  Chromaprint regrouping when all successful files already have group ids. On
  the measured 77,590-group store, a warm refresh completes in about 2.7
  seconds with zero decodes instead of repeating hours of audio work.
- Changed-file scans now regroup only new or changed tracks and the old groups
  they can split or merge, while preserving unaffected connected components
  and stable ids. An exact copied 99,159-file / five-change run spent about
  1.8 seconds grouping and 2.85 seconds end to end, replacing a full-library
  regroup observed running for more than 17 minutes. Initial scans and
  interrupted-state recovery retain the exact full regroup fallback. Loading
  Chromaprint only for affected duration neighborhoods reduced peak RSS from
  about 894 MiB to 380 MiB in a paired replay while slightly improving time.
- Changing `Analysis power` while a scan is running now applies immediately:
  the scan stops (keeping all completed work) and restarts at the new power.
- The analysis progress rate (and its ETA) now reports recent throughput
  over roughly the last minute instead of the lifetime average, which after
  hours of scanning barely moved and hid the effect of power changes.
- The audio-analysis menu action is now `Stop analysis`, and stopping says
  explicitly that completed work is saved and the next run resumes.

### Fixed

- Gapless track changes no longer leak the handoff into the progress bar:
  between playbin's `about-to-finish` and the audible switch, the outgoing
  track's duration is held so the bar runs to 100% and snaps straight to
  0:00/new-length instead of jumping through bogus intermediate ratios
  (100% → ~20% → 0%). The new track's duration is published in the same tick
  the queue advance commits.
- Seeking is now robust around track boundaries: flushing seeks are never
  stacked (a slider scrub coalesces onto the in-flight seek and re-issues the
  latest target on completion), a seek during the armed gapless handoff
  window — where playbin's behavior is undefined and could wedge the pipeline
  at 0:00 — cancels the handoff and deterministically reloads the audible
  track at the target position (re-arming the prepared next track), and a
  4-second watchdog reloads the source if a flushing seek never completes, so
  a wedged pipeline self-heals instead of requiring a manual track switch.
- Seeking while paused with a released output device (bit-perfect /
  release-on-pause soft pause) now retargets the resume position instead of
  being silently dropped by the READY pipeline, and the position shown while
  scrubbing updates immediately instead of snapping back until the seek
  completes. A backward seek in the last seconds of a track also no longer
  falsely triggers the gapless queue advance.
- Pausing in a release-on-pause profile no longer loses the resume position to
  0:00 when the position query fails transiently (e.g. mid-flush).
- Seeks are now sample-accurate (`ACCURATE` instead of `KEY_UNIT`), which also
  fixes an "Internal data stream error" that key-unit seeks near the end of
  FLACs without a seektable (e.g. ffmpeg-muxed rips) triggered in flacparse.
- A pipeline error no longer freezes the transport: the backend first retries
  the current source in place (one bounded attempt per position — decode
  errors after a seek and transient stalls recover silently), and starting any
  track from the error state rebuilds the pipeline, so a genuinely unplayable
  file reports its error once and the next play works instead of requiring a
  track switch to revive playback.
- `CLAP provider neighbor operation` now scales to real libraries: the cosine
  neighbor rebuild works in fixed-size blocks instead of materializing the
  full group-by-group similarity matrix (24 GB at 77k groups) and ranking it
  in pure Python. Ranking semantics are unchanged (highest cosine first,
  ties by ascending group id) and pinned by a conformance test.
- The Analysis status dialog `Files` row no longer clips large counts such as
  `99,159 (98,561 ok, 598 failed)` at the default size; form fields grow with
  the dialog width.
- Incremental audio analysis scans (and cancel + resume) no longer wipe the
  whole `features` table and serially re-analyze every unchanged group
  representative; existing feature rows are preserved and only stale or new
  groups are recomputed. A scan that analyzed nothing also no longer
  overwrites the last-run summary.

## [2026.07.07]

### Added

- Documentation restructure: the README is now a concise front page and the
  deep guides moved to docs/ (search, radio/recommendation engine,
  playlists, controls, muzaitenctl reference, runtime paths).
- Track context menus now share one canonical action builder across the library
  table, queue, search results, playlist items, file explorers, and Music
  Explorer inline tracks. The shared menus add `Copy path`, make Search expose
  Start Radio and taste flags, add taste flags to playlist items, and use
  consistent ordering, count suffixes, checked flag state, and ellipsis glyphs.
  Album and artist menus also gained the missing playlist/radio/play-all
  actions, and the player-bar artwork can now reveal the current track in the
  library or file manager.
- Empty-area context menus now expose view-level actions in the queue, library
  track table, search results, Music Explorer, and playlist item table, so a
  right-click on whitespace no longer dead-ends common queue, search, layout,
  and playlist commands.
- The menu bar was reorganized around what muzaiten actually is. "File" is
  now "Library" (everything in it manages the library) and hosts the MPD
  source config that previously hid under Settings. Radio gets its own
  top-level menu: start radio / artist radio from the current track, stop
  radio, the Rediscovery and Deep-cuts mixes (replacing the two-item Mixes
  menu), and — reachable at last WITHOUT an active session — Adventurous,
  Exploration, Radio batch size, and Radio shuffle percent (the radio
  indicator button's right-click menu still mirrors the session controls).
  A Help menu with About muzaiten shows the exact running version for
  reports.
- Radio and Library now expose Stage-4 curation in-app: `Radio > Genre
  curation...` edits aliases and ignored radio genres while showing the folded
  vocabulary, and `Library > Audio analysis` opens read-only status and
  duplicate-copy dialogs with pin/unpin controls.
- `Library > Audio analysis > Analyze library audio` now starts the bundled
  `muzaiten-features` from inside the app, with live menu progress, cancel, and a
  completion summary. The indexer gained a `--progress` stderr stream so JSON
  stdout stays machine-readable.
- `Radio > Scoring weights...` now edits the active scoring weights, manages
  named tuning profiles, and can save suggestion-only learned profiles from the
  same local radio telemetry used by `muzaitenctl radio-learn`.
- Clean-room scalar extraction is now live end to end: `muzaiten-features` is a
  C++ binary built with the app, writes schema-v3 `features.sqlite` rows with
  DSP tempo/loudness/energy/spectral scalars, and replaces the GPL-blocked
  bliss sidecar plan. The Rust indexer crate and top-level `sidecar/` layout
  were retired; the optional CLAP embedder now lives under `tools/features-clap`.
- Start Radio: seed a radio session from any library track
  (`muzaitenctl start-radio <path>` / `stop-radio`, plus a UI entry). Picks are
  scored by genre, era, rating and listening affinity, with a novelty bonus for
  the unheard and artist/album throttling so a session never stalls on one
  artist or album — and every pick records why it was chosen. Song no-repeat
  now follows MusicBrainz recording IDs when available, falling back to folded
  artist+title, and the album cap follows MusicBrainz release groups so
  remasters/deluxe editions count as the same album. Listening affinity is
  pooled across every library path with the same song key, so a FLAC album copy
  and a compilation/portable copy strengthen the same recommendation signal
  instead of diluting each other. The shuffle button now includes Radio shuffle,
  an ambient taste-aware shuffle mode that uses the radio engine for library
  pulls without starting a visible Start Radio session; its pull chance defaults
  to 80% and is configurable from the Radio menu. Hovering a radio
  pick in the queue now explains the choice with a human-readable summary and
  the numeric scorer breakdown. Powered by the play history, imported scrobbles,
  and genre data added in the previous
  slices. A "Start Radio" context-menu action, seeded from the clicked track,
  is now available in the library track table, the queue (sidebar and
  full-screen), the playlist view (per-item and per-playlist/saved-queue,
  seeded from its first resolvable track), the library/free-roam file
  explorers (only for files that resolve to a scanned library track), and the
  music explorer's expanded-album track list. Starting radio snapshots the
  current queue to the backlog first, the same way Clear queue does, so
  "Restore previous queue" can undo it; those radio-origin entries are tagged
  and labeled "Radio session <timestamp>", and kept in their own 15-entry
  bucket so repeated radio starts no longer evict regular automatic restore
  points. Regular automatic queue snapshots also keep 15 entries by default;
  either bucket can be switched to unlimited from Saved queue limits. The player
  bar shows a radio indicator button next to shuffle/repeat while a session is
  active; clicking it stops the radio and keeps the queue. Genre matching now
  canonicalizes engine-side aliases before matching and weights shared genres
  by rarity (IDF) instead of a plain overlap ratio, so multilingual variants
  like "clássica"/"classique" can count as "classical" while the raw scanned
  genre tags remain untouched; the starter alias set is intentionally small and
  will grow through user curation. A broad or junk genre like "Other" can no
  longer dominate a session by itself; tagger placeholder genres ("Other",
  "Unknown", "Various", ...) are ignored entirely — they
  never seed a candidate pool or count as a genre match — and every session's
  candidate pool always blends in a random library slice so radio can escape
  a single genre cohort even when the seed's only genre is uninformative.
  Crowded tag-soup genre lists now dampen the genre score instead of saturating
  similarity by offering many broad overlaps, and the scorer can load runtime
  JSON weights from `radio.scoringWeights` for no-rebuild tuning.
  Skips are only held against a track when they happen before the scrobble
  threshold (half the duration, capped at 4 minutes) — skipping near the end
  is a listen, not a dislike — and a lone skip on a barely-played track is a
  light penalty rather than a maximal one. Radio now fills the queue in
  batches (default 15, configurable) instead of generating one pick at a time,
  so upcoming picks are visible in the queue and can be pruned like any other
  row; a run of three early skips in a row regenerates the not-yet-played
  picks from the drifted mood. The player bar's radio indicator button gained
  a right-click menu exposing exploration (a persistent 0–100 setting plus a
  per-session "Adventurous" boost to 85) and the batch size. Play events
  originating from radio picks are now attributed source "radio" instead of
  riding along as "library_shuffle"/"queue_auto". Manually adding tracks while
  radio is active is now a defined part of the session contract: queued rows
  play normally, then join the rolling radio mood once they are heard. Removing
  queue rows now records local, record-only telemetry for future inspection,
  including whether the row was an unheard radio pick, without feeding that
  signal back into scoring. Active radio and mix
  sessions now survive app restarts: the visible queue resumes from the saved
  queue state, while the radio brain restores its sequencing constraints and
  rolling mood but rebuilds candidates and listening affinity from fresh data.
- Rediscovery and Deep cuts mixes: the Mixes menu and
  `muzaitenctl start-mix <rediscovery|deepcuts>` can now start seedless,
  radio-backed sessions over long-unheard favorites or rarely played tracks by
  artists with strong listening history. Mixes reuse the normal radio queue,
  reasons, indicator, stop control, exploration, and batch-size behavior.
- Start Artist Radio: right-click an artist in the library sidebar or run
  `muzaitenctl start-artist-radio <artist>` to seed radio from that artist's
  aggregate genres and era. The session opens with a representative track by
  the artist, then uses the normal radio batching, explanations, reroll,
  restore, and stop controls.
- Taste controls: tracks can be marked "Never play on radio" or "Don't learn
  from this", applying to every library copy of the same song; Listening
  History can also forget a song's local recommendation behavior without
  deleting the user's scrobble history, with imported listens removed only when
  explicitly requested.
- Scrobbler history backfill: muzaiten can now import your historical listening
  data — full timestamped listens from ListenBrainz and per-track play counts
  from Last.fm (`user.getTopTracks`) — into `history.sqlite`, matched to library
  tracks by recording MBID or folded artist+title. Imported rows live in their
  own tables and never re-scrobble; re-running is cheap and incremental. The
  data feeds the upcoming recommendation engine. An interrupted ListenBrainz
  import (persisted resume cursor) now resumes automatically ~20s after
  launch, unless it was explicitly canceled — cancel is the only thing that
  stops eager auto-resume. The Scrobblers menu shows live import/sync
  progress (processed/stored counts, the point reached, and a total when
  known) with start and cancel actions, and `muzaitenctl scrobble-backfill`
  gained `status` and `cancel` alongside `<listenbrainz|lastfm>`. Transient
  ListenBrainz failures (its deep-history pages are flaky) self-heal: rate
  limiting waits out the advertised window, a run that gives up retries on
  its own ten minutes later, and the status shows the cumulative stored
  total so a resumed run's counters don't read as data loss.
- Queryable genres: the GENRE tags already captured in each track's metadata
  are now mirrored into a `track_genres` table (split on common separators,
  deduplicated case-insensitively), populated on scan and backfilled once from
  the existing library without a rescan. Groundwork for genre-based
  recommendations; the accompanying schema bump triggers a one-time search
  index rebuild on first launch.
- Queryable genre splitting now also treats `|` as a separator, so tags like
  `Alternative | Other` no longer survive as one folded genre. A one-time
  library migration rebuilds `track_genres` from the already-stored metadata
  blobs without rescanning files.
- `muzaitenctl genre-report` now dumps the folded genre vocabulary offline with
  document frequency, radio-style IDF, alias/stoplist status, sample artists,
  and curation flags for near-duplicates, separators, classifier-looking tags,
  and non-ASCII genre names.
- `muzaitenctl radio-reasons` now prints the active radio session's stored pick
  explanations, including scorer components, so tuning can inspect the live
  queue without hovering UI tooltips.
- Radio can now ignore curated real genres without hiding them elsewhere:
  schema v14 adds `radio_ignored_genres`, radio session genre joins/scoring skip
  ignored canonical genres, `genre-report` marks them, and `muzaitenctl`
  provides `radio-genre`/`genre-alias` curation verbs.
- `muzaitenctl radio-weights` now exposes validated get/set/save/apply/list/remove
  commands for radio scoring weights, with named profiles stored in the library
  database and active changes taking effect on the next radio session.
- `muzaitenctl radio-learn` now suggests a local learned radio-weight profile
  from accumulated radio-pick telemetry and play outcomes; dry-runs print the
  multiplier table and JSON diff, while saves create a named profile for later
  explicit `radio-weights apply`.
- A standalone `sidecar/indexer` Rust crate now builds `features.sqlite`
  groundwork from generated or library audio paths: canonical decode hashes,
  fpcalc Chromaprint fingerprints, content groups, incremental scan state, and
  status JSON. Bliss scalar extraction is deliberately not linked because the
  available Rust bliss crates are GPL-only.
- The app now opens `features.sqlite` read-only when present and exposes a
  `FeatureStore` seam for later radio consumers, while
  `muzaitenctl features-status` reports local coverage without a running app.
- Radio now uses `features.sqlite` content groups when present: duplicate copies
  share one recommendation identity, candidate explanations follow the resolved
  queued copy, and playback prefers the highest-quality copy unless the user
  pins a different path. `muzaitenctl duplicate-groups`, `pin-copy`, and
  `unpin-copy` expose the local duplicate sets and per-group override.
- When `features.sqlite` includes scalar feature rows, radio scoring now adds
  explainable tempo and energy proximity components to the existing genre, era,
  rating, and history signals. Libraries without scalar rows keep the previous
  scores and explanations exactly.
- A standalone `sidecar/embedder` Python CLI can now add CLAP content-group
  embeddings and precomputed cosine neighbors to `features.sqlite` schema v2.
  The app accepts both schema v1 and v2 databases read-only; model weights stay
  outside the repository cache and are verified before use.
- When CLAP embeddings are present, radio now scores an explainable "sounds
  similar" audio component against the session's rolling sonic centroid and
  augments seeded/anchorless candidate pools with precomputed sonic neighbors,
  helping tag-poor tracks participate without changing behavior for libraries
  that have no embeddings.
- `muzaitenctl semantic-search "<text>"` now performs CLI-only free-text
  semantic library search by embedding the query through the optional
  semantic provider, cosine-ranking stored CLAP content-group
  embeddings, and returning each group as its preferred library copy with a
  score.
- Local play-event telemetry: every playback now records how it ended
  (completion, skip, stop, or session end), how much was actually heard, where
  the track came from, and which listening session it belonged to, stored
  locally in `history.sqlite`. No network use; groundwork for the upcoming
  recommendation engine.
- Local rating-change telemetry: every explicit track-rating edit now records
  an append-only event in `history.sqlite` with old/new rating values and the
  playback/UI context, without changing recommendation scoring behavior.
- Local radio-pick telemetry: generated radio picks now persist their scorer
  component breakdown, active weights, session kind, exploration value, and
  total score in `history.sqlite`, without feeding that data back into current
  recommendation scoring.
- `muzaitenctl scrobble-backfill reset <listenbrainz|lastfm>` clears a service's
  completed/synced marker so the next import re-walks full history. This recovers
  listens added *behind* an already-imported range, which the completed-import
  early-stop would otherwise never revisit; imported-listen dedup keeps the
  re-walk safe, and `reset` is refused while a backfill is running. README's
  `muzaitenctl` reference now documents the full backfill verb set.

## [2026.07.01.2]

### Packaging

- Release tarballs now contain the installed prefix tree directly instead of a
  staging-directory `./` entry, avoiding accidental target-directory permission
  changes when extracting the prebuilt archive.

## [2026.07.01.1]

### Packaging

- Release tarballs now normalize archived file ownership to root, so prebuilt
  AUR packages do not inherit the maintainer's local user/group ownership.

## [2026.07.01]

### Added

- The "/" incremental search now works the same way everywhere. The queue, both
  file explorers, and the playlist tracklist gain the main view's ncmpcpp-style
  behavior: "/" opens the search, Return confirms the constraint (so M-n/M-p keep
  cycling matches with the list focused), and Escape clears it. The queue's
  search previously had no confirm step, so cycling only worked while typing.
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

- Secondary screens are now built only on first use. Startup opens with just the
  library home in memory; the file explorers, search page, queue screen, and
  playlist view are constructed the first time you navigate to them, with saved
  view settings restored at that point.
- Background screens now release their memory when left idle. After about a
  minute on another screen, the file explorers, playlist view, search index, and
  the library's decoded cover thumbnails free their retained data and return to
  baseline memory, rebuilding it on return. Quick back-and-forth navigation
  within the idle window keeps everything resident, so it stays instant.
- Memory reclaim timings are configurable from Settings > Memory reclaim. Set
  the hidden-screen or deeper artwork-cache release interval to `0` to disable
  that tier; the deeper tier also asks the artwork cache to release retained
  decoded-image memory after a longer stay away from the library home.
- Scrobbler backlog-clearing actions in the menu now appear only while that
  service has pending scrobbles, with the pending count shown in the action.
- Listening history now labels manual resubmission actions as "Scrobble to
  Last.fm" and "Scrobble to ListenBrainz"; row state remains visible in the
  service columns.

### Fixed

- Releasing an exclusively-held card from bit-perfect output no longer leaves a
  "ghost" bit-perfect state behind. The Release device action flipped only the
  profile's mode to shared while keeping bit-perfect's pinned ALSA sink and hw
  device, so the persisted "shared" profile still opened the card directly —
  playback errored on a busy device without offering takeover, or silently ran
  bit-perfect on a free one, until the output dialog was reopened and re-saved.
  Releasing now materializes the full shared profile from the remembered
  shared-mode selections.
- Music Explorer's expanded album panel now scrolls with inline track keyboard
  navigation, uses the same view-wide `j`/`k`/`h`/`l` routing as the main
  library panels, and paints the expanded tracklist rows as a seamless custom
  table. Pressing `h` from the expanded tracklist now collapses back to the
  album grid as one step before a second `h` moves to the previous panel, and
  switching directly between expanded albums no longer flashes the intermediate
  collapsed layout. Albums without artwork now keep a neutral panel background
  instead of falling back to the app highlight color.
- The selected album in Music Explorer's grid now uses the same full-strength
  highlight as the library album grid when the panel is active, instead of a
  permanently dimmed tint, and dims only when another panel takes focus.
- Music Explorer's expanded tracklist selection now fills with the full app
  highlight to match its high-contrast text (it previously read as dimmed under
  the bright foreground), and its header reuses the even-row zebra shade so it
  tracks the album art tint instead of standing out as a flat opaque bar.
- Music Explorer no longer flashes the album grid or drops keyboard focus when
  the window is (de)activated by the window manager — selection colors are read
  from the active palette group so idle activation repaints to identical pixels,
  and keyboard focus is restored to the last-focused panel on reactivation so
  `hjkl` keep working without a click.
- Expanding, collapsing, and switching the expanded album in Music Explorer now
  reuse the existing grid cards and inline tracklist instead of rebuilding the
  whole middle panel. Previously each of these recreated every card, which
  blanked all album art until the async artwork cache reloaded it (a visible
  flash); the grid now only rebuilds when the album set or column count actually
  changes.
- Music Explorer no longer flashes its album art on redundant refreshes
  (re-selecting the same artist, returning to the view, unrelated rating
  changes): identical album sets are now a no-op, and genuine changes keep the
  cached art for albums that survive instead of clearing every cover.
- Music Explorer album cards and the expanded panel now paint opaquely, so a
  repaint during an expand/collapse/switch reflow no longer briefly blanks the
  album art or text before redrawing it.
- Double-clicking an album in Music Explorer no longer replaces the queue,
  matching the library album grid: play actions come from the album's right-click
  menu or from selecting tracks in the expanded tracklist. Single click still
  selects and expands the album.

- Library-wide shuffle no longer grows a playlist while playing it. When the
  queue is mirroring a playlist, the fresh library tracks shuffle injects join
  the live queue only and are no longer saved into the playlist — those tracks
  were never part of it. Your own adds to a playlist-backed queue still mirror
  (and still prompt) as before.

- Closing the window to the tray (or quitting) while a playlist drop-import was
  running no longer crashes: the worker is kept alive on hide and cleanly stopped
  and joined on quit, leaving the tracks matched so far persisted.

- Shuffle now remembers playback order in both directions: after stepping back
  with Previous, Next (or auto-advance) replays the exact tracks you just
  retraced instead of re-rolling, and only resumes fresh randomness once the
  remembered trail is spent. Picking a track clears the forward retrace trail;
  reordering or editing the queue still resets shuffle memory.
- Manually picking a track while shuffle is on now refreshes the no-repeat
  shuffle bucket from that track, while keeping Previous able to return to the
  track you left.
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
