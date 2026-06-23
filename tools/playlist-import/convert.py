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

A muzaiten "central" file named ``<service>--gen-N--<kind>`` (e.g.
``spotify--gen-2--authoritative``, ``spotify--gen-4--a-z``) is parsed by its CSV
dialect exactly like any other file; the recognised name only yields a tidy
playlist name ("Spotify Gen 2") and a "muzaiten central authoritative"
provenance line. The Spotify/Rdio central authoritatives are Soundiiz-dialect
and convert directly; their extra ``source``/``ordinal`` columns are ignored.

Files that are clearly not track lists (artist lists, comments, XML/XSPF
alternatives, spreadsheets, READMEs) are skipped. YouTube ``mus--gen-N`` central
files are ``Video ID``/timestamp lists with no title/artist and are skipped:
they need metadata enrichment (e.g. yt-dlp), which this offline tool does not do.

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
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError
import xml.etree.ElementTree as ET

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
_PLAYLIST_CSV_TIMESTAMP = re.compile(r"playlistCSV(?P<epoch>\d{9,11})(?:-XML)?\s*$", re.IGNORECASE)
_MIN_EXPORT_TIMESTAMP = 946684800       # 2000-01-01 UTC
_MAX_EXPORT_TIMESTAMP = 4102444800      # 2100-01-01 UTC
_MIN_TRACK_DURATION_MS = 1000
_MAX_TRACK_DURATION_MS = 2 * 60 * 60 * 1000
_ISRC = re.compile(r"^[A-Z]{2}[A-Z0-9]{3}\d{7}$")


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


def source_provenance(dialect: str, stem: str, path: Path) -> str:
    """Return the human-readable source line for the playlist header."""
    if dialect == "rdio":
        return "Rdio"
    service = source_label(stem, path)
    return "Soundiiz" if service.lower() in {"import", "soundiiz"} else f"{service} via Soundiiz"


# A muzaiten "central" authoritative/snapshot file: ``<service>--gen-<n>--<kind>``
# (e.g. ``spotify--gen-2--authoritative``, ``spotify--gen-4--a-z``). These are
# this archive's own assembled generations; recognising the convention only
# yields a nicer playlist name and provenance — it changes no parsing or
# selection. The file is still parsed by its detected CSV dialect like any other.
_CENTRAL_NAME = re.compile(r"^(spotify|rdio|mus)--gen-(\d+)--(.+)$", re.IGNORECASE)
_CENTRAL_SERVICE = {"spotify": "Spotify", "rdio": "Rdio", "mus": "YouTube Music"}


def central_meta(file_stem: str) -> tuple[str, str] | None:
    """(playlist name, provenance) for a central ``<service>--gen-N--<kind>``
    filename, or None when the name does not follow that convention."""
    match = _CENTRAL_NAME.match(file_stem.strip())
    if not match:
        return None
    service, number, kind = match.group(1).lower(), match.group(2), match.group(3).lower()
    name = f"{_CENTRAL_SERVICE[service]} Gen {number}"
    if kind != "authoritative":
        name += f" ({kind})"
    return name, "muzaiten central authoritative"


def format_utc(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def export_timestamp(path: Path) -> tuple[str, str]:
    """Use Soundiiz's filename timestamp when plausible, otherwise file mtime."""
    match = _PLAYLIST_CSV_TIMESTAMP.search(path.stem)
    if match:
        epoch = int(match.group("epoch"))
        if _MIN_EXPORT_TIMESTAMP <= epoch <= _MAX_EXPORT_TIMESTAMP:
            return "Exported", f"{format_utc(epoch)} (filename timestamp)"
    return "File modified", f"{format_utc(path.stat().st_mtime)} (filesystem timestamp)"


def playlist_comment(dialect: str, stem: str, path: Path, sidecars: list[Path] | None = None) -> str:
    timestamp_label, timestamp = export_timestamp(path)
    lines = [
        f"Source: {source_provenance(dialect, stem, path)}",
        f"{timestamp_label}: {timestamp}",
        f"File: {path.name}",
    ]
    lines.extend(f"Supplemented by: {sidecar.name}" for sidecar in sidecars or [])
    return "\n".join(lines)


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


def added_at(value: str, naive_timezone: ZoneInfo | None = None) -> int | None:
    """Parse an ISO-8601 source timestamp to epoch seconds when unambiguous."""
    text = (value or "").strip()
    if not text:
        return None
    if text.endswith(("Z", "z")):
        text = f"{text[:-1]}+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        if naive_timezone is None:
            return None
        # `fold=0` deliberately chooses the earlier offset when a local time is
        # repeated at DST fall-back. A UTC round trip detects nonexistent local
        # times at DST spring-forward, which must not be guessed.
        parsed = parsed.replace(tzinfo=naive_timezone, fold=0)
        round_trip = parsed.astimezone(timezone.utc).astimezone(naive_timezone)
        if round_trip.replace(tzinfo=None) != parsed.replace(tzinfo=None):
            return None
    timestamp = int(parsed.timestamp())
    return timestamp if timestamp > 0 else None


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


def parse_rows(path: Path, dialect: str, naive_timezone: ZoneInfo | None):
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
                timestamp = added_at(row.get("addeddate", ""), naive_timezone)
                if timestamp:
                    rec["addedAt"] = timestamp
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


# --- source-specific sidecar enrichment -------------------------------------


def local_name(element: ET.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def child_text(element: ET.Element, name: str) -> str:
    return next(((child.text or "").strip() for child in element if local_name(child) == name), "")


def parse_rdio_xspf(path: Path) -> list[dict] | None:
    """Read the Rdio-specific fields from an XSPF sidecar, if it is one."""
    try:
        playlist = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        print(f"  ! cannot read XSPF sidecar {path.name}: {exc}", file=sys.stderr)
        return None
    if not any(local_name(meta) == "meta" and meta.attrib.get("rel", "").startswith("https://rdio.com/xspf/")
               for meta in playlist):
        return None

    records = []
    for track in playlist.iter():
        if local_name(track) != "track":
            continue
        isrcs = next(((child.text or "").strip() for child in track
                      if local_name(child) == "meta"
                      and child.attrib.get("rel") == "https://rdio.com/xspf/t/isrcs"), "")
        records.append({
            "title": child_text(track, "title"),
            "artist": child_text(track, "creator"),
            "duration": child_text(track, "duration"),
            "isrcs": isrcs,
        })
    return records


def parse_soundcloud_xml(path: Path) -> list[dict] | None:
    """Read numeric SoundCloud track ids from the provider's XML sidecar."""
    try:
        playlist = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        print(f"  ! cannot read XML sidecar {path.name}: {exc}", file=sys.stderr)
        return None
    if local_name(playlist) != "playlist":
        return None
    records = []
    for track in playlist.iter():
        if local_name(track) != "track":
            continue
        records.append({
            "title": child_text(track, "title"),
            "artist": child_text(track, "artist"),
            "id": child_text(track, "id"),
        })
    return records


def matching_sidecar(records: list[dict], candidates: list[tuple[Path, list[dict] | None]]) -> tuple[Path, list[dict]] | None:
    """Require an unambiguous ordered title/artist match before enrichment."""
    matches = []
    for path, sidecar_records in candidates:
        if sidecar_records is None or len(records) != len(sidecar_records):
            continue
        if all((record.get("title", ""), record.get("artist", ""))
               == (sidecar.get("title", ""), sidecar.get("artist", ""))
               for record, sidecar in zip(records, sidecar_records)):
            matches.append((path, sidecar_records))
    return matches[0] if len(matches) == 1 else None


def first_isrc(value: str) -> str:
    for candidate in value.split(","):
        normalized = candidate.strip().upper()
        if _ISRC.fullmatch(normalized):
            return normalized
    return ""


def normalized_rdio_duration(value: str) -> tuple[int, bool]:
    """Return XSPF milliseconds, repairing only observed 1,000x Rdio outliers."""
    try:
        raw = int(value)
    except ValueError:
        return 0, False
    if _MIN_TRACK_DURATION_MS <= raw <= _MAX_TRACK_DURATION_MS:
        return raw, False
    if raw > _MAX_TRACK_DURATION_MS and raw % 1000 == 0:
        repaired = raw // 1000
        if _MIN_TRACK_DURATION_MS <= repaired <= _MAX_TRACK_DURATION_MS:
            return repaired, True
    return 0, False


def enrich_rdio(records: list[dict], path: Path) -> list[Path]:
    sidecar_path = path.with_suffix(".xspf")
    if not sidecar_path.is_file():
        return []
    match = matching_sidecar(records, [(sidecar_path, parse_rdio_xspf(sidecar_path))])
    if match is None:
        print(f"  ! Rdio XSPF sidecar not applied to {path.name}: ordered tracks differ", file=sys.stderr)
        return []
    _, sidecar_records = match
    corrected = 0
    for record, sidecar in zip(records, sidecar_records):
        if not record.get("durationMs"):
            duration, was_corrected = normalized_rdio_duration(sidecar["duration"])
            if duration:
                record["durationMs"] = duration
            corrected += was_corrected
        if not record.get("externalId"):
            isrc = first_isrc(sidecar["isrcs"])
            if isrc:
                record["externalId"] = f"isrc:{isrc}"
    detail = f"; repaired {corrected} duration values" if corrected else ""
    print(f"  + enriched from {sidecar_path.name}{detail}", file=sys.stderr)
    return [sidecar_path]


def enrich_soundcloud(records: list[dict], path: Path) -> list[Path]:
    if source_label(clean_stem(path.stem), path).lower() != "soundcloud":
        return []
    candidates = [(candidate, parse_soundcloud_xml(candidate))
                  for candidate in sorted(path.parent.glob("*.xml"))]
    match = matching_sidecar(records, candidates)
    if match is None:
        return []
    sidecar_path, sidecar_records = match
    for record, sidecar in zip(records, sidecar_records):
        if not record.get("externalId") and sidecar["id"].isdigit():
            record["externalId"] = f"soundcloud:{sidecar['id']}"
    print(f"  + enriched from {sidecar_path.name}", file=sys.stderr)
    return [sidecar_path]


def enrich_records(records: list[dict], dialect: str, path: Path) -> list[Path]:
    if dialect == "rdio":
        return enrich_rdio(records, path)
    return enrich_soundcloud(records, path)


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


def convert(src: Path, out_dir: Path, dry_run: bool, naive_timezone: ZoneInfo | None = None) -> int:
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

        records = list(parse_rows(path, dialect, naive_timezone))
        if not records:
            skipped += 1
            continue

        stem = clean_stem(path.stem)
        sidecars = enrich_records(records, dialect, path)
        central = central_meta(path.stem)
        if central:
            name, provenance = central
            comment_lines = [f"Source: {provenance}", f"File: {path.name}"]
            comment_lines.extend(f"Supplemented by: {s.name}" for s in sidecars)
            comment = "\n".join(comment_lines)
        else:
            name = playlist_name(stem)
            comment = playlist_comment(dialect, stem, path, sidecars)
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
    parser.add_argument("--naive-timezone", metavar="IANA_ZONE",
                        help="interpret timezone-less Soundiiz addedDate values in this IANA timezone")
    args = parser.parse_args(argv)
    src = args.src.expanduser()
    out = args.out.expanduser()
    if not src.exists():
        parser.error(f"source does not exist: {src}")
    naive_timezone = None
    if args.naive_timezone:
        try:
            naive_timezone = ZoneInfo(args.naive_timezone)
        except ZoneInfoNotFoundError:
            parser.error(f"unknown IANA timezone: {args.naive_timezone}")
    return convert(src, out, args.dry_run, naive_timezone)


if __name__ == "__main__":
    raise SystemExit(main())
