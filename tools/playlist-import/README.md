# Playlist export converter

`convert.py` converts supported playlist exports into the JSONL format consumed
by muzaiten. It is a standalone, offline tool: it does not build, launch, or
modify the application, its database, or an existing music library.

The JSONL contract is [documented here](../../docs/playlist-import-jsonl.md).

## Supported input

- Soundiiz CSV exports: semicolon-delimited `title`, `artist`, `album`, `isrc`,
  `duration`, and `url` columns. The converter retains duration and derives a
  namespaced external identifier from a YouTube URL, ISRC, or Spotify URL.
- Rdio CSV exports: comma-delimited `Name`, `Artist`, `Album`, and `Track Number`
  columns.

It recursively discovers CSVs, skips known metadata files, and writes one JSONL
file per non-empty playlist. Every output begins with a playlist header followed
by one item per input row.

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
