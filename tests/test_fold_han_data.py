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

    def test_word_loader_skips_character_tables_and_checks_word_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "han.tsv").write_text("好\thǎo\t\n", encoding="utf-8")
            words = root / "words.tsv"
            words.write_text("# word readings\n你\tnǐ\n", encoding="utf-8")

            entries, _ = foldgen.load_authored_han(root)
            self.assertEqual(entries, {"好": ("hao", None, "")})
            self.assertEqual(checker.authored_ideographs(root), {"好"})
            self.assertEqual(foldgen.load_han_words(words, {}), {"你": ("ni", "")})

            words.write_text("# word readings\n行程\txíng chéng\tháng chéng\n", encoding="utf-8")
            self.assertEqual(foldgen.load_han_words(words, {}), {"行程": ("xing cheng", "hang cheng")})

            words.write_text("# word readings\n你好\tnǐ\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, r"words.tsv:2: 2 characters but 1 syllables"):
                foldgen.load_han_words(words, {})

            words.write_text("# word readings\n樂隊\tlè duì\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, r"words.tsv:2: word must use simplified Han forms"):
                foldgen.load_han_words(words, {"樂": "乐"})

            words.write_text("# word readings\n你好\tnǐ hǎo\tnǐ\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, r"words.tsv:2: second reading: 2 characters but 1 syllables"):
                foldgen.load_han_words(words, {})

            words.write_text("# word readings\n你\tnǐ\tnǐ\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, r"words.tsv:2: second reading must differ from the first"):
                foldgen.load_han_words(words, {})

            words.write_text("# word readings\n你好\tnǐ hǎo\tnǐ hào\textra\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, r"words.tsv:2: expected 2 or 3 tab-separated fields"):
                foldgen.load_han_words(words, {})

    def test_second_word_reading_is_checked_against_characters_and_cedict(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            authored = root / "authored"
            authored.mkdir()
            (authored / "han.tsv").write_text(
                "行\txíng háng\t\n程\tchéng\t\n重\tchóng zhòng\t\n庆\tqìng\t\n",
                encoding="utf-8",
            )
            unihan = root / "unihan"
            unihan.mkdir()
            (unihan / "han_pinyin.tsv").write_text("", encoding="utf-8")
            (unihan / "han_simplified.tsv").write_text("", encoding="utf-8")
            words = root / "words.tsv"
            words.write_text(
                "行程\txíng chéng\thóng chéng\n重庆\tchóng qìng\tchóng qìng\n",
                encoding="utf-8",
            )

            with mock.patch.object(checker, "DEFAULT_UNIHAN_DIR", unihan):
                findings, counts = checker.check_words(
                    words,
                    authored,
                    {"行程": {("xing", "cheng")}, "重庆": {("chong", "qing")}},
                    {"xing", "hang", "cheng", "hong", "chong", "zhong", "qing"},
                    set(),
                    set(),
                )

        self.assertEqual(counts["format"], 1)
        self.assertEqual(counts["reading"], 1)
        self.assertEqual(counts["cedict"], 1)
        self.assertIn((str(words), "1", "行程", "reading", "hóng", "xing hang"), findings)
        self.assertIn((str(words), "1", "行程", "cedict", "hóng chéng", "xing cheng"), findings)
        self.assertIn(
            (str(words), "2", "重庆", "format", "chóng qìng", "a distinct second reading"), findings
        )

    def test_cedict_indexes_both_spellings_and_prints_its_date_line(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "cedict.zip"
            with checker.zipfile.ZipFile(archive, "w") as zipped:
                zipped.writestr(
                    "cedict_ts.u8",
                    "#! date=2026-09-26T00:33:45Z\n樂隊 乐队 [yue4 dui4] /band/\n",
                )
            date_line, words = checker.cedict_words(archive)

        self.assertEqual(date_line, "#! date=2026-09-26T00:33:45Z")
        self.assertEqual(words, {"乐队": {("yue", "dui")}, "樂隊": {("yue", "dui")}})

    def test_word_findings_cover_duplicates_readings_cedict_and_uncommon(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            authored = root / "authored"
            authored.mkdir()
            (authored / "han.tsv").write_text(
                "重\tchóng zhòng\t\n庆\tqìng\t\n行\txíng háng\t\n程\tchéng\t\n"
                "罕\thǎn\t\n见\tjiàn\t\n",
                encoding="utf-8",
            )
            unihan = root / "unihan"
            unihan.mkdir()
            (unihan / "han_pinyin.tsv").write_text("", encoding="utf-8")
            (unihan / "han_simplified.tsv").write_text("", encoding="utf-8")
            words = root / "words.tsv"
            words.write_text(
                "重庆\tchóng qìng\n重庆\tchóng qìng\n行程\thóng chéng\n罕见\thǎn jiàn\n",
                encoding="utf-8",
            )
            with mock.patch.object(checker, "DEFAULT_UNIHAN_DIR", unihan):
                findings, counts = checker.check_words(
                    words,
                    authored,
                    {"重庆": {("chong", "qing")}, "行程": {("xing", "cheng")}},
                    {"chong", "zhong", "qing", "xing", "hang", "hong", "cheng", "han", "jian"},
                    set(),
                    set(),
                )
                suppressed_findings, suppressed_counts = checker.check_words(
                    words,
                    authored,
                    {"重庆": {("chong", "qing")}, "行程": {("xing", "cheng")}},
                    {"chong", "zhong", "qing", "xing", "hang", "hong", "cheng", "han", "jian"},
                    set(),
                    {("行程", "reading", "hóng", "xing hang")},
                )

        self.assertEqual(counts["format"], 1)
        self.assertEqual(counts["reading"], 1)
        self.assertEqual(counts["cedict"], 1)
        self.assertEqual(counts["uncommon"], 1)
        self.assertIn((str(words), "3", "行程", "reading", "hóng", "xing hang"), findings)
        self.assertIn((str(words), "3", "行程", "cedict", "hóng chéng", "xing cheng"), findings)

        self.assertEqual(suppressed_counts["reading"], 0)
        self.assertFalse(any(finding[3] == "reading" for finding in suppressed_findings))

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

            words_file = root / "external-words.tsv"
            with mock.patch.object(checker, "check", return_value=0) as check:
                self.assertEqual(
                    checker.main([
                        "check", "--authored", str(partial), "--words", str(words_file), str(explicit),
                    ]),
                    0,
                )
            self.assertEqual(check.call_args.args[2], words_file)
            self.assertEqual(check.call_args.args[3], partial)

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
