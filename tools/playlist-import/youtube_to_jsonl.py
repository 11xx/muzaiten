#!/usr/bin/env python3
"""Enrich YouTube playlists into muzaiten's JSONL interchange format.

This is a dedicated, network-using companion to ``convert.py``. ``convert.py``
is strictly offline and handles already-rich CSV exports (Soundiiz/Rdio/etc.);
it deliberately cannot handle YouTube, whose exports carry only video IDs. This
tool fills that gap: it resolves YouTube metadata with ``yt-dlp`` and emits the
same JSONL documented in ``docs/playlist-import-jsonl.md``.

It exists as a separate script (rather than a mode of ``convert.py``) because the
concerns are different: this one needs the network, is slow and rate-limited,
caches results, and is meant to be run interactively. Keeping it apart preserves
``convert.py``'s offline guarantee.

Three input modes (combine freely; each input becomes one ``.jsonl``):

* ``--takeout PATH``   Google Takeout YouTube playlist CSV(s) — the primary use.
  The official Takeout format is a ``Video ID, <timestamp>`` table (English) or
  ``ID do vídeo, Horário da adição`` (localized), optionally preceded by a
  playlist-metadata preamble. Per-video added timestamps become ``addedAt``.
* ``--playlist URL``   A normal YouTube / YouTube Music playlist link. The video
  list is read with ``yt-dlp --flat-playlist`` and each entry enriched. This is
  the path that lets the app's internal YouTube playlist parsing be superseded by
  simply calling this script and importing the JSONL it produces.
* ``--links PATH``     A text file of YouTube video URLs/IDs, one per line
  (``#`` comments and blanks ignored).

Metadata mapping: a YouTube Music ``track``/``artist``/``album`` is used when
present; otherwise the video title is split on ``"Artist - Title"`` and an
``"<Artist> - Topic"`` channel is recognised. ``durationMs = duration * 1000``;
``externalId = "youtube:<id>"``. Items that resolve no title are skipped.

Usage:
    youtube_to_jsonl.py --out ./converted --takeout ./Takeout/playlists
    youtube_to_jsonl.py --out ./converted --playlist "https://www.youtube.com/playlist?list=PL..."
    youtube_to_jsonl.py --out ./converted --links ./links.txt --sleep 1.5 --cache ./yt-cache.json
    youtube_to_jsonl.py --out ./converted --takeout f.csv --dry-run   # parse only, no network
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_YT_ID = re.compile(r"[A-Za-z0-9_-]{11}")
# yt-dlp's placeholder title for an unavailable/private/deleted video.
_PLACEHOLDER_TITLE = re.compile(r"^youtube video #[A-Za-z0-9_-]{11}$", re.IGNORECASE)
# Pull a video id out of a watch URL, youtu.be, music.youtube, or a bare id.
_URL_ID = re.compile(
    r"(?:v=|/shorts/|youtu\.be/|/watch/|/embed/)([A-Za-z0-9_-]{11})")
_TAKEOUT_HEADER = re.compile(r"^(video[\s_-]*id|id\s*do\s*v[íi]deo)\b", re.IGNORECASE)


# --- timestamp parsing ------------------------------------------------------

def parse_timestamp(text: str) -> int | None:
    """Takeout uses ISO-8601 (``...T...+00:00``) or ``YYYY-MM-DD HH:MM:SS UTC``."""
    text = (text or "").strip()
    if not text:
        return None
    if text.endswith(" UTC"):
        try:
            dt = datetime.strptime(text, "%Y-%m-%d %H:%M:%S UTC").replace(tzinfo=timezone.utc)
        except ValueError:
            return None
    else:
        iso = f"{text[:-1]}+00:00" if text.endswith(("Z", "z")) else text
        try:
            dt = datetime.fromisoformat(iso)
        except ValueError:
            return None
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
    ts = int(dt.timestamp())
    return ts if ts > 0 else None


# --- input parsing ----------------------------------------------------------

def extract_id(token: str) -> str | None:
    token = token.strip()
    m = _URL_ID.search(token)
    if m:
        return m.group(1)
    return token if _YT_ID.fullmatch(token) else None


def parse_takeout(path: Path) -> tuple[str, list[tuple[str, int | None]]] | None:
    """(playlist name, [(video_id, addedAt|None)]) from a Takeout playlist CSV.

    Handles both the bare ``Video ID,<timestamp>`` table and the composite file
    that prefixes a playlist-metadata block before the track section."""
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as exc:
        print(f"  ! cannot read {path.name}: {exc}", file=sys.stderr)
        return None
    header_idx = next((i for i, ln in enumerate(lines)
                       if ln and _TAKEOUT_HEADER.match(next(csv.reader([ln]))[0].strip())), None)
    if header_idx is None:
        return None
    items: list[tuple[str, int | None]] = []
    for ln in lines[header_idx + 1:]:
        if not ln.strip():
            continue
        cells = next(csv.reader([ln]))
        vid = cells[0].strip()
        if not _YT_ID.fullmatch(vid):
            continue
        items.append((vid, parse_timestamp(cells[1]) if len(cells) > 1 else None))
    stem = re.sub(r"[-_\s]*videos?$", "", path.stem, flags=re.IGNORECASE).strip()
    return stem or path.stem, items


def parse_links(path: Path) -> tuple[str, list[tuple[str, int | None]]] | None:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"  ! cannot read {path.name}: {exc}", file=sys.stderr)
        return None
    items = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        vid = extract_id(line)
        if vid:
            items.append((vid, None))
    return (path.stem or "youtube-links", items) if items else None


# --- yt-dlp -----------------------------------------------------------------

def ytdlp_json(target: str, flat: bool = False) -> dict | None:
    cmd = ["yt-dlp", "-J", "--no-warnings", "--ignore-no-formats-error"]
    if flat:
        cmd.append("--flat-playlist")
    cmd.append(target)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except FileNotFoundError:
        sys.exit("error: yt-dlp is not installed or not on PATH")
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None


def parse_playlist_url(url: str) -> tuple[str, list[tuple[str, int | None]]] | None:
    info = ytdlp_json(url, flat=True)
    if not info:
        print(f"  ! could not read playlist {url}", file=sys.stderr)
        return None
    name = (info.get("title") or "youtube-playlist").strip()
    items = [(e["id"], None) for e in info.get("entries") or [] if e.get("id")]
    return name, items


def pick_artist(info: dict) -> str:
    for key in ("artist", "creator"):
        if info.get(key):
            return str(info[key]).strip()
    artists = info.get("artists")
    if isinstance(artists, list) and artists:
        return ", ".join(str(a).strip() for a in artists if a)
    channel = (info.get("uploader") or info.get("channel") or "").strip()
    if channel.endswith(" - Topic"):                 # auto-generated music channel
        return channel[: -len(" - Topic")].strip()
    return ""


def record_from_info(info: dict, added_at: int | None) -> dict | None:
    """Map a yt-dlp info dict to a muzaiten JSONL item, or None if unusable."""
    is_music = bool(info.get("track"))
    title = str(info.get("track") or info.get("title") or "").strip()
    if not title or _PLACEHOLDER_TITLE.match(title):   # unavailable/deleted video
        return None
    artist = pick_artist(info)
    album = str(info.get("album") or "").strip()
    if not is_music and not artist and " - " in title:   # "Artist - Title"
        left, right = title.split(" - ", 1)
        artist, title = left.strip(), right.strip()
    if not title:
        return None
    rec: dict = {"title": title}
    if artist:
        rec["artist"] = artist
    if album:
        rec["album"] = album
    duration = info.get("duration")
    if isinstance(duration, (int, float)) and duration > 0:
        rec["durationMs"] = int(duration * 1000)
    if info.get("id"):
        rec["externalId"] = f"youtube:{info['id']}"
    if added_at:
        rec["addedAt"] = added_at
    return rec


# --- driver -----------------------------------------------------------------

def watch_url(video_id: str) -> str:
    return f"https://www.youtube.com/watch?v={video_id}"


def enrich(items, cache, sleep, dry_run, limit):
    """Resolve [(id, addedAt)] to JSONL records, with cache + rate limiting."""
    if limit:
        items = items[:limit]
    records, skipped, fetched = [], 0, 0
    for video_id, added_at in items:
        if dry_run:
            print(f"    would fetch youtube:{video_id}")
            continue
        info = cache.get(video_id)
        if info is None:
            if fetched and sleep:
                time.sleep(sleep)
            info = ytdlp_json(watch_url(video_id))
            fetched += 1
            if info is None:
                print(f"    ! unavailable: youtube:{video_id}", file=sys.stderr)
                skipped += 1
                continue
            cache[video_id] = info
        rec = record_from_info(info, added_at)
        if rec is None:
            skipped += 1
            continue
        records.append(rec)
    return records, skipped, fetched


def write_jsonl(out_dir: Path, name: str, comment: str, records: list[dict]) -> Path:
    safe = re.sub(r"[\\/:*?\"<>|]+", "_", name).strip() or "playlist"
    path = out_dir / f"{safe}.jsonl"
    with path.open("w", encoding="utf-8") as fh:
        fh.write(json.dumps({"playlist": {"name": name, "comment": comment}},
                            ensure_ascii=False) + "\n")
        for rec in records:
            fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
    return path


def iter_inputs(args):
    """Yield (kind, label, provenance, (name, items)) for every input."""
    for spec in args.takeout or []:
        path = Path(spec).expanduser()
        files = sorted(path.rglob("*.csv")) if path.is_dir() else [path]
        for file in files:
            parsed = parse_takeout(file)
            if parsed:
                yield "takeout", file.name, f"Source: YouTube (Google Takeout)\nFile: {file.name}", parsed
            else:
                print(f"  - skipped (not a Takeout playlist CSV): {file.name}", file=sys.stderr)
    for url in args.playlist or []:
        parsed = parse_playlist_url(url)
        if parsed:
            yield "playlist", url, f"Source: YouTube playlist\nURL: {url}", parsed
    for spec in args.links or []:
        path = Path(spec).expanduser()
        parsed = parse_links(path)
        if parsed:
            yield "links", path.name, f"Source: YouTube links\nFile: {path.name}", parsed


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=Path, help="output directory for .jsonl")
    parser.add_argument("--takeout", action="append", metavar="PATH",
                        help="Google Takeout playlist CSV or a directory of them")
    parser.add_argument("--playlist", action="append", metavar="URL",
                        help="a YouTube / YouTube Music playlist URL")
    parser.add_argument("--links", action="append", metavar="FILE",
                        help="a text file of YouTube video URLs/IDs, one per line")
    parser.add_argument("--cache", type=Path,
                        help="JSON cache of resolved metadata (read + updated); makes re-runs cheap")
    parser.add_argument("--sleep", type=float, default=1.0,
                        help="seconds to wait between yt-dlp fetches (default 1.0)")
    parser.add_argument("--limit", type=int, help="only process the first N items per input (testing)")
    parser.add_argument("--dry-run", action="store_true",
                        help="parse inputs and list video ids without calling yt-dlp")
    args = parser.parse_args(argv)
    if not (args.takeout or args.playlist or args.links):
        parser.error("provide at least one of --takeout / --playlist / --links")

    cache: dict = {}
    if args.cache and args.cache.exists():
        try:
            cache = json.loads(args.cache.read_text(encoding="utf-8"))
            print(f"loaded {len(cache)} cached entries from {args.cache}")
        except (OSError, json.JSONDecodeError):
            print(f"  ! ignoring unreadable cache {args.cache}", file=sys.stderr)

    args.out.mkdir(parents=True, exist_ok=True)
    total_playlists = total_items = total_skipped = total_fetched = 0
    for kind, label, provenance, (name, items) in iter_inputs(args):
        print(f"  {label}  [{kind}]  ({len(items)} videos)")
        records, skipped, fetched = enrich(items, cache, args.sleep, args.dry_run, args.limit)
        total_skipped += skipped
        total_fetched += fetched
        if args.dry_run:
            continue
        out_path = write_jsonl(args.out, name, provenance, records)
        total_playlists += 1
        total_items += len(records)
        print(f"    -> {out_path.name}  ({len(records)} items, {skipped} skipped)")
        if args.cache:                                # persist after each playlist
            args.cache.write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")

    if args.dry_run:
        print("\n(dry run: no network calls, no output written)")
    else:
        print(f"\n{total_playlists} playlists, {total_items} items -> {args.out}; "
              f"{total_skipped} skipped, {total_fetched} fetched via yt-dlp.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
