#!/usr/bin/env python3
"""Exercise Han fold generation and authored-data checks without a network."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(sys.argv.pop(1)).resolve()


def load_tool(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


foldgen = load_tool("foldgen", ROOT / "tools" / "foldgen.py")
checker = load_tool("fold_han_data", ROOT / "tools" / "fold-han-data.py")


class FoldHanDataTests(unittest.TestCase):
    def test_simplification_chains_resolve_to_terminals_and_reject_cycles(self):
        self.assertEqual(
            foldgen.resolve_simplification_chains({"鉅": "钜", "钜": "巨", "鯰": "鲶", "鲶": "鲇"}),
            {"鉅": "巨", "钜": "巨", "鯰": "鲇", "鲶": "鲇"},
        )
        with self.assertRaisesRegex(ValueError, "simplification cycle"):
            foldgen.resolve_simplification_chains({"鉅": "钜", "钜": "鉅"})

    def test_reviewed_suppression_includes_the_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            reviewed_path = pathlib.Path(directory) / "reviewed.tsv"
            reviewed_path.write_text("掺\tmissing\tchān\tcàn\tarchaic reading\n", encoding="utf-8")
            reviewed = checker.read_reviewed(reviewed_path)

        rejected = ("path", "1", "掺", "missing", "chān", "càn")
        distinct = ("path", "1", "掺", "missing", "chān", "shǎn")
        self.assertIn(checker.finding_key(rejected), reviewed)
        self.assertNotIn(checker.finding_key(distinct), reviewed)

    def test_directory_mode_requires_every_tier_but_explicit_files_are_scoped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            empty = root / "empty"
            empty.mkdir()
            partial = root / "partial"
            partial.mkdir()
            for name in ("gb2312.tsv", "big5.tsv", "ksx.tsv"):
                (partial / name).write_text("", encoding="utf-8")

            for authored, missing in (
                (empty, checker.TIER_FILES),
                (partial, ("jis.tsv",)),
                (root / "missing", checker.TIER_FILES),
            ):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    result = checker.main(["check", "--authored", str(authored)])
                self.assertEqual(result, 2)
                for name in missing:
                    self.assertIn(name, stderr.getvalue())

            explicit = pathlib.Path(directory) / "one.tsv"
            explicit.write_text("", encoding="utf-8")
            with mock.patch.object(checker, "check", return_value=0) as check:
                self.assertEqual(checker.main(["check", "--authored", str(authored), str(explicit)]), 0)
            self.assertEqual(check.call_args.args[0], [explicit])

    def test_every_reading_tone_uses_exact_attestation_across_variants(self):
        fields = {name: {} for name in (*checker.READING_FIELDS, *checker.VARIANT_FIELDS)}
        fields["kMandarin"].update({"长": "cháng", "吗": "ma", "哪": "nǎ"})
        fields["kTGHZ2013"].update({"长": "长:zhǎng", "哪": "哪:nǎ"})
        fields["kSimplifiedVariant"]["長"] = "U+957F"

        with tempfile.TemporaryDirectory() as directory:
            authored = pathlib.Path(directory) / "authored.tsv"
            authored.write_text(
                "長\tcháng zhàng\t长\n吗\tma\t\n哪\tna\t\n",
                encoding="utf-8",
            )
            reviewed = pathlib.Path(directory) / "reviewed.tsv"
            reviewed.write_text("", encoding="utf-8")
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                mock.patch.object(checker, "unihan_archive", return_value=object()),
                mock.patch.object(checker, "unihan_fields", return_value=fields),
                mock.patch.object(checker, "tiers", return_value={}),
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
            ):
                result = checker.check([authored], reviewed)

        tone_findings = [line.split("\t") for line in stdout.getvalue().splitlines() if "\ttone\t" in line]
        self.assertEqual(result, 1)
        self.assertEqual({(row[2], row[4], row[5]) for row in tone_findings},
                         {("長", "zhàng", "zhǎng"), ("哪", "na", "nǎ")})
        self.assertNotIn("吗", {row[2] for row in tone_findings})


    def test_archaic_applies_to_readings_after_the_first(self):
        fields = {name: {} for name in (*checker.READING_FIELDS, *checker.VARIANT_FIELDS)}
        fields["kHanyuPinyin"].update({"夲": "10001.010:běn", "房": "10002.020:fáng,páng"})
        fields["kMandarin"].update({"夲": "běn", "房": "fáng"})
        fields["kTGHZ2013"]["房"] = "房:fáng"

        with tempfile.TemporaryDirectory() as directory:
            authored = pathlib.Path(directory) / "authored.tsv"
            authored.write_text("夲\tběn\t本\n房\tfáng páng\t\n", encoding="utf-8")
            reviewed = pathlib.Path(directory) / "reviewed.tsv"
            reviewed.write_text("", encoding="utf-8")
            stdout = io.StringIO()
            with (
                mock.patch.object(checker, "unihan_archive", return_value=object()),
                mock.patch.object(checker, "unihan_fields", return_value=fields),
                mock.patch.object(checker, "tiers", return_value={}),
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                checker.check([authored], reviewed)

        archaic = {(row.split("\t")[2], row.split("\t")[4]) for row in stdout.getvalue().splitlines()
                   if "\tarchaic\t" in row}
        self.assertEqual(archaic, {("房", "páng")})

if __name__ == "__main__":
    unittest.main()
