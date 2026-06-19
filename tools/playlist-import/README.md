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
  columns.

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

Review the generated files before importing them through the playlist-import
dialog. Re-running against the same output directory overwrites same-named
outputs; use a fresh directory when preserving an earlier conversion matters.
