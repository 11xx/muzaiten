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
