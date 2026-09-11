#!/usr/bin/env python3
"""Shared PEP 440 version calculation for native builds and packages."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess


def describe(root: Path) -> dict:
    def git(*args: str) -> str:
        return subprocess.check_output(["git", "-C", str(root), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()

    try:
        if Path(git("rev-parse", "--show-toplevel")).resolve() != root.resolve():
            raise ValueError("not a repository root")
        timestamp = int(git("show", "-s", "--format=%ct", "HEAD"))
    except (OSError, ValueError, subprocess.CalledProcessError):
        return {"version": "0.0.0+unknown", "project_version": "0.0.0", "release": False}

    day = datetime.fromtimestamp(timestamp, timezone.utc).date()
    date = (day.year, day.month, day.day)
    tags = []
    for tag in git("tag", "--merged", "HEAD").splitlines():
        if re.fullmatch(r"[0-9]{4}\.[0-9]{1,2}\.[0-9]{1,2}(?:\.[0-9]+)?", tag):
            parts = tuple(map(int, tag.split(".")))
            try:
                datetime(*parts[:3])
            except ValueError:
                continue
            tags.append((parts, tag))
    exact = set(git("tag", "--points-at", "HEAD").splitlines())
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal"))
    releases = [(parts, tag) for parts, tag in tags if tag in exact]
    if releases and not dirty:
        parts, _ = max(releases)
        version = ".".join(map(str, parts))
        return {"version": version, "project_version": version, "release": True}

    recent = [(parts, tag) for parts, tag in tags if parts[:3] >= date]
    if recent:
        parts, tag = max(recent)
        base = (*parts[:3], (parts[3] if len(parts) == 4 else 0) + 1)
        count = git("rev-list", "--count", f"{tag}..HEAD")
    else:
        base = date
        count = git("rev-list", "--count", f"--since={day.isoformat()}T00:00:00Z", "HEAD")
    project = ".".join(map(str, base))
    local = "g" + git("rev-parse", "--short", "HEAD") + (".dirty" if dirty else "")
    return {"version": f"{project}a0.dev{count}+{local}", "project_version": project,
            "release": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--release", action="store_true", help="require a clean date-tagged commit")
    args = parser.parse_args()
    result = describe(args.repo)
    if args.release and not result["release"]:
        parser.error("release packaging requires a clean date-tagged commit")
    print(json.dumps(result) if args.json else result["version"])


if __name__ == "__main__":
    main()
