# Data Safety

Shutdown stops and joins in-flight workers before destroying their owners or
stores. A slow filesystem operation can delay exit until the operation returns.
Scan batches commit as units; a failed batch rolls back. MPD catalog replacement
keeps the previous catalog if the replacement fails or is interrupted.

muzaiten must assume the user's music library is irreplaceable.

## Library contract

muzaiten never moves, renames, or deletes files in the music library, and it
never writes artwork into album folders. Scans do not traverse symlinks, and no
automatic metadata lookup ever changes files on disk.

The single, deliberate exception is **rating tag writes**:

- Setting a local track rating queues its background tag write. There is no
  separate tag-sync enable switch. `Library > Rating tags` also offers explicit
  sync and retry actions. Clearing an application rating removes its override
  and pending write; it does not erase an existing rating tag from the file.
- They are narrow: only the rating field is touched. No other tag, no file
  structure, no artwork.
- They are verified: every write is confirmed by re-reading the file, and a
  failed or unwritable write is kept pending for retry rather than silently
  dropped.

Rating edits always land in the application database first, so the database — not
your files — is the source of truth. Tag writes only ever mirror that state back
out following a rating edit or explicit sync action. A write that finishes after
a newer edit cannot remove that edit's pending write or overwrite its UI value.

Missing files are marked missing rather than deleted from the database until you
explicitly choose `Library > Remove missing tracks`. An incomplete directory
enumeration skips missing-file detection so an unreadable subtree is not treated
as deleted.

## Listening history and scrobbling

Local listening history is permanent and independent of every scrobbling
service. Nothing a scrobbler does removes a listen from it.

Network outages, server-side failures and rate limits delay delivery without
disabling collection. Permanent credential or configuration rejections can disable
a destination. Compatible-server rate-limit delays are capped at one day.

Delivery is tracked separately, one record per listen and destination. A record
exists only for destinations that were enabled when the listen happened, so
adding or enabling a destination never enqueues history it did not witness; you
can still queue specific rows yourself from Listening History.

The destructive-looking actions are deliberately narrow:

- **Clear backlog** drops the undelivered records of every destination
  currently picked. The listens stay, and deliveries already completed stay
  recorded as completed.
- **Removing a destination** deletes that destination's delivery records and its
  stored token, and nothing else. Other destinations' backlogs are untouched,
  and the listening history is kept in full. It takes effect on confirmation
  and cannot be undone, so the prompt states how many pending deliveries are
  being discarded before you confirm.
- **Editing a destination's URL** keeps its identity and its backlog: pending
  listens follow it to the new address rather than being dropped.

When the destinations document is a valid object with a `destinations` array,
only loaded destination IDs own custom token rows. Rows for absent IDs are
removed; a missing or malformed document leaves those rows untouched so their
owners remain recoverable by hand.

Identifiers minted for custom destinations are never reused, so a removed
destination's records can never be inherited by a later one.

## Application state

### Radio tuning-profile files

`Radio > Customization…` Export…/Import… and
`muzaitenctl radio-weights export|import` exchange local files only. A
`muzaiten-radio-profile` version 1 JSON file contains exactly:

- `format`: `muzaiten-radio-profile`; `version`: 1.
- `name`: the user-chosen profile name, included verbatim.
- `origin`: `manual` or `learned`.
- `weights`: `genreWeight`, `genreIdfSaturation`, `genreCrowdingSoftLimit`,
  `eraWeight`, `eraSpanYears`, `tempoWeight`, `energyWeight`, `audioWeight`,
  `ratingWeight`, `userRatingBoost`, `historyWeight`, `historySaturation`,
  `noveltyWeight`, `noveltyZeroAt`, `recencyPenalty`, `recencyHalfLifeDays`,
  `skipPenalty`, `sameArtistPenalty`.
- `sessionDecay`: integer `decayStartTrack` (1–1000), `decayCurve` (0–1;
  below 0.5 selects linear, otherwise exponential), `noveltyDecayFloor`
  (0–1) and `ratingDecayFloor` (0–1).

Every weight is required and uses the scorer's validated numeric range. The
file excludes paths, track/artist/album identifiers, library contents,
listening history, timestamps, undo snapshots and telemetry rows. Review the
custom name before sharing, since it can contain information you typed.
Imported names reject invisible format characters and unpaired surrogates.
Learned tuning itself can reveal listening taste even without the underlying
records. Each export of a learned or unknown-origin profile requires explicit
consent: `--allow-learned` on the CLI, or confirmation in the dialog. An
unknown-origin profile is exported as learned. Origin follows copies and
renames; it is not inferred to be manual from a name.

Import previews the complete validated payload in the dialog; CLI
`--dry-run` reports it without writing stores. Replacement of a matching
profile requires a separate checkbox or `--replace`. CLI import refuses while
a running app owns the state lock, since its later in-memory save could replace
the imported profile. Unknown keys and
versions, duplicate or missing keys, malformed or truncated JSON, invalid
UTF-8, non-finite or out-of-range values, and inputs over 64 KiB are rejected
without changing the store. Replacement starts a fresh undo history for the
named profile. Profile and export writes are atomic; a failed
import write also keeps the in-memory profile list intact. Exchange provides
no accounts, uploads or network transport.

### Local scalar feature-pack files

`muzaiten-features pack export|import FILE` exchanges local
`muzaiten-features-pack` v1 NDJSON files. There is no network transport,
account, upload or automatic pack generation. Export uses existing current
scalar analysis and never invokes a semantic provider. Taste, history and
ratings are absent, but decoded-audio content hashes identify recordings
and can reveal which recordings a library contains to anyone with the same
audio. Plain export prints that disclosure before writing, and JSON reports
include it in `contents`. Review it before sharing a pack.
Fingerprints and MusicBrainz identifiers are not
private; neither is included in v1.

The header contains format, version and a record count. Each group record
contains its representative's exact decoded-audio hash with that file's own
extractor/version and nine scalar features, so the values always describe the
audio the hash names. No paths, filenames, file sizes, mtimes,
analysis times, internal group IDs, history, state, ratings, timestamps or
embeddings are included. Import only adopts an exact local representative
hash with the running binary's compatible extractor/version. Matching tags,
MusicBrainz IDs, fingerprints or other members of a group cannot substitute
for that key.

The complete pack is validated before store access. Unknown versions or
keys, duplicates, malformed/truncated JSON, invalid UTF-8 and values,
incorrect counts, files over 64 MiB, more than 213,044 records and lines over
16 KiB excluding the newline are rejected without changing the feature store.
The record guard permits every valid pack within the byte cap. `--dry-run` previews
the same adoption decisions without writing rows or export files. Import
holds the native writer lock and commits all adopted rows in one transaction;
operational write failures roll back that transaction. Current feature rows
are preserved unless `--replace` is explicit. Stale rows from another DSP
version never enter scoring and the next refresh recomputes them, so import
replaces them without it. Imported rows obey normal DSP-version freshness and never activate
embeddings or modify listening state.

Export uses atomic writes, requires `--replace` for an existing file and
refuses symlink destinations or the feature database and its sidecars.
Failed export writes retain an existing destination. Local rows that fail the
same validation are skipped and counted as invalid, never written to a pack. See
[features-schema.md](features-schema.md#local-feature-pack-format) for
exact payload keys and report counts.

### Aggregate taste-profile files

`Radio > Taste profiles…` and `muzaitenctl radio-taste` exchange local
`muzaiten-taste-profile` version 1 JSON files. The contents are exactly:

- `format`: `muzaiten-taste-profile`; `version`: 1.
- `genres`: canonical genre labels and normalized shares.
- `features`: optional tempo-BPM and energy q25/q50/q75 quartiles.
- Optional `decades`: normalized decade shares.

Export summarizes the playable tracks inside source directories with
**Library** enabled; a directory hidden from the library stays out of the
file. Tracks marked **Don't learn from this** are left out, as that flag
keeps a track out of every taste signal. Tracks marked **Never play on radio**
are included: they remain part of the library's composition, which is what
the file describes.

Export excludes buckets backed by fewer than five distinct songs and feature
distributions backed by fewer than five distinct samples. Duplicated song
identities and content groups count once. Raw counts and discarded bucket
labels are absent. Shares and energy quartiles are rounded to hundredths (each
share set sums to exactly 1.00) and tempo quartiles to whole BPM, so the file
pins down neither exact bucket counts nor individual tracks' extractor values.
The file/preview contain no artist, album, track, MusicBrainz ID, path, timestamp, skip, rating,
listening-history row, content hash or per-record identifier. The user-chosen
seed name stays local and is not an exchange field.

These aggregates reveal library taste. Suppression does not guarantee
anonymity, and an imported aggregate cannot prove its source support floor.
Genre labels are library tags: curate them before exporting. No file is
automatically generated or exported. Every CLI export/preview requires
`--opt-in`, with no saved consent. GUI export starts with a deliberate preview
action and writes only after **Export these contents…**; that exact preview
is the written file. Nothing is uploaded or transmitted by the exchange UI.

Import validates and previews before writes. Unknown keys or versions,
duplicate keys, invalid UTF-8, incomplete JSON, nonfinite/out-of-range values,
invalid shares and files exceeding 64 KiB are rejected. A same-name seed
requires explicit replacement; export file replacement also needs consent.
Imported seeds live in `config/radio-taste-seeds.json` under a combined root
(or the configured configuration directory), with at most 32 named seeds.
Import/removal use a local write lock and an atomic file replacement. Rejected
imports leave that store and library/features/state databases untouched.

Selection is a separate user action. A seed adds 0 to 3 score points without
relaxing radio eligibility or throttles. **Stop radio** deactivates it;
GUI removal also stops the matching active seed. Offline removal prevents
future selection while existing sessions retain their snapshot until stopped.
The snapshot and local seed name are part of local radio session state, so
replacement/removal cannot corrupt a saved session. Ordinary saved sessions
need no taste fields. No network transport, accounts, URLs or external sharing
are provided. See [radio.md](radio.md#aggregate-taste-seeds) and
[features-schema.md](features-schema.md#aggregate-taste-profile-format).

### Storage locations

Application-owned state belongs under XDG locations:

- config: `$XDG_CONFIG_HOME/muzaiten`
- data: `$XDG_DATA_HOME/muzaiten`
- state: `$XDG_STATE_HOME/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten`

## Adding new write paths

Any future feature that writes to the library must stay explicit, previewed,
narrowly scoped, verified after write, and tested against disposable fixture
files before it is enabled for a real library.
