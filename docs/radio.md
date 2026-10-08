# Radio — the offline recommendation engine

Everything here runs locally against your own library and listening
history. There is no cloud, no account, and nothing leaves your machine.

## Starting a session

- **Start Radio** from any track's context menu (library, queue, playlists,
  explorers, search results), from `Radio > Start radio from current track`,
  or `muzaitenctl start-radio <path>`. The seed's genres, era, and sound set
  the session's initial direction.
- **Artist radio** from the artist sidebar's context menu or
  `Radio > Start artist radio`.
- **Radio shuffle** is the ambient variant: the shuffle button cycles to a
  taste-aware mode that uses the radio engine for library pulls without a
  visible session. It starts its rolling mood from the current library track,
  including the same genre, tempo, energy, sonic-neighbor, and CLAP audio
  context as Start Radio, then follows the rolling context as tracks play. Its
  pull chance is configurable at
  `Radio > Radio shuffle percent…` (default 80%).
- **Mixes**: `Radio > Play Rediscovery mix` (loved-but-forgotten) and
  `Play Deep cuts mix` (rarely surfaced album tracks).

Starting radio snapshots the current queue first, so "Restore saved queue…"
can undo it. While a session runs, a radio indicator appears in the player
bar. Click to stop (the queue is kept), or right-click for session options.
Closing the window to the tray does not stop the session; reopening it restores
the indicator and its session controls.
Starting from the track that is already playing keeps its playback position
and replaces only the queue around it. Starting from another track plays that
seed immediately. In both cases recommendations load in the background and an
indeterminate status-bar indicator remains visible until the new tail arrives.

## Anchor for new sessions

`Radio > Anchor for new sessions` sets how far a new session may travel from
where it started. `Stay near the starting song` (the default) keeps every pick
close to the track you seeded from. `Drift with what plays` lets the direction
move as the session runs: queued picks nudge it, and a song that actually starts
playing steers it strongly. Rows you remove, refresh, or skip past stop steering
it. The choice applies to new seeded and artist sessions; a running session,
Mixes, and Radio shuffle are unaffected.

## Multiple starting points

Selecting several tracks and choosing `Start Radio (N)` plays the first one and
treats the rest as equal starting points, taking turns between them rather than
averaging them together. Rows that are not in your library are ignored.

## How picks are scored

Every pick blends **era** proximity, **ratings**, **listening history**
(local plays + imported scrobbles, pooled across duplicate copies), a
**novelty** bonus for the unheard, and penalties for recent plays, high skip
rates, and repeating artists. The first five radio picks use the full novelty
and rating weights; subsequent picks linearly taper those two signals by 10%
per pick to small floors, so the session naturally settles into its rolling
sound without losing discovery or taste history entirely. When the rolling context and a candidate share a
complete tempo-and-energy pair or matching CLAP embeddings, **tempo** and
**energy** proximity plus **audio** similarity by CLAP embedding are the
primary match signal. In that case genre tags are intentionally ignored. If
neither audio signal is available, radio falls back to shared **genres**
(alias-canonicalized and rarity-weighted, so `shoegaze` counts more than
`rock`) rather than combining incomplete audio with potentially incorrect
tags.

Hover a radio pick in the queue to see exactly why it was chosen — a
human-readable summary plus the numeric component breakdown.

Sessions throttle artists and albums, never repeat a song (MusicBrainz
recording identity, falling back to folded artist+title), and resolve
duplicate copies to the best available quality (see duplicate pinning
below).

## Steering a session

- **Exploration** (`Radio > Exploration…`, 0–100) controls how far picks
  may stray from the seed's mood; **Adventurous (this session)** is a
  one-session boost.
- **Never play on radio** and **Don't learn from this** are per-track
  taste flags on every track context menu. The first excludes a track from
  radio permanently; the second keeps a track's listening history out of
  the engine's taste signals (guilty-pleasure mode).
- **Batch size** (`Radio > Radio batch size…`) sets how many picks queue
  ahead at a time.
- **Refill padding** (`Radio > Radio refill padding…`) sets how many queued
  tracks may remain before the next batch starts loading. The default is 5;
  use 0 to wait until playback reaches the final queued track.
- **Refresh picks below** appears when you right-click a queue row during a
  radio session. It replaces only the unplayed radio picks below that row;
  earlier rows and manually queued tracks are kept.
- During an explicit radio session, ordinary **Add to queue** actions become
  play-next operations while preserving selection and album order. When radio
  stops, they return to appending at the tail.

## Tuning the scoring

`Radio > Customization…` edits every scorer weight and the session-decay
controls with validated ranges. Profiles are named and persisted in the
application configuration directory, with the active profile restored at
startup. New, Duplicate, Rename, Delete, and Reset to Default manage the
profile list; the Default profile cannot be deleted. Each profile retains its
own most recent 50 changes for Undo and Redo.

Edits preview immediately in a running radio or Radio Shuffle session: newly
generated picks use the changed settings, while tracks already queued keep
their existing order. A 500 ms pause saves one history step. Apply sets the
Revert baseline; Revert restores that baseline and applies it live.

The same profile operations exist client-side:

```sh
muzaitenctl radio-weights get | set '<json>' | save <name> | apply <name> | list | remove <name>
muzaitenctl radio-learn [--dry-run] [--min-samples N]
```

These commands edit the same `radio-profiles.json` store as the dialog: `set`
changes the active profile's weights, `save` copies it to a named profile, and
`apply` chooses the profile used on the next application launch. `remove`
cannot delete Default. `radio-learn` creates or updates a suggestion-only
`learned-YYYYMMDD` profile; learned profiles are CLI-first and are not shown as
suggestions in the dialog. GUI profile changes take effect for the next
generated pick.

`radio-learn` labels each recorded pick by what happened to it within the
following twelve hours. A pick that started playing is labeled by its play:
an early skip is a rejection, anything else is not. A pick removed from the
queue before it played is a rejection too, at half the weight of a skip, since
you turned the track down without hearing it. The command reports how many
picks were labeled from plays and how many from unheard removals.

### Local profile exchange

Export… and Import… in `Radio > Customization…` exchange local JSON files.
Export discloses the contents before opening a file picker. Import shows the
name, origin, all scoring weights and session-decay values before saving. A
matching name requires checking **Replace existing profile**. Import keeps the
active selection; replacing the active profile applies its tuning to newly
generated picks. Replacement starts a fresh undo history for that profile.
CLI import requires the app to be closed; it refuses a running instance so a
later in-memory GUI save cannot overwrite the imported profile. A successful
CLI import affects the next application launch. Dry-run and export remain
available while the app runs.

```sh
muzaitenctl radio-weights export evening.json --profile Evening
muzaitenctl radio-weights import evening.json --dry-run
muzaitenctl radio-weights import evening.json
muzaitenctl radio-weights import evening.json --replace
```

Without `--profile`, export uses the active profile. Both import and export
refuse an existing destination unless replacement is explicit; export's
`--replace` replaces the file, import's replaces the named local profile.
Export publishes the file atomically, so a file created during the export is
refused unless `--replace` is given.
`--dry-run` validates and previews an import, including whether replacement
is required, without saving anything. Importing a different name adds a
profile without selecting it. Importing the name Default also requires
explicit replacement when Default exists.

The `muzaiten-radio-profile` version 1 format contains only `format`,
`version`, `name`, `origin` (`manual` or `learned`), `weights` and
`sessionDecay`. It preserves every weight and all four decay controls.
Canonical exports use UTF-8 JSON with sorted keys, four-space indentation and
a final newline. Export, import, then export preserves the bytes. Unknown
formats, versions or keys, duplicate or missing keys, malformed JSON,
non-finite or out-of-range numbers, and files over 64 KiB are rejected before
any profile changes. Profile names must be 1–200 UTF-16 code units without
surrounding whitespace, control characters, invisible format characters or
unpaired surrogates. Writes
replace destinations atomically. See [data-safety.md](data-safety.md) for the
complete payload and privacy boundary.

Profiles created by `radio-learn` carry a durable learned origin. Renaming,
duplicating or tuning one preserves that origin. Reset to Default replaces
the tuning with manual factory values; Undo can restore its previous origin.
Legacy `learned-*` names are treated as learned; other profiles without origin
metadata, including migrated legacy weights, have unknown origin. Loading
does not guess that a profile is manual; the origin is persisted on the next
save. Learned and unknown-origin exports require `--allow-learned` or an
explicit dialog confirmation because tuning derived from listening can reveal
taste. An unknown-origin export is labeled learned so recipients retain this
warning.

```sh
muzaitenctl radio-weights export learned.json --profile learned-20260101 --allow-learned
```

The file contains no listening records or recording identifiers. Its custom
name is included verbatim, so choose one you are comfortable sharing. Exchange
uses local files only and makes no network requests.

## Aggregate taste seeds

`Radio > Taste profiles…` exports aggregate library taste and imports local
files as named seeds. **Preview library export…** generates an exact JSON
preview only after that action; **Export these contents…** deliberately opts
in to writing those bytes. Decade shares are optional and unchecked by
default. **Preview import…** validates and shows the aggregate contents;
choose a local seed name and press **Import seed** to save. Replacement needs
an explicit checkbox. Import does not start radio.

Select an imported seed and press **Start radio**, or use:

```sh
muzaitenctl radio-taste export taste.json --opt-in --dry-run
muzaitenctl radio-taste export taste.json --opt-in
muzaitenctl radio-taste import taste.json --name "Evening library" --dry-run
muzaitenctl radio-taste import taste.json --name "Evening library"
muzaitenctl start-taste-radio "Evening library"
muzaitenctl stop-radio
muzaitenctl radio-taste remove "Evening library"
```

Every CLI export, including its preview, requires `--opt-in`; consent is never
saved. The preview and export print only the exact file bytes to stdout, with
disclosure/status on stderr. Exports read the local library and current DSP
scalars without changing either store. They summarize scanned, present and
playable tracks inside source directories with **Library** enabled (every
library track when no source directory is configured). Tracks marked **Don't
learn from this** are left out; tracks marked **Never play on radio** stay in,
since the file describes the library's composition. Genre aliases are
canonicalized and ignored/placeholder genres omitted. Duplicate song
identities and duplicate content groups count once. Genres and decades with
fewer than five distinct tracks are omitted; each feature distribution needs
five distinct samples. Shares normalize over retained buckets; multi-genre
tracks support each of their distinct genres. Tempo and energy report
interpolated q25/q50/q75 quartiles, never individual rows or sample counts.

Every exported number is rounded. Shares are whole hundredths, assigned by
largest remainder so each set sums to exactly 1.00; a bucket that rounds to
zero drops out. Tempo quartiles are whole BPM and energy quartiles are
hundredths. Quantization limits precision and inference, but a rounded value
can still coincide with a track's value, and distinctive aggregates can reveal
recordings or narrow possible counts. It does not guarantee anonymity.

The selected seed contributes a fixed term from **0 to 3 score points** through
`RadioSession` and `TrackScorer`. Genre contributes twice the largest matching
canonical share, from 0 to 2. Tempo and energy each use
`max(0, 1 - abs(value - q50) / max(q75 - q25, floor))`, with floors of 20 BPM
and 0.1 energy. Their mean contributes from 0 to 1; missing candidate features
contribute zero. This term augments ordinary context scoring even when richer
audio features are available. Decade shares are informational and do not
change scoring. Pick reasons name the local seed, including picks with no
matching aggregates. The pool uses the same playable/enabled-root filters,
Never play on radio exclusions, artist/album throttles and duplicate prevention
as other radio modes, with the existing 5,000-candidate fallback cap.

**Stop radio** deactivates the seed. **Remove seed** in the dialog also stops
radio when that seed is active. Offline CLI removal prevents future selection;
a running or saved session owns its aggregate snapshot until stopped, so
removal/replacement does not invalidate it. Track, artist and seedless sessions
remain compatible. Names stay in `radio-taste-seeds.json` in the configuration
directory, in local session state and in radio-pick history, where they remain
after seed removal. They are absent from exchange files. Picks from seeded
sessions can inform ordinary local learning through their other score components.

The strict `muzaiten-taste-profile` v1 file is limited to 64 KiB. Unknown
keys, versions, duplicate keys, invalid shares,
unordered/out-of-range/nonfinite quartiles, invalid UTF-8 and incomplete
inputs are rejected before import changes. Existing export files and same-name
seeds require `--replace`. Writes are atomic; on filesystems without hard
links, such as FAT and exFAT drives, a new export file is instead created
exclusively and removed if the write fails. The app supplies no transport,
accounts, URLs or automatic export. Aggregates still reveal library taste;
suppression and quantization do not guarantee anonymity. A seed biases picks
toward the source library's genre and feature mix; it does not model the source
listener's preferences. See
[data-safety.md](data-safety.md#aggregate-taste-profile-files).

## Genre curation

Scanned genre tags are matched through a folded, alias-canonicalized
vocabulary. `Radio > Genre curation…` edits aliases (`clássica` →
`classical`), radio-ignored genres (`soundtrack`), and shows the full
vocabulary with track counts for context. CLI equivalents: `genre-alias`,
`radio-genre`, `genre-report`.

## Audio analysis (content-aware tier)

`Library > Audio analysis > Analyze library audio` runs the bundled
`muzaiten-features` over your library with live progress: one canonical decode
per file computes exact audio identity, Chromaprint content groups
(duplicate detection across formats/codecs), and clean-room DSP scalars
(tempo, loudness, energy, brightness) into `features.sqlite`. Progress
moves through file analysis, grouping, then a **Writing features** phase
that refreshes only missing or stale group feature rows (with its own n/m,
rate, and ETA). Stopping mid-run keeps completed work; the next analysis
resumes remaining files and stale groups. Terminal equivalent:

```sh
muzaiten-features refresh --progress=jsonl
```

`Library > Audio analysis > Analysis status…` (or
`muzaitenctl features-status`) reports coverage. `Duplicate copies…` (or
`duplicate-groups` / `pin-copy` / `unpin-copy`) inspects detected duplicate
groups and pins which copy radio should prefer.

The current `muzaiten-dsp-v2` analyzer makes full-track analysis substantially
cheaper with a first-party fixed-size real FFT. The v2 comparison kept the
tempo and energy values used by radio exactly stable across the synthetic
oracle and an isolated DSF/high-resolution corpus. Radio treats half- and
double-tempo estimates as equivalent, so an 85 BPM context matches a 170 BPM
candidate. Upgrading marks v1 scalar rows stale until the normal resumable
feature phase refreshes them; stale rows never enter scoring.

### Local scalar feature packs

`muzaiten-features pack` exchanges scalar analysis through a local
`muzaiten-features-pack` v1 NDJSON file. Export writes each group
representative's own current analysis under that file's decoded-audio hash; it
never analyzes audio or generates embeddings. There are no accounts, uploads or
network transport.

```sh
muzaiten-features pack export scalars.ndjson --dry-run
muzaiten-features pack export scalars.ndjson
muzaiten-features refresh --stage identity --no-semantic
muzaiten-features pack import scalars.ndjson --dry-run
muzaiten-features pack import scalars.ndjson
```

Run identity refresh on the recipient before previewing a pack. Adoption
requires the exact canonical decoded-audio SHA-256 of the recipient group's
representative, plus the running binary's extractor and DSP version. Matching
MusicBrainz IDs or similar Chromaprint fingerprints never qualify a record.
Lossy re-encoding normally changes that hash, even with identical tags.
Different metadata and genres do not affect the adoption key. The hash covers
ffmpeg's decoded and resampled output rather than the file bytes, so it also
depends on the decoder: installations whose ffmpeg builds decode or resample a
file differently compute different hashes for it and match nothing. Such a
miss is safe; the recipient analyzes those files itself.

Import preserves current group rows unless `--replace` explicitly permits
replacement. Stale rows from another DSP version never enter scoring, so
import replaces them without it. Export's `--replace` permits
overwriting an existing pack file. `--dry-run` previews either operation
without writing the pack or feature rows; `--json` reports counts for scripts.
Adopted current rows are skipped by the next scalar-analysis phase. Other
groups still need local analysis, and version upgrades make adopted rows stale
under the same freshness rules as locally analyzed rows.

A pack saves analysis work only where the recipient's group rows are stale,
typically after an upgrade that changes the DSP version. Identity refresh
skips unchanged files, so their per-file rows stay stale too, and the next
feature phase would decode each such group's representative again; an adopted
current row lets that phase skip the group. On a fresh installation, identity
refresh already decodes and analyzes every file, and the feature phase fills
group rows from that analysis without decoding, so a pack adds nothing there.
The exporting installation needs a completed refresh at the same DSP version.

The pack contains scalar features and extractor versions. Taste, ratings and
listening history are absent, but anyone with the same audio can use its
content hashes to identify recordings in the exporting library. Plain export
prints this disclosure before writing, and JSON reports include it in
`contents`. Paths, filenames, timestamps, file sizes,
internal group IDs and embeddings are excluded. Fingerprints and MusicBrainz
IDs are also excluded from v1; those identifiers must not be treated as private.
See [features-schema.md](features-schema.md#local-feature-pack-format) for the
strict format, limits and report fields.

**CLAP embeddings** (audio similarity + free-text search) are opt-in and
provided by the separately installed `muzaiten-features-clap` package. Native
analysis never needs Python. Install the provider yourself—the GUI never runs
uv—and explicitly consent to the model download:

```sh
uv tool install 'muzaiten-features-clap[model]'
muzaiten-features model download
muzaiten-features refresh --semantic
```

The saved policy is disabled by default. The orchestrator discovers and
handshakes the provider, holds the same feature-store lock across native and
semantic phases, uses durable inference batches, and rebuilds neighbors only
after embeddings change. Scans and queries never download the model implicitly.
See [semantic-analysis.md](semantic-analysis.md) for provider setup, device,
consent, progress, and provenance details.

With embeddings present, radio pools get augmented with sonic neighbors
(tag-poor tracks surface because they *sound* right) and free-text semantic
search works:

```sh
muzaitenctl semantic-search "melancholic shoegaze" --limit 10
```

The full `features.sqlite` layout — identity, scalar, and embedding tables,
with units and caveats — is documented in
[features-schema.md](features-schema.md).

## Telemetry and privacy

The engine records its own picks, your play/skip outcomes, and queue removals
into the local history database purely as training data for the weight
suggestions above. Previous and direct queue-row selections are recorded as
navigation, not as rejections; Next remains a skip. Live scoring never reads
that telemetry; nothing is transmitted anywhere.
