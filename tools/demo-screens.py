#!/usr/bin/env python3
"""Capture demos from fresh SQLite snapshots and replace generated images."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import sqlite3
import struct
import subprocess
import tempfile


def snapshot(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(source.resolve().as_uri() + "?mode=ro", uri=True) as src:
        with sqlite3.connect(destination) as dst:
            src.backup(dst)


def copy_profile(root):
    home = Path.home()
    homes = {
        "data": Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share"),
        "state": Path(os.environ.get("XDG_STATE_HOME") or home / ".local/state"),
        "cache": Path(os.environ.get("XDG_CACHE_HOME") or home / ".cache"),
    }
    for area, names in {"data": ("library.sqlite", "playlists.sqlite", "history.sqlite"),
                        "state": ("state.sqlite",), "cache": ("artwork.sqlite",)}.items():
        for name in names:
            source = homes[area] / "muzaiten" / name
            if source.is_file():
                snapshot(source, root / area / name)
            elif name == "library.sqlite":
                raise RuntimeError(f"Library database not found: {source}")


def animated_png(path):
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError(f"Invalid PNG: {path}")
        while header := stream.read(8):
            if len(header) != 8:
                raise RuntimeError(f"Truncated PNG: {path}")
            length, kind = struct.unpack(">I4s", header)
            if kind == b"acTL":
                return True
            if kind == b"IEND":
                return False
            stream.seek(length + 4, 1)
    raise RuntimeError(f"Incomplete PNG: {path}")


def enabled(name, default="1"):
    return os.environ.get(name, default).lower() not in ("0", "false", "no")


def optimize(paths):
    if not enabled("DEMO_OPTIMIZE_PNG"):
        return
    tools = []
    if enabled("DEMO_PNG_LOSSY"):
        tools.append(("DEMO_PNG_QUANTIZER", "pngquant", "--force --skip-if-larger --ext .png --strip --speed 1 --quality 0-90", {0, 98, 99}))
    tools.append(("DEMO_PNG_OPTIMIZER", "oxipng", "-o 4 --strip safe", {0}))
    stills = [p for p in paths if not animated_png(p)]
    for key, default, flags, accepted in tools:
        tool = os.environ.get(key, default)
        executable = shutil.which(tool)
        if not executable:
            print(f"warning: {tool} not found; skipping optimization", flush=True)
            continue
        args = shlex.split(os.environ.get(key + "_FLAGS", flags))
        for path in stills:
            result = subprocess.run([executable, *args, str(path)], check=False)
            if result.returncode not in accepted:
                raise RuntimeError(f"{tool} failed for {path.name}: exit {result.returncode}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--output", required=True)
    args, capture_args = parser.parse_known_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Keep the database copies on the output filesystem, not a RAM-backed /tmp.
    with tempfile.TemporaryDirectory(prefix=".demo-run-", dir=output.parent) as temporary:
        run = Path(temporary)
        state = run / "profile"
        copy_profile(state)
        images = run / "images"
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", MUZAITEN_STATE_ROOT=str(state),
                   MUZAITEN_DEMO_SILENT_AUDIO="1")
        env.pop("MUZAITEN_DEV_STATE", None)
        for area in ("DATA", "STATE", "CACHE", "CONFIG"):
            env.pop(f"MUZAITEN_{area}_DIR", None)
        subprocess.run([str(Path(args.app).resolve()), "--demo-screens", str(images), *capture_args],
                       env=env, check=True)
        paths = sorted(images.rglob("*.png"))
        if not paths:
            raise RuntimeError("Capture produced no PNG images")
        optimize(paths)
        for path in paths:
            destination = output / path.relative_to(images)
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(path, destination)
        for directory in {output / p.relative_to(images).parent for p in paths}:
            for obsolete in ("05-explorer.png", "02-search.mp4"):
                (directory / obsolete).unlink(missing_ok=True)
        print(f"Updated {len(paths)} demo images in {output}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, sqlite3.Error, subprocess.CalledProcessError) as error:
        raise SystemExit(f"demo-screens: {error}") from error
