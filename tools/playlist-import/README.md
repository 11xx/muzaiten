# Playlist import tooling

`muzaiten-import` converts external playlist exports into the JSONL format
consumed by muzaiten. It is a standalone tool: it does not build, launch, or
modify the application, its database, or an existing music library. Two
subcommands:

- `muzaiten-import convert` — the offline CSV converter (this document's bulk).
- `muzaiten-import youtube` — the network-using `yt-dlp` enricher (see below).

The examples below invoke the script from the repo (`tools/playlist-import/muzaiten-import`,
which is executable); once installed it is simply `muzaiten-import` on `PATH`.

The JSONL contract is [documented here](../../docs/playlist-import-jsonl.md).

## `convert` — supported input

- Soundiiz CSV exports: semicolon-delimited `title`, `artist`, `album`, `isrc`,
  `addedDate`, `duration`, and `url` columns. The converter retains duration,
  derives a namespaced external identifier from a YouTube URL, ISRC, or Spotify
  URL, and writes `addedAt` only when `addedDate` is a valid offset-bearing
  ISO-8601 timestamp. Empty or invalid source dates remain absent rather than
  being replaced with conversion time.
- Rdio CSV exports: comma-delimited `Name`, `Artist`, `Album`, and `Track Number`
  columns. A matching Rdio XSPF sidecar is used only after its ordered
  title/artist sequence exactly matches the CSV; it supplies ISRCs and durations.
  XSPF durations are milliseconds. Values that are implausibly long but exactly
  1,000× a plausible 1-second-to-2-hour track duration are repaired; other
  implausible values are omitted and reported.
- SoundCloud CSV exports: a matching XML sidecar can supply numeric track IDs,
  retained as `soundcloud:<id>` external identifiers.
- muzaiten central authoritatives: a file named `<service>--gen-N--<kind>` (e.g.
  `spotify--gen-2--authoritative`, `spotify--gen-4--a-z`) is parsed by its CSV
  dialect like any other input; the name only sets a tidy playlist name
  ("Spotify Gen 2") and a `muzaiten central authoritative` provenance line. The
  Spotify/Rdio central authoritatives are Soundiiz-dialect and convert directly.
  The YouTube `mus--gen-N` central files are `Video ID`/timestamp lists with no
  title and are skipped — they require metadata enrichment (e.g. yt-dlp) that
  this offline tool does not perform.

It recursively discovers CSVs, skips known metadata files, and writes one JSONL
file per non-empty playlist. Every output begins with a playlist header followed
by one item per input row. Source CSV line order remains the canonical playlist
order; no ordinal is written.

The header comment is readable provenance, for example:

```text
Source: Spotify via Soundiiz
Exported: 2020-11-09 20:48:16 UTC (filename timestamp)
File: Spotify - Forza - (484) playlistCSV1604954896.csv
```

When the filename does not carry a plausible `playlistCSV<epoch>` suffix, the
second line is instead `File modified: … UTC (filesystem timestamp)`. That is
only provenance for the exported file, never a claimed playlist creation date.
The converter does not synthesize per-item comments.
When a verified sidecar contributes metadata, the header appends a
`Supplemented by: <filename>` provenance line. Unpaired XSPF/XML files are not
emitted as duplicate playlists.

## `youtube` — YouTube enrichment

`convert` is offline and cannot handle YouTube, whose exports carry only video
ids. The `youtube` subcommand is the network-using companion: it resolves
metadata with `yt-dlp` and emits the same JSONL. It is a separate subcommand so
`convert` keeps its offline guarantee; this one is slow, rate-limited, cached,
and meant to be run interactively.

Inputs are **positional and auto-detected** — mix any number freely. Each is
classified by what it is, not by a flag:

- **Takeout CSV** (the primary use) — a file (or a directory of them) carrying
  the Google Takeout header: the `Video ID,<timestamp>` table or localized
  `ID do vídeo,Horário da adição`, with or without a playlist-metadata preamble.
  Per-video timestamps become `addedAt`. → one `.jsonl` per CSV.
- **playlist URL** — a YouTube / YouTube Music link with a `list=` query or
  `/playlist` path, enumerated with `yt-dlp --flat-playlist`. This lets the
  app's internal YouTube playlist parsing be superseded by importing the JSONL.
  → one `.jsonl` per playlist.
- **video URL/id** — a single `watch`/`youtu.be`/`shorts` URL or a bare 11-char
  id. All loose videos on the command line are grouped → one `youtube-videos.jsonl`.
- **links file** — a text file of video URLs/ids, one per line (`#` comments and
  blank lines ignored). → one `.jsonl` per file.

Detection is unambiguous (path vs URL; a Takeout header vs not; `list=` vs not).
The `--takeout` / `--playlist` / `--video` / `--links` flags are optional
**filters**: pass one or more to keep only inputs of those kinds (e.g.
`--playlist` skips loose videos and files); with none, every detected kind is
processed. A filtered-out playlist URL is never fetched.

A YouTube Music `track`/`artist`/`album` is used when present; otherwise the
title is split on `Artist - Title` and an `<Artist> - Topic` channel is
recognised. Unavailable/deleted videos (yt-dlp placeholder titles) are skipped.
`--cache FILE` memoizes resolved metadata so re-runs are cheap; `--sleep`
rate-limits fetches; `--dry-run` parses inputs and lists ids without any network.

```sh
# auto-detected mix: a Takeout dir, a playlist URL, a links file, a loose video
tools/playlist-import/muzaiten-import youtube --out ./converted --cache ./yt-cache.json \
  "./Takeout/YouTube and YouTube Music/playlists" \
  "https://www.youtube.com/playlist?list=PL..." \
  ./links.txt https://youtu.be/dQw4w9WgXcQ
# keep only the playlists from a mixed dump
tools/playlist-import/muzaiten-import youtube --out ./converted --playlist ./dump.txt --sleep 1.5
```

## `convert` — usage

Preview discovery without writing output:

```sh
tools/playlist-import/muzaiten-import convert \
  --src ./exports \
  --out ./converted-playlists \
  --dry-run
```

Write the JSONL files to a dedicated output directory:

```sh
tools/playlist-import/muzaiten-import convert \
  --src ./exports \
  --out ./converted-playlists
```

Soundiiz exports commonly use timezone-less `addedDate` values. To preserve
them, declare their source timezone explicitly; otherwise they remain absent:

```sh
tools/playlist-import/muzaiten-import convert \
  --src ./exports \
  --out ./converted-playlists \
  --naive-timezone America/Sao_Paulo
```

The timezone must be an IANA name. Nonexistent local times are omitted; repeated
local times use the earlier UTC offset.

Review the generated files before importing them through the playlist-import
dialog. Re-running against the same output directory overwrites same-named
outputs; use a fresh directory when preserving an earlier conversion matters.
