#!/usr/bin/env python3
"""Offline failure checks for fold sweep classification and input safety."""
import importlib.util
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import io
import contextlib

spec = importlib.util.spec_from_file_location("fold_sweep", Path(__file__).resolve().parents[1] / "tools/fold-sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


class SweepTests(unittest.TestCase):
    def test_expected_reading_loss_controls_exit(self):
        rows = [("cedict", "女人", "nuren")]
        changes, counts = sweep.evaluate(rows, [("女人", "nuren")], [("女人",)])
        self.assertEqual(changes[0][1], "worse")
        self.assertEqual(counts["cedict"]["worse"], 1)
        with tempfile.TemporaryDirectory() as cache_dir, patch.object(sweep, "CACHE", Path(cache_dir)), patch.object(sweep, "prepare_build", return_value=(Path("base"), Path("head"), {"CMAKE_CXX_COMPILER": "c++"}, 0)), \
             patch.object(sweep, "compile_inspector", side_effect=[Path("base"), Path("head")]), \
             patch.object(sweep, "cache_values", return_value={"CMAKE_CXX_COMPILER": "c++"}), \
             patch.object(sweep, "cedict_rows", return_value=rows), \
             patch.object(sweep, "phrase_rows", return_value=[]), \
             patch.object(sweep, "inspector", side_effect=[[('女人', 'nuren')], [('女人',)]]), \
             patch.object(sweep, "output", return_value="0123456789abcdef"), \
             patch.object(sweep, "report"):
            self.assertEqual(sweep.main([]), 0)
        with tempfile.TemporaryDirectory() as cache_dir, patch.object(sweep, "CACHE", Path(cache_dir)), patch.object(sweep, "prepare_build", return_value=(Path("base"), Path("head"), {"CMAKE_CXX_COMPILER": "c++"}, 0)), \
             patch.object(sweep, "compile_inspector", side_effect=[Path("base"), Path("head")]), \
             patch.object(sweep, "cache_values", return_value={"CMAKE_CXX_COMPILER": "c++"}), \
             patch.object(sweep, "cedict_rows", return_value=rows), \
             patch.object(sweep, "phrase_rows", return_value=[]), \
             patch.object(sweep, "inspector", side_effect=[[('女人', 'nuren')], [('女人',)]]), \
             patch.object(sweep, "output", return_value="0123456789abcdef"), \
             patch.object(sweep, "report"):
            self.assertEqual(sweep.main(["--fail-on-regression"]), 1)

    def test_alternate_loss(self):
        self.assertEqual(sweep.classify("cedict", "nuren", ("女人", "nuren", "alternate"),
                                        ("女人", "nuren")), "lost")

    def test_still_broken(self):
        changes, _ = sweep.evaluate([("phrases", "看着急", "kanzheji")],
                                    [("kanzhaoji",)], [("kanzhaoji",)])
        self.assertEqual(changes[0][1], "still-broken")
        self.assertFalse(any(row[1] == "worse" for row in changes))

    def test_primary_change_with_same_segments(self):
        self.assertEqual(sweep.classify("corpus", "", ("a", "b"), ("b", "a")), "primary")

    def test_classes_and_external_corpus_labels(self):
        self.assertEqual(sweep.classify("phrases", "x", ("a",), ("x",)), "better")
        self.assertEqual(sweep.classify("cedict", "", ("a",), ("a", "b")), "gained")
        self.assertEqual(sweep.classify("cedict", "", ("a", "b", "c"), ("a", "c", "b")), "reordered")
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ("phrases.txt", "cedict.txt")]
            for path in paths:
                path.write_text("天地\n")
            rows = sweep.corpus_rows(paths)
            self.assertEqual([row[0] for row in rows], ["corpus:phrases", "corpus:cedict"])
            changes, _ = sweep.evaluate(rows, [("old",)] * 2, [("new",)] * 2)
            self.assertEqual([row[1] for row in changes], ["primary", "primary"])

    def test_report_includes_broken_cedict_rows(self):
        changes, counts = sweep.evaluate([("cedict", "字", "zi")], [("wrong",)], [("wrong",)])
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.tsv"
            with contextlib.redirect_stdout(io.StringIO()) as summary:
                sweep.report(changes, counts, 5, report, 2)
            self.assertIn("still-broken=1", summary.getvalue())
            self.assertIn("skipped invalid values=2", summary.getvalue())
            self.assertNotIn("\tstill-broken\t", summary.getvalue())
            self.assertIn("cedict\tstill-broken\t", report.read_text())

    def test_inspector_rejects_invalid_input_and_output(self):
        with self.assertRaisesRegex(ValueError, "line break"):
            sweep.inspector(Path("unused"), ["a\n"])
        with self.assertRaisesRegex(ValueError, "separator"):
            sweep.inspector(Path("unused"), ["a\x1fb"])
        with patch.object(subprocess, "run", return_value=type("Result", (), {"stdout": "one\n"})()):
            with self.assertRaisesRegex(ValueError, "expected 2 output lines"):
                sweep.inspector(Path("unused"), ["one", "two"])

    def test_phrase_rows_reject_bad_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phrases.tsv"
            for value in ("word", "word\t", "word\texpected\textra"):
                path.write_text(value + "\n")
                with self.assertRaisesRegex(ValueError, "expected text<TAB>expected fold"):
                    sweep.phrase_rows(path)

    def test_library_skips_values_inspector_cannot_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "library.sqlite"
            with sqlite3.connect(path) as db:
                db.execute("CREATE TABLE tracks (title TEXT, artist_name TEXT, album_artist_name TEXT, album_title TEXT, path TEXT)")
                db.executemany("INSERT INTO tracks VALUES (?, '', '', '', '')",
                               [("valid",), ("line\nbreak",), ("segment\x1fbreak",)])
            rows, skipped = sweep.library_rows(path, Path(directory) / "other-data")
            self.assertEqual(rows, [("library", "valid", "")])
            self.assertEqual(skipped, 2)

    def test_prepare_build_switches_base_and_refuses_tracked_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, tree, build = root / "repo", root / "base", root / "head-build"
            repo.mkdir()
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n")
            def git(*args, cwd=repo):
                return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            (repo / "source").write_text("first")
            subprocess.run(["git", "add", "source"], cwd=repo, check=True)
            subprocess.run(["git", "-c", "commit.gpgsign=false", "-c", "user.name=Test", "-c", "user.email=test@example.test", "commit", "-qm", "first"], cwd=repo, check=True)
            first = git("rev-parse", "HEAD")
            (repo / "source").write_text("second")
            subprocess.run(["git", "-c", "commit.gpgsign=false", "-c", "user.name=Test", "-c", "user.email=test@example.test", "commit", "-qam", "second"], cwd=repo, check=True)
            second = git("rev-parse", "HEAD")
            def run(*args, cwd=sweep.ROOT):
                if args[:2] == ("git", "worktree"):
                    subprocess.run(args, cwd=repo, check=True, stdout=subprocess.DEVNULL)
                elif args[:2] == ("git", "checkout"):
                    subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.DEVNULL)
            def output(*args, cwd=sweep.ROOT):
                return subprocess.check_output(args, cwd=cwd if cwd != sweep.ROOT else repo, text=True).strip()
            with patch.object(sweep, "CACHE", root / "cache"), patch.object(sweep, "run", side_effect=run), patch.object(sweep, "output", side_effect=output):
                sweep.prepare_build(first, build, tree)
                (tree / "stray").write_text("untracked")
                (tree / "build-sweep").mkdir()
                sweep.prepare_build(second, build, tree)
                self.assertEqual(git("rev-parse", "HEAD", cwd=tree), second)
                (tree / "source").write_text("edited")
                with self.assertRaisesRegex(ValueError, "local changes"):
                    sweep.prepare_build(second, build, tree)
                with self.assertRaisesRegex(ValueError, "on a branch"):
                    sweep.prepare_build(second, build, repo)

    def test_live_library_refused_before_open(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            live = home / "muzaiten/library.sqlite"
            with patch.object(sqlite3, "connect", side_effect=AssertionError("opened")):
                with self.assertRaisesRegex(ValueError, "live muzaiten"):
                    sweep.library_rows(live, home)
            with patch.dict(os.environ, {"XDG_DATA_HOME": str(home)}), \
                 patch.object(sweep, "prepare_build", side_effect=AssertionError("built")):
                self.assertEqual(sweep.main(["--library", str(live)]), 2)
            with patch.dict(os.environ, {"XDG_DATA_HOME": ""}), \
                 patch.object(Path, "home", return_value=home):
                with self.assertRaisesRegex(ValueError, "live muzaiten"):
                    sweep.checked_library_path(home / ".local/share/muzaiten/library.sqlite")


if __name__ == "__main__":
    unittest.main()
