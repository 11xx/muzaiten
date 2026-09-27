#!/usr/bin/env python3
"""Compare fold segments from a checkout and a detached base revision."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from contextlib import closing
import csv
import fcntl
import hashlib
import importlib.util
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
CACHE = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "muzaiten/fold-sweep"
SEP = "\x1f"
CLASSES = ("worse", "better", "still-broken", "primary", "lost", "gained", "reordered")


def run(*args: str, cwd: Path = ROOT) -> None:
    subprocess.run(args, cwd=cwd, check=True, stdout=sys.stderr)


def output(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def cache_values(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def prepare_build(base_sha: str, head_build: Path, base_tree: Path) -> tuple[Path, Path, dict[str, str], float]:
    head = cache_values(head_build)
    generator = head["CMAKE_GENERATOR"]
    build_type = head.get("CMAKE_BUILD_TYPE", "")
    if not base_tree.exists():
        base_tree.parent.mkdir(parents=True, exist_ok=True)
        run("git", "worktree", "add", "--detach", str(base_tree), base_sha)
    else:
        if output("git", "status", "--porcelain", "--untracked-files=no", cwd=base_tree):
            raise ValueError(f"{base_tree}: base worktree has local changes")
        if output("git", "rev-parse", "HEAD", cwd=base_tree) != base_sha:
            run("git", "checkout", "--detach", base_sha, cwd=base_tree)
    base_build = base_tree.with_name(f"{base_tree.name}-build")
    if not (base_build / "CMakeCache.txt").exists():
        run("cmake", "-S", str(base_tree), "-B", str(base_build), "-G", generator,
            f"-DCMAKE_BUILD_TYPE={build_type}")
    else:
        base = cache_values(base_build)
        if base["CMAKE_GENERATOR"] != generator or base.get("CMAKE_BUILD_TYPE", "") != build_type:
            raise ValueError("base build generator or type differs from head build")
    start = time.monotonic()
    run("cmake", "--build", str(base_build), "--target", "muzaiten_search", "-j", "3")
    elapsed = time.monotonic() - start
    run("cmake", "--build", str(head_build), "--target", "muzaiten_search", "-j", "3")
    return base_build, head_build, head, elapsed


def compile_inspector(name: str, tree: Path, build: Path, compiler: str) -> Path:
    CACHE.mkdir(parents=True, exist_ok=True)
    binary = CACHE / name
    flags = output("pkg-config", "--cflags", "Qt6Core").split()
    libs = output("pkg-config", "--libs", "Qt6Core").split()
    command = [compiler, "-std=c++26", "-O2", "-fPIC", str(ROOT / "tools/fold-inspect.cpp"),
               f"-I{tree / 'src'}", *flags, str(build / "libmuzaiten_search.a")]
    core = build / "libmuzaiten_core.a"
    if core.exists():
        command.append(str(core))
    with tempfile.NamedTemporaryFile(prefix=f".{name}-", dir=CACHE, delete=False) as temporary:
        staged = Path(temporary.name)
    try:
        run(*command, *libs, "-o", str(staged))
        os.replace(staged, binary)
    finally:
        staged.unlink(missing_ok=True)
    return binary


def inspector(binary: Path, inputs: list[str]) -> list[tuple[str, ...]]:
    if any("\n" in value or "\r" in value or SEP in value for value in inputs):
        raise ValueError("corpus contains a line break or segment separator")
    result = subprocess.run([str(binary)], input="".join(f"{value}\n" for value in inputs),
                            text=True, capture_output=True, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(inputs):
        raise ValueError(f"{binary}: expected {len(inputs)} output lines, got {len(lines)}")
    return [tuple(line.split(SEP)) for line in lines]


def han_module():
    spec = importlib.util.spec_from_file_location("fold_han_data", ROOT / "tools/fold-han-data.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def cedict_rows() -> list[tuple[str, str, str]]:
    han = han_module()
    archive = Path.home() / ".cache/muzaiten" / han.CEDICT_CACHE_FILE
    if not archive.exists():
        raise ValueError(f"{archive}: missing CC-CEDICT archive; cache it with fold-han-data.py check")
    _, words = han.cedict_words(han.cedict_archive(archive))
    return [("cedict", word, "".join(syllables)) for word, readings in words.items()
            if word and all(any(start <= ord(char) <= end for start, end in han.IDEOGRAPH_BLOCKS)
                            for char in word)
            for syllables in sorted(readings)]


def phrase_rows(path: Path = ROOT / "tools/fold-sweep-phrases.tsv") -> list[tuple[str, str, str]]:
    rows = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not all(fields):
            raise ValueError(f"{path}:{number}: expected text<TAB>expected fold")
        rows.append(("phrases", *fields))
    return rows


def corpus_rows(paths: list[Path]) -> list[tuple[str, str, str]]:
    return [(f"corpus:{path.stem}", line, "") for path in paths for line in path.read_text().splitlines() if line]


def checked_library_path(path: Path, data_home: Path | None = None) -> Path:
    live = (data_home or Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")) / "muzaiten"
    resolved = path.resolve()
    if resolved.is_relative_to(live.resolve()):
        raise ValueError(f"{path}: live muzaiten data directory is not a library copy")
    return resolved


def library_rows(path: Path, data_home: Path | None = None) -> tuple[list[tuple[str, str, str]], int]:
    uri = f"{checked_library_path(path, data_home).as_uri()}?mode=ro&immutable=1"
    rows = []
    skipped = 0
    with closing(sqlite3.connect(uri, uri=True)) as db:
        for column in ("title", "artist_name", "album_artist_name", "album_title", "path"):
            for (value,) in db.execute(f"SELECT DISTINCT {column} FROM tracks WHERE {column} IS NOT NULL AND {column} != ''"):
                if any(char in value for char in ("\n", "\r", SEP)):
                    skipped += 1
                    continue
                rows.append(("library", value, ""))
    return rows, skipped


def classify(corpus: str, expected: str, base: tuple[str, ...], head: tuple[str, ...]) -> str | None:
    if expected and expected in base and expected not in head:
        return "worse"
    if expected and expected not in base and expected in head:
        return "better"
    if expected and expected not in base and expected not in head:
        return "still-broken"
    if base == head:
        return None
    if base[0] != head[0]:
        return "primary"
    if set(base) - set(head):
        return "lost"
    if set(head) - set(base):
        return "gained"
    return "reordered"


def evaluate(rows: list[tuple[str, str, str]], base: list[tuple[str, ...]],
             head: list[tuple[str, ...]]) -> tuple[list[tuple[str, str, str, str, str, str]], dict[str, Counter]]:
    changes = []
    counts: dict[str, Counter] = defaultdict(Counter)
    for (corpus, value, expected), old, new in zip(rows, base, head, strict=True):
        kind = classify(corpus, expected, old, new)
        counts[corpus][kind or "unchanged"] += 1
        if kind:
            changes.append((corpus, kind, value, expected, " | ".join(old), " | ".join(new)))
    return changes, counts


def report(changes, counts, examples: int, path: Path | None, skipped: int = 0) -> None:
    for corpus, counter in counts.items():
        print(f"{corpus}: " + ", ".join(f"{key}={counter[key]}" for key in ("unchanged", *CLASSES)))
    if skipped:
        print(f"library: skipped invalid values={skipped}")
    for kind in CLASSES:
        selected = [row for row in changes if row[1] == kind and not (row[0] == "cedict" and kind == "still-broken")]
        for row in selected[:examples]:
            print("\t".join(row))
    if path:
        with path.open("w", newline="") as file:
            writer = csv.writer(file, delimiter="\t")
            writer.writerow(("corpus", "class", "input", "expected", "base", "head"))
            writer.writerows(changes)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="master")
    parser.add_argument("--base-tree", type=Path, default=CACHE / "base")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--corpus", type=Path, action="append", default=[])
    parser.add_argument("--library", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--examples", type=int, default=20)
    parser.add_argument("--fail-on-regression", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.library:
            checked_library_path(args.library)
        sha = output("git", "rev-parse", "--verify", f"{args.base}^{{commit}}")
        head_build = args.build_dir.resolve()
        CACHE.mkdir(parents=True, exist_ok=True)
        with (CACHE / "base.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            base_build, _, head_cache, elapsed = prepare_build(sha, head_build, args.base_tree)
            print(f"base build: {elapsed:.2f}s", file=sys.stderr)
            base_binary = compile_inspector(f"base-{sha}", args.base_tree, base_build, cache_values(base_build)["CMAKE_CXX_COMPILER"])
        compiler = head_cache["CMAKE_CXX_COMPILER"]
        checkout = hashlib.sha256(str(ROOT).encode()).hexdigest()[:12]
        head_binary = compile_inspector(f"head-{checkout}", ROOT, head_build, compiler)
        rows = cedict_rows() + phrase_rows() + corpus_rows(args.corpus)
        skipped = 0
        if args.library:
            library, skipped = library_rows(args.library)
            rows += library
        inputs = list(dict.fromkeys(row[1] for row in rows))
        old = dict(zip(inputs, inspector(base_binary, inputs), strict=True))
        new = dict(zip(inputs, inspector(head_binary, inputs), strict=True))
        changes, counts = evaluate(rows, [old[row[1]] for row in rows], [new[row[1]] for row in rows])
        report(changes, counts, args.examples, args.report, skipped)
        return int(args.fail_on_regression and any(row[1] == "worse" for row in changes))
    except (OSError, ValueError, subprocess.CalledProcessError, sqlite3.Error, KeyError) as error:
        print(f"fold-sweep: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
