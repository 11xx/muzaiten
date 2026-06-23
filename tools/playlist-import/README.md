# Playlist export converter

`convert.py` converts supported playlist exports into the JSONL format consumed
by muzaiten. It is a standalone, offline tool: it does not build, launch, or
modify the application, its database, or an existing music library.

The JSONL contract is [documented here](../../docs/playlist-import-jsonl.md).

## Supported input

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

## YouTube enrichment (`youtube_to_jsonl.py`)

`convert.py` is offline and cannot handle YouTube, whose exports carry only video
ids. `youtube_to_jsonl.py` is the dedicated, network-using companion: it resolves
metadata with `yt-dlp` and emits the same JSONL. It is kept separate so
`convert.py` keeps its offline guarantee; this one is slow, rate-limited, cached,
and meant to be run interactively.

Three input modes (combine freely; each input becomes one `.jsonl`):

- `--takeout PATH` — Google Takeout YouTube playlist CSV(s) (the primary use):
  the `Video ID,<timestamp>` table or localized `ID do vídeo,Horário da adição`,
  with or without a playlist-metadata preamble. Per-video timestamps become
  `addedAt`.
- `--playlist URL` — a normal YouTube / YouTube Music playlist link, enumerated
  with `yt-dlp --flat-playlist`. This lets the app's internal YouTube playlist
  parsing be superseded by calling this script and importing its JSONL.
- `--links FILE` — a text file of YouTube video URLs/ids, one per line.

A YouTube Music `track`/`artist`/`album` is used when present; otherwise the
title is split on `Artist - Title` and an `<Artist> - Topic` channel is
recognised. Unavailable/deleted videos (yt-dlp placeholder titles) are skipped.
`--cache FILE` memoizes resolved metadata so re-runs are cheap; `--sleep`
rate-limits fetches; `--dry-run` parses inputs and lists ids without any network.

```sh
python3 tools/playlist-import/youtube_to_jsonl.py --out ./converted \
  --takeout "./Takeout/YouTube and YouTube Music/playlists" --cache ./yt-cache.json
python3 tools/playlist-import/youtube_to_jsonl.py --out ./converted \
  --playlist "https://www.youtube.com/playlist?list=PL..." --sleep 1.5
```

## Usage

Preview discovery without writing output:

```sh
python3 tools/playlist-import/convert.py \
  --src ./exports \
  --out ./converted-playlists \
  --dry-run
```

Write the JSONL files to a dedicated output directory:

```sh
python3 tools/playlist-import/convert.py \
  --src ./exports \
  --out ./converted-playlists
```

Soundiiz exports commonly use timezone-less `addedDate` values. To preserve
them, declare their source timezone explicitly; otherwise they remain absent:

```sh
python3 tools/playlist-import/convert.py \
  --src ./exports \
  --out ./converted-playlists \
  --naive-timezone America/Sao_Paulo
```

The timezone must be an IANA name. Nonexistent local times are omitted; repeated
local times use the earlier UTC offset.

Review the generated files before importing them through the playlist-import
dialog. Re-running against the same output directory overwrites same-named
outputs; use a fresh directory when preserving an earlier conversion matters.
