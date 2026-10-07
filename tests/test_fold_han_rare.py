#!/usr/bin/env python3
"""Offline records, assembly and provenance fixtures for rare Han tooling."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name, path):
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


rare = load("fold_han_rare", ROOT / "tools/fold-han-rare.py")
data = rare.data


def metadata(batch="fixture-1"):
    pin = {"source": "fixture:authored", "revision": "1", "sha256": "1" * 64}
    return {"batch": batch, "model": "fixture-predictor", "effort": "unknown", "date": "2026-01-01",
            "prompt_sha256": "2" * 64, "decomposition": {"id": "synthetic-ids", **pin}, "hint": pin}


def prediction_file(path, rows, batch="fixture-1"):
    text = f"# schema: {rare.PREDICTION_SCHEMA}\n# metadata: {rare.canonical(metadata(batch))}\n"
    text += "# columns: " + "\t".join(rare.PREDICTION_COLUMNS) + "\n"
    for ch, raw, simple in rows:
        normalized = " ".join(rare.nfc(token) for token in raw.split())
        synthetic_ids = "⿰木⿱火水 (invented fixture)"
        decomposition_sha = hashlib.sha256(synthetic_ids.encode("utf-8")).hexdigest()
        text += "\t".join((ch, raw, "g", "木", "synthetic structural guess", decomposition_sha, "mù", normalized, simple)) + "\n"
    path.write_text(text, encoding="utf-8")
    return path


def zip_fixture(path, members):
    with zipfile.ZipFile(path, "w") as archive:
        for name, text in sorted(members.items()):
            archive.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), text.encode("utf-8"))


def corpus(root):
    """Write a miniature complete tgh tier using the shipped archive layouts."""
    root.mkdir(parents=True, exist_ok=True)
    unihan = root / "Unihan.zip"
    properties = root / "Properties.zip"
    mappings = root / "MapingTables.zip"
    opencc = root / "TSCharacters.txt"
    fields = {name: {} for name in rare.FIELDS}
    fields["kTGH"] = {ch: "1" for ch in "𠀀𠀁𠀂"}
    fields["kMandarin"] = {"𠀀": "hē", "𠀁": "lǚ", "𠀂": "mā"}
    fields["kHanyuPinyin"] = {"𠀀": "10001.010:hē,bá", "𠀁": "10001.020:lǚ", "𠀂": "10001.030:mā,má"}
    fields["kXHC1983"] = {"𠀀": "001:hē,bá", "𠀂": "002:mā,má"}
    text = "".join(f"U+{ord(ch):X}\t{field}\t{value}\n" for field, values in fields.items() for ch, value in values.items())
    zip_fixture(unihan, {"Unihan_Readings.txt": text})
    zip_fixture(properties, {"CNS_pinyin_2.txt": "ㄏㄜˊ\tbá\nㄌㄩˋ\tlǜ\n",
                             "CNS_phonetic.txt": "1-1111\tㄏㄜˊ\n1-1112\tㄌㄩˋ\n"})
    zip_fixture(mappings, {"Unicode/CNS2UNICODE_Unicode 2.txt": "1-1111\t20000\n1-1112\t20001\n"})
    opencc.write_text("# empty fixture\n", encoding="utf-8")
    pins = {"schema": rare.REFERENCE_SCHEMA}
    for name, path in zip(("unihan", "cns-properties", "cns-mappings", "opencc"), (unihan, properties, mappings, opencc)):
        pins[name] = {"source": "fixture:" + name, "revision": "1", "sha256": rare.digest(path)}
    manifest = root / "references.json"
    manifest.write_text(rare.canonical(pins) + "\n", encoding="utf-8")
    predictions = prediction_file(root / "tgh-fixture-1.tsv", [("𠀀", "he\u0304 bō", ""), ("𠀁", "lü lu", ""), ("𠀂", "-", "")])
    reviewed = root / "reviewed.tsv"
    reviewed.write_text("", encoding="utf-8")
    authored = root / "authored"
    authored.mkdir(exist_ok=True)
    refs = rare.References(data.unihan_fields(unihan, *rare.FIELDS), data.cns_readings((properties, mappings)), {}, pins)
    args = ["--references", str(manifest), "--unihan", str(unihan), "--cns-properties", str(properties),
            "--cns-mappings", str(mappings), "--opencc", str(opencc), "--authored", str(authored), "--predictions", str(predictions)]
    return refs, rare.read_predictions([predictions]), args, reviewed


class FoldHanRareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.refs, self.predictions, self.args, self.reviewed = corpus(self.root)

    def refs_for(self, unihan, cns=None, opencc=None):
        fields = {name: {} for name in rare.FIELDS}
        fields.update(unihan)
        return rare.References(fields, cns or {}, opencc or {}, self.refs.pins)

    def predict(self, text, batch="guess", ch="𠀀", simple=""):
        path = prediction_file(self.root / (batch + ".tsv"), [(ch, text, simple)], batch)
        return rare.read_predictions([path])

    def test_basis_exact_syllable_agreed_and_single(self):
        rows, records, gaps, findings = rare.assemble(list("𠀀𠀁𠀂"), self.refs, self.predictions, set())
        self.assertEqual(findings, [])
        self.assertEqual(rows, [("𠀀", "bá hē", ""), ("𠀁", "lǚ lǜ", ""), ("𠀂", "mā má", "")])
        by_value = {r.value: r for r in records}
        self.assertEqual((by_value["hē"].basis, by_value["hē"].tone_from), ("predicted", "-"))
        self.assertEqual((by_value["bá"].basis, by_value["bá"].attested_by), ("agreed", "cns,unihan"))
        self.assertEqual((by_value["lǜ"].basis, by_value["lǜ"].tone_from), ("predicted", "cns"))
        self.assertEqual((by_value["lǚ"].basis, by_value["lǚ"].tone_from), ("predicted", "unihan"))
        self.assertEqual(by_value["má"].basis, "single")
        self.assertEqual({row[2] for row in gaps}, {"bō", "lu"})
        self.assertNotIn("bō", by_value)

    def test_two_unihan_fields_count_as_one_lineage(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā"}, "kHanyuPinyin": {"𠀀": "1:mā"}})
        records, _ = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual((records[0].basis, records[0].attested_by), ("single", "unihan"))

    def test_toneless_agreement_does_not_attest_an_exact_tone(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā"}}, {"𠀀": {"má"}})
        records, _ = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual([r.basis for r in records], ["single", "single"])

    def test_syllable_preserves_diaeresis_and_neutral_tones(self):
        self.assertEqual(rare.syllable("lǜ"), "lü")
        self.assertNotEqual(rare.syllable("lǜ"), rare.syllable("lù"))
        self.assertEqual(rare.syllable("ma"), rare.syllable("mǎ"))
        self.assertNotEqual(rare.syllable("ê"), rare.syllable("e"))

    def test_exact_prediction_beats_syllable_match_across_batches(self):
        first = prediction_file(self.root / "a.tsv", [("𠀀", "ma", "")], "a")
        second = prediction_file(self.root / "z.tsv", [("𠀀", "mā", "")], "z")
        predictions = rare.read_predictions([second, first])
        record = rare.classify("𠀀", "mā", {"unihan"}, predictions)
        self.assertEqual((record.batch, record.tone_from), ("z", "-"))
        self.assertEqual(predictions.inputs, rare.read_predictions([first, second]).inputs)

    def test_kmandarin_then_model_order_break_ties(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā má"}, "kHanyuPinyin": {"𠀀": "1:mā,má,mǎ,mà"}})
        records, issues = rare.ordered_readings("𠀀", refs, self.predict("mà mǎ má mā"))
        self.assertEqual([r.value for r in records], ["mā", "má", "mà", "mǎ"])
        self.assertEqual(issues, [])

    def test_predicted_and_single_evidence_order(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā"}, "kHanyuPinyin": {"𠀀": "1:mā,hé"}})
        records, issues = rare.ordered_readings("𠀀", refs, self.predict("hé"))
        self.assertEqual([r.value for r in records], ["hé", "mā"])
        self.assertEqual(issues, [])

    def test_mainland_precedes_predicted_cns_and_checker_enforces_order(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kMandarin": {"𠀀": "mā"}}, {"𠀀": {"hé"}})
        predictions = self.predict("hé")
        records, issues = rare.ordered_readings("𠀀", refs, predictions)
        self.assertEqual([r.value for r in records], ["mā", "hé"])
        self.assertEqual(issues, [])
        out = self.root / "mainland"
        self.assertFalse(rare.write_assembly(out, "tgh", ["𠀀"], refs, predictions, set()))
        table, provenance = out / "tgh.tsv", out / "tgh-provenance.tsv"
        self.assertFalse(rare.check_provenance(table, provenance, refs, predictions, set()))
        table.write_text(table.read_text().replace("mā hé", "hé mā"))
        findings = rare.check_provenance(table, provenance, refs, predictions, set())
        self.assertTrue(any(f[3] == "provenance" for f in findings))

    def test_kmandarin_second_value_is_taiwan_unless_mainland_field_attests_it(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā má"}}, {"𠀀": {"má"}})
        records, issues = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual([r.value for r in records], ["mā", "má"])
        self.assertFalse(issues)
        refs.fields["kHanyuPinyin"] = {"𠀀": "1:má"}
        records, issues = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual([r.value for r in records], ["má", "mā"])
        self.assertFalse(issues)

    def test_cns_only_model_order_and_unpredicted_default_decision(self):
        refs = self.refs_for({}, {"𠀀": {"mā", "hé", "bō"}})
        records, issues = rare.ordered_readings("𠀀", refs, self.predict("hé mā"))
        self.assertEqual([r.value for r in records], ["hé", "mā", "bō"])
        self.assertFalse(issues)
        _, issues = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual(len(issues), 1)

    def test_single_cns_reading_needs_no_choice(self):
        refs = self.refs_for({}, {"𠀀": {"mā"}})
        records, issues = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual([r.value for r in records], ["mā"])
        self.assertFalse(issues)

    def test_evidence_order_applies_within_each_region(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā"}, "kHanyuPinyin": {"𠀀": "1:mā,bá,qǐ"}},
                             {"𠀀": {"bá", "hé", "wū"}})
        records, issues = rare.ordered_readings("𠀀", refs, self.predict("hé qǐ"))
        self.assertEqual([r.value for r in records], ["bá", "qǐ", "mā", "hé", "wū"])
        self.assertFalse(issues)

    def test_mainland_fields_match_region_definition(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā má"}, "kHanyuPinyin": {"𠀀": "1:mǎ"},
                              "kXHC1983": {"𠀀": "1:mà"}, "kTGHZ2013": {"𠀀": "1:bá"},
                              "kHanyuPinlu": {"𠀀": "hé(1)"}})
        self.assertEqual(refs.mainland("𠀀"), {"mā", "mǎ", "mà", "bá"})

    def test_evidence_tie_requires_a_decision(self):
        refs = self.refs_for({"kHanyuPinyin": {"𠀀": "1:mā,má"}})
        records, issues = rare.ordered_readings("𠀀", refs, rare.Predictions({}, []))
        self.assertEqual({r.value for r in records}, {"mā", "má"})
        self.assertIn("evidence-order tie", issues[0][1])

    def test_mapping_family_rule_and_blind_basis(self):
        refs = self.refs_for({"kSimplifiedVariant": {"鵰": "U+2B6F2"}, "kTraditionalVariant": {"𫛲": "U+9D70"},
                              "kZVariant": {"𫛲": "U+96D5"}}, opencc={"鵰": ["雕"]})
        mappings, issues = rare.family_mappings(refs, {"雕"})
        self.assertFalse(issues)
        self.assertEqual(mappings["𫛲"][0], "雕")
        rows, records, _, findings = rare.assemble(["𫛲"], refs, self.predict("-", ch="𫛲", simple="雕"), {"雕"})
        self.assertEqual(rows, [("𫛲", "-", "雕")])
        self.assertEqual((records[0].basis, records[0].rank, records[0].tone_from), ("predicted", 0, "-"))
        self.assertFalse(findings)

    def test_general_standard_target_and_reverse_bmp_mapping(self):
        refs = self.refs_for({"kSimplifiedVariant": {"訏": "U+2C8D9"}, "kTGH": {"𬣙": "1"},
                              "kZVariant": {"𰡻": "U+7459"}})
        mappings, issues = rare.family_mappings(refs, set())
        self.assertEqual((mappings["訏"][0], mappings["𰡻"][0]), ("𬣙", "瑙"))
        self.assertFalse(issues)

    def test_partial_relation_guards_and_ambiguous_targets(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "木": "mā", "禾": "mā"},
                              "kSemanticVariant": {"𠀀": "U+6728"}})
        mappings, issues = rare.family_mappings(refs, {"木"})
        self.assertEqual(mappings["𠀀"][0], "木")
        self.assertFalse(issues)
        refs.fields["kSemanticVariant"]["𠀀"] += " U+79BE"
        mappings, issues = rare.family_mappings(refs, {"木", "禾"})
        self.assertNotIn("𠀀", mappings)
        self.assertIn("tied-lineages", issues["𠀀"][0])
        refs.fields["kSemanticVariant"]["𠀀"] = "U+6728"
        _, issues = rare.family_mappings(refs, {"𠀀", "木"})
        self.assertIn("variant-guard", issues["𠀀"][0])

    def test_partial_target_scores_independent_lineages_not_unihan_fields(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "木": "mā", "禾": "mā"},
                              "kHanyuPinyin": {"木": "1:mā"}, "kXHC1983": {"木": "1:mā"},
                              "kSemanticVariant": {"𠀀": "U+6728 U+79BE"}},
                             {"𠀀": {"mā"}, "禾": {"mā"}})
        mappings, issues = rare.family_mappings(refs, {"木", "禾"})
        self.assertEqual(mappings["𠀀"][0], "禾")
        self.assertFalse(issues)
        refs.cns["木"] = {"mā"}
        mappings, issues = rare.family_mappings(refs, {"木", "禾"})
        self.assertNotIn("𠀀", mappings)
        self.assertEqual(issues["𠀀"], ["variant-guard:tied-lineages=木:2 禾:2"])

    def test_tied_partial_target_choice_remains_blocking_until_reviewed(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "木": "mā", "禾": "mā"},
                              "kSemanticVariant": {"𠀀": "U+6728 U+79BE"}})
        predictions = rare.Predictions({}, [])
        out = self.root / "partial"
        rare.write_assembly(out, "extb", ["𠀀"], refs, predictions, {"木", "禾"})
        table, provenance = out / "extb.tsv", out / "extb-provenance.tsv"
        text = table.read_text().replace("𠀀\tmā\t\n", "𠀀\tmā\t禾\n")
        table.write_text(text)
        with provenance.open("a") as stream:
            stream.write("𠀀\t禾\tsingle\tunihan:kSemanticVariant\t-\t0\t-\n")
        findings = rare.check_provenance(table, provenance, refs, predictions, {"木", "禾"})
        self.assertEqual([(f[3], f[4]) for f in findings], [("variant-guard", "禾")])
        row = "\t".join(data.finding_key(findings[0])) + "\tSynthetic tied-target choice\n"
        argv = ["check", str(table), "--provenance", str(provenance), "--reviewed", str(self.reviewed)]
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=predictions),
              mock.patch.object(rare, "authored_before", return_value={"木", "禾"}),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            self.reviewed.write_text(row)
            self.assertEqual(rare.main(argv), 0)

    def test_compatibility_endpoints_canonicalize_and_others_leave_the_graph(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "huī", "灰": "huī", "\U0002F835": "huī"},
                              "kSemanticVariant": {"𠀀": "U+2F835 U+FA11", "\uFA11": "U+5D0E", "\U0002F835": "U+7070"},
                              "kZVariant": {"\uFA17": "U+76CA"}})
        mappings, issues = rare.family_mappings(refs, {"灰", "崎"})
        self.assertEqual(mappings["𠀀"], ("灰", "unihan:kSemanticVariant"))
        self.assertFalse(issues)
        self.assertFalse({"\uFA11", "\uFA17", "\U0002F835"} & set(mappings))
        self.assertNotIn("\U0002F835", data.tiers(refs.fields, {})["extc"])

    def test_cns_spacing_caron_after_a_vowel_is_the_third_tone(self):
        self.assertEqual(data.cns_pinyin("e\u0302\u02c7"), "ê\u030c")
        for spelling in ("mao\u02c7", "\u02c7", "mǎ"):
            self.assertEqual(data.cns_pinyin(spelling), spelling)
        properties, mappings = self.root / "caron-properties.zip", self.root / "caron-mappings.zip"
        zip_fixture(properties, {"CNS_pinyin_2.txt": "ㄝˇ\tê\u02c7\nㄇㄠˇ\tmao\u02c7\n",
                                 "CNS_phonetic.txt": "1-1111\tㄝˇ\n1-1112\tㄇㄠˇ\n"})
        zip_fixture(mappings, {"Unicode/CNS2UNICODE_Unicode 2.txt": "1-1111\t20000\n1-1112\t20001\n"})
        cns = data.cns_readings((properties, mappings))
        self.assertEqual(cns, {"𠀀": {"ê\u030c"}, "𠀁": {"mao\u02c7"}})
        records, issues = rare.ordered_readings("𠀀", self.refs_for({}, cns), rare.Predictions({}, []))
        self.assertEqual(([r.value for r in records], issues), (["ê\u030c"], []))

    def test_unparseable_reference_spellings_are_findings(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "x5 mā"}}, {"𠀀": {"mao\u02c7", "mā"}})
        rows, records, _, findings = rare.assemble(["𠀀"], refs, rare.Predictions({}, []), set())
        self.assertEqual(rows, [("𠀀", "mā", "")])
        self.assertEqual([(r.value, r.attested_by) for r in records], [("mā", "cns,unihan")])
        self.assertEqual(findings, [("𠀀", "provenance", "mao\u02c7", "unparseable cns reading"),
                                    ("𠀀", "provenance", "x5", "unparseable kMandarin reading")])

    def test_decomposition_pointer_and_hash_exclude_input_text(self):
        path = self.root / "tgh-fixture-1.tsv"
        text = path.read_text(encoding="utf-8")
        synthetic_ids = "⿰木⿱火水 (invented fixture)"
        sha = hashlib.sha256(synthetic_ids.encode("utf-8")).hexdigest()
        self.assertNotIn(synthetic_ids, text)
        self.assertIn('"id":"synthetic-ids"', text)
        self.assertEqual(self.predictions.rows["𠀀"][0].decomposition_sha256, sha)
        for bad in (synthetic_ids, "bad", sha.upper(), ""):
            path.write_text(text.replace(sha, bad))
            with self.assertRaises(ValueError):
                rare.read_predictions([path])
        path.write_text(text.replace(sha, "-"))
        self.assertEqual(rare.read_predictions([path]).rows["𠀀"][0].decomposition_sha256, "-")
        path.write_text(text.replace('"id":"synthetic-ids",', ""))
        with self.assertRaisesRegex(ValueError, "decomposition pointer"):
            rare.read_predictions([path])
        path.write_text(text.replace(rare.PREDICTION_SCHEMA, "fold-han-predictions/1"))
        with self.assertRaisesRegex(ValueError, "prediction schema"):
            rare.read_predictions([path])

    def test_direct_ambiguous_family_does_not_choose_lexically(self):
        refs = self.refs_for({"kSimplifiedVariant": {"𠀀": "U+6728 U+79BE"}})
        mappings, issues = rare.family_mappings(refs, set())
        self.assertFalse(mappings)
        self.assertIn("unresolved", issues["𠀀"][0])

    def test_authored_guard_snapshot_excludes_current_and_later_tiers(self):
        authored = self.root / "authored"
        (authored / "gb2312.tsv").write_text("木\tmù\t\n", encoding="utf-8")
        (authored / "tgh.tsv").write_text("𠀀\tmā\t\n", encoding="utf-8")
        (authored / "extb.tsv").write_text("𠀁\tlǚ\t\n", encoding="utf-8")
        self.assertEqual(rare.authored_before(authored, "tgh"), {"木"})
        self.assertEqual(rare.authored_before(authored, "extb"), {"木", "𠀀"})
        _, table, provenance = self.assemble_fixture()
        findings = rare.check_provenance(table, provenance, self.refs, self.predictions, {"木"})
        self.assertIn("membership checksum", findings[0][5])

    def test_prediction_format_rejects_missing_duplicate_and_invalid_records(self):
        path = self.root / "tgh-fixture-1.tsv"
        original = path.read_text(encoding="utf-8")
        mutations = [original.replace(f"# schema: {rare.PREDICTION_SCHEMA}\n", ""),
                     original + original.splitlines()[-1] + "\n",
                     original.replace("he\u0304 bō\tg", "he\u0304 bō\tbad"),
                     original.replace("he\u0304 bō\tg", "HE bō\tg"),
                     original.replace("hē bō\t\n", "hé bō\t\n"),
                     original.replace('"date":"2026-01-01"', '"date":"2026-13-01"'),
                     original.replace('"revision":"1"', '"revision":"/home/example/source"')]
        for content in mutations:
            with self.subTest(content=content[:40]):
                path.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    rare.read_predictions([path])
        path.write_text(original, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate prediction batch"):
            rare.read_predictions([path, path])

    def test_duplicate_members_and_predictions_outside_tier_fail(self):
        with self.assertRaisesRegex(ValueError, "duplicate tier member"):
            rare.assemble(["𠀀", "𠀀"], self.refs, self.predictions, set())
        with self.assertRaisesRegex(ValueError, "outside tier"):
            rare.assemble(["𠀀"], self.refs, self.predictions, set())

    def assemble_fixture(self):
        out = self.root / "output"
        rare.write_assembly(out, "tgh", data.tiers(self.refs.fields, self.refs.cns)["tgh"], self.refs, self.predictions, set())
        return out, out / "tgh.tsv", out / "tgh-provenance.tsv"

    def test_complete_tier_cli_and_reproducible_bytes(self):
        out = self.root / "output"
        command = [sys.executable, str(ROOT / "tools/fold-han-rare.py"), "assemble", "--tier", "tgh", "--output", str(out), *self.args]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        before = {p.name: p.read_bytes() for p in out.iterdir()}
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(before, {p.name: p.read_bytes() for p in out.iterdir()})
        command = [sys.executable, str(ROOT / "tools/fold-han-rare.py"), "check", "--provenance", str(out / "tgh-provenance.tsv"),
                   "--reviewed", str(self.reviewed), str(out / "tgh.tsv"), *self.args]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        table = (out / "tgh.tsv").read_text(encoding="utf-8")
        self.assertIn("# complete\n", table)
        self.assertIn(self.refs.pins["unihan"]["sha256"], table)
        self.assertNotIn(str(self.root), table)

    def test_provenance_catches_tampering_and_missing_record(self):
        _, table, provenance = self.assemble_fixture()
        original = provenance.read_text(encoding="utf-8")
        mutations = [original.replace("bá\tagreed\tcns,unihan", "bá\tagreed\tunihan"),
                     original.replace("hē\tpredicted", "hē\tsingle"),
                     original.replace("\t2\tfixture-1", "\t1\tfixture-1"),
                     original.replace("\tfixture-1\n", "\tmissing-batch\n"),
                     original.replace("bá\tagreed\tcns,unihan\tcns,unihan\t1\t-\n", ""),
                     original + "𠀀\tbō\tsingle\tunihan\tunihan\t3\t-\n",
                     original.replace(self.refs.pins["unihan"]["sha256"], "9" * 64)]
        for content in mutations:
            provenance.write_text(content, encoding="utf-8")
            self.assertTrue(rare.check_provenance(table, provenance, self.refs, self.predictions, set()))
        provenance.write_text(original, encoding="utf-8")
        self.assertFalse(rare.check_provenance(table, provenance, self.refs, self.predictions, set()))

    def test_missing_sidecar_and_table_order_are_findings(self):
        _, table, provenance = self.assemble_fixture()
        original = table.read_text(encoding="utf-8")
        for content in (original.replace("bá hē", "hē bá"), original.replace("bá hē", "bá hē bō"),
                        original.replace('"predicted":3', '"predicted":99')):
            table.write_text(content, encoding="utf-8")
            self.assertTrue(rare.check_provenance(table, provenance, self.refs, self.predictions, set()))
        provenance.unlink()
        self.assertTrue(rare.check_provenance(table, provenance, self.refs, self.predictions, set()))

    def test_complete_tier_catches_missing_rows_with_matching_sidecar(self):
        _, table, provenance = self.assemble_fixture()
        table.write_text(table.read_text(encoding="utf-8").replace("𠀂\tmā má\t\n", ""), encoding="utf-8")
        provenance.write_text("\n".join(line for line in provenance.read_text(encoding="utf-8").splitlines()
                                         if not line.startswith("𠀂\t")) + "\n", encoding="utf-8")
        findings = rare.check_provenance(table, provenance, self.refs, self.predictions, set())
        self.assertIn(("tgh.tsv", "-", "𠀂", "coverage", "-", "required by tier"), findings)

    def test_provenance_rejects_malformed_rank_duplicate_and_mapping(self):
        _, _, path = self.assemble_fixture()
        original = path.read_text(encoding="utf-8")
        for text in (original.replace("\t1\t-", "\t-1\t-"), original + original.splitlines()[-1] + "\n",
                     original + "𠀀\t𠀀\tsingle\tunihan:kZVariant\t-\t0\t-\n",
                     original.replace("\tcns,unihan\t", "\tunihan,cns\t")):
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(ValueError):
                rare.read_records(path)

    def test_reference_checksum_is_enforced_without_network(self):
        (self.root / "TSCharacters.txt").write_text("corrupted", encoding="utf-8")
        with contextlib.redirect_stderr(output := io.StringIO()):
            result = rare.main(["assemble", "--tier", "tgh", "--output", str(self.root / "output"), *self.args])
        self.assertEqual(result, 2)
        self.assertIn("sha256 mismatch", output.getvalue())

    def test_cns_default_choice_is_blocking_until_exact_reviewed_row(self):
        refs = self.refs_for({}, {"𠀀": {"mā", "hé"}})
        predictions = rare.Predictions({}, [])
        out = self.root / "conflict"
        rare.write_assembly(out, "cns", ["𠀀"], refs, predictions, set())
        table, sidecar = out / "cns.tsv", out / "cns-provenance.tsv"
        # Choose the other attested default and keep the rank audit aligned.
        table.write_text(table.read_text().replace("hé mā", "mā hé"))
        text = sidecar.read_text().replace("hé\tsingle\tcns\tcns\t1", "hé\tsingle\tcns\tcns\t2")
        text = text.replace("mā\tsingle\tcns\tcns\t2", "mā\tsingle\tcns\tcns\t1")
        sidecar.write_text(text)
        argv = ["check", str(table), "--provenance", str(sidecar), "--reviewed", str(self.reviewed), "--authored", str(self.root / "authored")]
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=predictions),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            findings = rare.check_provenance(table, sidecar, refs, predictions, set())
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0][4:6], ("mā hé", "CNS-only default requires a reviewed decision"))
            self.reviewed.write_text("\n".join("\t".join(data.finding_key(f)) + "\tFixture decision" for f in findings) + "\n", encoding="utf-8")
            self.assertEqual(rare.main(argv), 0)
            self.reviewed.write_text(self.reviewed.read_text().replace("mā hé", "hé mā"))
            self.assertEqual(rare.main(argv), 1)

    def test_integrated_checker_requires_provenance_and_uses_evidence_default(self):
        out, table, provenance = self.assemble_fixture()
        sidecars = self.root / "provenance"
        sidecars.mkdir()
        (sidecars / "tgh.tsv").write_bytes(provenance.read_bytes())
        def run():
            with (mock.patch.object(data, "unihan_archive", return_value=self.root / "Unihan.zip"),
                  mock.patch.object(data, "cns_archives", return_value=(self.root / "Properties.zip", self.root / "MapingTables.zip")),
                  mock.patch.object(data, "opencc_archive", return_value=self.root / "TSCharacters.txt"),
                  mock.patch.object(rare, "shipped_pins", return_value=self.refs.pins),
                  mock.patch.object(data, "ROOT", ROOT),
                  contextlib.redirect_stdout(output := io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
                result = data.check([table], self.reviewed, authored_dir=out, provenance_dir=sidecars, predictions_dir=self.root)
            return result, output.getvalue()
        result, output = run()
        self.assertEqual(result, 0, output)
        (sidecars / "tgh.tsv").unlink()
        result, output = run()
        self.assertEqual(result, 1)
        self.assertIn("\tprovenance\t", output)


def pinned_references():
    """Return the cached pinned references without downloading, or None when one is absent or differs."""
    paths = data.reference_cache()
    pins = rare.shipped_pins()
    if not all(path.is_file() and rare.digest(path) == pins[name]["sha256"] for name, path in paths.items()):
        return None
    return rare.References(data.unihan_fields(paths["unihan"], *rare.FIELDS),
                           data.cns_readings((paths["cns-properties"], paths["cns-mappings"])),
                           data.opencc_characters(paths["opencc"]), pins)


class PinnedReferenceSmokeTests(unittest.TestCase):
    """Assemble every tier read-only from the cached pinned references."""

    @classmethod
    def setUpClass(cls):
        cls.refs = pinned_references()
        if cls.refs is None:
            raise unittest.SkipTest("pinned references are not cached")
        cls.tiers = data.tiers(cls.refs.fields, cls.refs.cns)

    def test_every_tier_assembles(self):
        for name in (path.removesuffix(".tsv") for path in data.RARE_FILES):
            with self.subTest(tier=name):
                members = self.tiers[name]
                self.assertTrue(all(data.in_ideograph_blocks(ch) for ch in members))
                authored = rare.authored_before(data.DEFAULT_AUTHORED_DIR, name)
                rows, _, _, findings = rare.assemble(members, self.refs, rare.Predictions({}, []), authored)
                self.assertEqual([row[0] for row in rows], members)
                self.assertLessEqual({f[1] for f in findings}, {"provenance", "variant-guard", "family-target"})
                self.assertFalse([f for f in findings if f[3].startswith("unparseable")])


if __name__ == "__main__":
    unittest.main()
