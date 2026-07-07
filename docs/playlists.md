# Playlists

Press `5` for the playlist management view: a playlist list on the left,
the selected playlist's items on the right, keyboard-first (`h`/`l` switch
panes, `j`/`k`/`n`/`p` move, `Enter` plays the focused playlist or item).

Playlists live in their own database, separate from the scanned library,
and each item carries a metadata snapshot plus the search query that
produced it — so a playlist survives rescans and keeps remembering tracks
even after they go missing (missing entries stay, marked with a red `×`).

## Importing

Add to an existing playlist or create a new one from:

- a pasted tracklist (one `Artist - Title` per line),
- an `m3u` / `m3u8`, `csv`, or `jsonl` / `ndjson` file — drag-drop a batch
  of files to get one playlist per file (JSONL format details:
  [playlist-import-jsonl.md](playlist-import-jsonl.md)), or
- a YouTube / YT Music playlist URL, fetched with `yt-dlp` (no account or
  paid middleman; the row is greyed out with an explanation when `yt-dlp`
  is missing).

Each line is resolved against the library by the same sound/shape matcher
used by search ([search.md](search.md)), so `Artist - Title` lines match
regardless of script or accents. Matches **stream into the preview live**
as they resolve. An **Exact only** toggle disables fuzzy/relaxed guessing
for stricter imports. Rows that resolve to several candidates (or to a
low-confidence best guess) are flagged for a quick triage pick — leave
as-is, choose one of the close candidates, or clear the match — before
committing. The original import text is preserved verbatim, so a row can
be re-matched later without losing where it came from.

For scripted conversion outside the app, the installed `muzaiten-import`
tool offers `convert` and `youtube` subcommands.

## Playing through a playlist

Playing a playlist mirrors it into the queue and keeps them linked: edits
flow both ways, and the track context menus offer "…(don't save to
playlist)" variants for queueing something transient without touching the
playlist. `Queue > Detach queue from playlist` breaks the link; saved
queues are managed from the same view as playlists.
