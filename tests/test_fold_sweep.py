#!/usr/bin/env python3
"""Offline failure checks for fold sweep classification and input safety."""
import importlib.util
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("fold_sweep", Path(__file__).resolve().parents[1] / "tools/fold-sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


class SweepTests(unittest.TestCase):
    def test_expected_reading_loss_controls_exit(self):
        rows = [("cedict", "女人", "nuren")]
        changes, counts = sweep.evaluate(rows, [("女人", "nuren")], [("女人",)])
        self.assertEqual(changes[0][1], "worse")
        self.assertEqual(counts["cedict"]["worse"], 1)
        with patch.object(sweep, "prepare_build", return_value=(Path("base"), Path("head"), {"CMAKE_CXX_COMPILER": "c++"}, 0)), \
             patch.object(sweep, "compile_inspector", side_effect=[Path("base"), Path("head")]), \
             patch.object(sweep, "cache_values", return_value={"CMAKE_CXX_COMPILER": "c++"}), \
             patch.object(sweep, "cedict_rows", return_value=rows), \
             patch.object(sweep, "phrase_rows", return_value=[]), \
             patch.object(sweep, "inspector", side_effect=[[('女人', 'nuren')], [('女人',)]]), \
             patch.object(sweep, "output", return_value="0123456789abcdef"), \
             patch.object(sweep, "report"):
            self.assertEqual(sweep.main([]), 0)
        with patch.object(sweep, "prepare_build", return_value=(Path("base"), Path("head"), {"CMAKE_CXX_COMPILER": "c++"}, 0)), \
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

    def test_live_library_refused_before_open(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            live = home / "muzaiten/library.sqlite"
            with patch.object(sqlite3, "connect", side_effect=AssertionError("opened")):
                with self.assertRaisesRegex(ValueError, "live muzaiten"):
                    sweep.library_rows(live, home)
            with patch.dict(os.environ, {"XDG_DATA_HOME": str(home)}):
                self.assertEqual(sweep.main(["--library", str(live)]), 2)


if __name__ == "__main__":
    unittest.main()
