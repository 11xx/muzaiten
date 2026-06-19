#!/usr/bin/env python3
"""Convert exported playlist files into muzaiten's JSONL interchange format.

This is the external, app-independent converter referenced by Phase F of the
playlist-import plan. It reads playlist CSV exports and emits one ``.jsonl`` file
per playlist, matching the schema in ``docs/playlist-import-jsonl.md``. The app
never runs this; it only imports the ``.jsonl`` output.

Handled input dialects (auto-detected from the header):

* Soundiiz CSV   ``title;artist;album;isrc;addedDate;addedBy;duration;url``
  (semicolon-delimited, UTF-8 BOM; covers Spotify/Pandora/YouTube/Soundcloud
  exports). ``duration`` is like ``225s``; ``url`` yields a namespaced
  ``externalId`` (youtube:ID, else isrc:ISRC, else spotify:track:ID).
* Rdio CSV       ``Name,Artist,Album,Track Number`` (comma-delimited).

Files that are clearly not track lists (artist lists, comments, XML/XSPF
alternatives, spreadsheets, READMEs) are skipped.

Usage:
    convert.py --src ./exports --out ./converted-playlists
    convert.py --src <dir-or-file> --out <dir> [--dry-run]
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

# --- filename / playlist-name cleaning --------------------------------------

# Export-tool noise stripped from a source filename to recover the playlist name.
_SUFFIX_NOISE = re.compile(
    r"\s*(?:_files)?\s*(?:playlist(?:CSV|XSPF)?|XML\s*playlist)\s*\d+(?:-XML)?\s*$",
    re.IGNORECASE,
)
_COUNT_TAG = re.compile(r"\s*[-–]?\s*\(\s*\d+\s*(?:DUPPED)?\s*\)\s*$", re.IGNORECASE)
_SOURCE_PREFIX = re.compile(
    r"^(Spotify(?:\s+Generated)?|YouTube\s*Music|Youtube\s*Music|YT\s*Music|"
    r"Soundcloud|Pandora|Rdio|Soundiiz)\s*[-–]\s*",
    re.IGNORECASE,
)


def clean_stem(name: str) -> str:
    """Source basename (no extension) with export-tool digits/suffixes removed."""
    stem = _SUFFIX_NOISE.sub("", name).strip()
    stem = re.sub(r"\s{2,}", " ", stem)
    return stem.strip(" -–")


def playlist_name(stem: str) -> str:
    """Human playlist name: cleaned stem minus a leading source + count tag."""
    name = _COUNT_TAG.sub("", stem).strip()
    no_prefix = _SOURCE_PREFIX.sub("", name).strip()
    return no_prefix or name or stem


def source_label(stem: str, path: Path) -> str:
    m = _SOURCE_PREFIX.match(stem)
    if m:
        return m.group(1).strip()
    # Fall back to a containing directory hint (e.g. Rdio export trees).
    parts = [p.lower() for p in path.parts]
    for known in ("rdio", "soundiiz", "pandora", "yt music"):
        if any(known in p for p in parts):
            return known.title()
    return "import"


# --- field helpers ----------------------------------------------------------

_SPOTIFY = re.compile(r"open\.spotify\.com/track/([A-Za-z0-9]+)")
_YOUTUBE = re.compile(r"(?:music\.)?youtube\.com/watch\?v=([\w-]+)")


def duration_ms(value: str) -> int:
    value = (value or "").strip().lower().rstrip("s").strip()
    if not value:
        return 0
    try:
        secs = int(float(value))
    except ValueError:
        return 0
    return secs * 1000 if secs > 0 else 0


def external_id(url: str, isrc: str) -> str:
    url = (url or "").strip()
    isrc = (isrc or "").strip()
    if url:
        m = _YOUTUBE.search(url)
        if m:
            return f"youtube:{m.group(1)}"
    if isrc:
        return f"isrc:{isrc}"
    if url:
        m = _SPOTIFY.search(url)
        if m:
            return f"spotify:track:{m.group(1)}"
    return ""


# --- dialect detection + row parsing ----------------------------------------

# Basenames / patterns that are never track lists.
_SKIP_NAME = re.compile(
    r"^(artists|artistsCSV|result_playlists|favorites_(artists|labels|stations)|"
    r"comments|downloaded)\b",
    re.IGNORECASE,
)


def detect_dialect(header_line: str):
    low = header_line.lstrip("﻿").lower()
    if ";" in low and "title" in low and "artist" in low:
        return "soundiiz"
    if "," in low and "name" in low and "artist" in low:
        return "rdio"
    return None


def parse_rows(path: Path, dialect: str):
    """Yield JSONL record dicts for each track row."""
    delimiter = ";" if dialect == "soundiiz" else ","
    with path.open("r", encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh, delimiter=delimiter)
        # Normalize header keys to lowercase for stable lookup.
        for raw in reader:
            row = {(k or "").strip().lower(): (v or "").strip() for k, v in raw.items()}
            if dialect == "soundiiz":
                title = row.get("title", "")
                if not title:
                    continue
                rec = {"title": title}
                if row.get("artist"):
                    rec["artist"] = row["artist"]
                if row.get("album"):
                    rec["album"] = row["album"]
                ms = duration_ms(row.get("duration", ""))
                if ms:
                    rec["durationMs"] = ms
                ext = external_id(row.get("url", ""), row.get("isrc", ""))
                if ext:
                    rec["externalId"] = ext
            else:  # rdio
                title = row.get("name", "")
                if not title:
                    continue
                rec = {"title": title}
                if row.get("artist"):
                    rec["artist"] = row["artist"]
                if row.get("album"):
                    rec["album"] = row["album"]
            yield rec


# --- discovery + conversion -------------------------------------------------


def iter_csv_files(src: Path):
    if src.is_file():
        yield src
        return
    for path in sorted(src.rglob("*.csv")):
        if _SKIP_NAME.match(path.name):
            continue
        yield path


def unique_out_path(out_dir: Path, stem: str, used: set) -> Path:
    safe = re.sub(r"[\\/:*?\"<>|]+", "_", stem).strip() or "playlist"
    candidate = safe
    n = 2
    while candidate.lower() in used:
        candidate = f"{safe} ({n})"
        n += 1
    used.add(candidate.lower())
    return out_dir / f"{candidate}.jsonl"


def convert(src: Path, out_dir: Path, dry_run: bool) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    used: set = set()
    total_files = total_tracks = skipped = 0

    for path in iter_csv_files(src):
        try:
            with path.open("r", encoding="utf-8-sig") as fh:
                header = fh.readline()
        except OSError as exc:
            print(f"  ! cannot read {path.name}: {exc}", file=sys.stderr)
            skipped += 1
            continue
        dialect = detect_dialect(header)
        if dialect is None:
            skipped += 1
            continue

        records = list(parse_rows(path, dialect))
        if not records:
            skipped += 1
            continue

        stem = clean_stem(path.stem)
        name = playlist_name(stem)
        comment = f"from {source_label(stem, path)} ({path.name})"
        out_path = unique_out_path(out_dir, stem, used)

        total_files += 1
        total_tracks += len(records)
        print(f"  {path.name}  [{dialect}]  -> {out_path.name}  ({len(records)} tracks)")
        if dry_run:
            continue
        with out_path.open("w", encoding="utf-8") as fh:
            fh.write(json.dumps({"playlist": {"name": name, "comment": comment}},
                                ensure_ascii=False) + "\n")
            for rec in records:
                fh.write(json.dumps(rec, ensure_ascii=False) + "\n")

    print(f"\n{total_files} playlists, {total_tracks} tracks"
          f"{' (dry run)' if dry_run else f' -> {out_dir}'}; {skipped} files skipped.")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", required=True, type=Path,
                        help="source directory (searched recursively) or a single CSV")
    parser.add_argument("--out", required=True, type=Path,
                        help="output directory for the .jsonl files")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would be written without writing")
    args = parser.parse_args(argv)
    src = args.src.expanduser()
    out = args.out.expanduser()
    if not src.exists():
        parser.error(f"source does not exist: {src}")
    return convert(src, out, args.dry_run)


if __name__ == "__main__":
    raise SystemExit(main())
