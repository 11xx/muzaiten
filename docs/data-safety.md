# Data Safety

muzaiten must assume the user's music library is irreplaceable.

## Library contract

muzaiten never moves, renames, or deletes files in the music library, and it
never writes artwork into album folders. Scans do not traverse symlinks, and no
automatic metadata lookup ever changes files on disk.

The single, deliberate exception is **rating tag writes**, which are off by
default and only happen when you ask for them:

- They are opt-in: triggered explicitly through `File > Rating tags`, or through
  the pending-write path after you change a rating with tag sync enabled.
- They are narrow: only the rating field is touched. No other tag, no file
  structure, no artwork.
- They are verified: every write is confirmed by re-reading the file, and a
  failed or unwritable write is kept pending for retry rather than silently
  dropped.

Rating edits always land in the application database first, so the database — not
your files — is the source of truth. Tag writes only ever mirror that state back
out when you opt in.

Missing files are marked missing rather than deleted from the database until you
explicitly choose `File > Remove missing tracks`.

## Listening history and scrobbling

Local listening history is permanent and independent of every scrobbling
service. Nothing a scrobbler does removes a listen from it.

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

Identifiers minted for custom destinations are never reused, so a removed
destination's records can never be inherited by a later one.

## Application state

Application-owned state belongs under XDG locations:

- config: `$XDG_CONFIG_HOME/muzaiten`
- data: `$XDG_DATA_HOME/muzaiten`
- state: `$XDG_STATE_HOME/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten`

## Adding new write paths

Any future feature that writes to the library must stay explicit, previewed,
narrowly scoped, verified after write, and tested against disposable fixture
files before it is enabled for a real library.
