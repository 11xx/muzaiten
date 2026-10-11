#!/usr/bin/env python3
"""Offline records, assembly and provenance fixtures for rare Han tooling."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
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
        # Synthetic pins are trusted only inside these in-process provenance fixtures.
        self.registry = copy.deepcopy(rare.notice_terms.read_registry())
        for source in self.registry["sources"].values():
            source["pins"] = {key: self.refs.pins[key] for key in source["pins"]}
        self.registry["sources"]["opencc"]["attribution"]["commit"] = self.refs.pins["opencc"]["revision"]
        self.registry_path = self.root / "source-terms.json"
        self.registry_path.write_text(json.dumps(self.registry), encoding="utf-8")
        self.notice_root = self.root / "source-notices"
        self.notice_root.mkdir()
        self.scopes = {name: list(data.TIER_FILES) for name in self.registry["sources"]}
        self.notice_root.joinpath("NOTICE").write_text(rare.notice_terms.render_notice(self.registry, self.scopes))
        shutil.copytree(data.DEFAULT_AUTHORED_DIR / "licenses", self.notice_root / "licenses")
        shutil.copytree(data.DEFAULT_AUTHORED_DIR / "licenses", self.root / "licenses")
        for patcher in (mock.patch.object(rare.notice_terms, "REGISTRY", self.registry_path),
                        mock.patch.object(data, "DEFAULT_AUTHORED_DIR", self.notice_root)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def refs_for(self, unihan, cns=None, opencc=None):
        fields = {name: {} for name in rare.FIELDS}
        fields.update(unihan)
        return rare.References(fields, cns or {}, opencc or {}, self.refs.pins)

    def predict(self, text, batch="guess", ch="𠀀", simple=""):
        path = prediction_file(self.root / (batch + ".tsv"), [(ch, text, simple)], batch)
        return rare.read_predictions([path])

    def test_frozen_typed_membership_ignores_later_authored_rows(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mù", "木": "mù", "禾": "mù"},
                              "kSemanticVariant": {"𠀀": "U+6728 U+79BE"}})
        refs.typed = frozenset("木")
        first = rare.family_mappings(refs, {"木"})
        self.assertEqual(first, rare.family_mappings(refs, {"木", "禾", "𠀀"}))
        self.assertEqual(first[0]["𠀀"][0], "木")

    def test_typed_snapshot_has_exact_ratified_hash(self):
        refs = rare.References({"kTGH": {}}, {}, {}, rare.shipped_pins())
        typed = rare.typed_members(refs, set())
        self.assertEqual(len(typed), 11799)
        self.assertEqual(rare.authored_pin(set(typed))["sha256"],
                         "843a97bc8bcab86a9295a29074cd074863fe877d0f7d4648931d49a9e7674fa7")
        with mock.patch.object(rare, "TYPED_SHA256", "0" * 64):
            with self.assertRaisesRegex(ValueError, "typed membership"):
                rare.typed_members(refs, set())

    def test_ratified_mapping_choices_reach_assembly(self):
        refs = self.refs_for({"kSimplifiedVariant": {"𧭈": "U+32087", "𨰥": "U+32210"}},
                             opencc={"𧭈": ["𧭈", "𫍾"], "𨰥": ["𨰥", "𫔕"]})
        refs.retained_mappings = {"𧭈": "𫍾", "𨰥": "𫔕"}
        rows, _, _, issues = rare.assemble(["𧭈", "𨰥"], refs, rare.Predictions({}, []), set())
        self.assertEqual([r[2] for r in rows], ["𫍾", "𫔕"])
        self.assertTrue(all(f[1] == "family-target" for f in issues))

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
        mappings, issues, _ = rare.family_mappings(refs, {"雕"})
        self.assertFalse(issues)
        self.assertEqual(mappings["𫛲"][0], "雕")
        rows, records, _, findings = rare.assemble(["𫛲"], refs, self.predict("-", ch="𫛲", simple="雕"), {"雕"})
        self.assertEqual(rows, [("𫛲", "-", "雕")])
        self.assertEqual((records[0].basis, records[0].rank, records[0].tone_from), ("predicted", 0, "-"))
        self.assertFalse(findings)

    def test_general_standard_target_and_reverse_bmp_mapping(self):
        refs = self.refs_for({"kSimplifiedVariant": {"訏": "U+2C8D9"}, "kTGH": {"𬣙": "1"},
                              "kZVariant": {"𰡻": "U+7459"}})
        mappings, issues, _ = rare.family_mappings(refs, set())
        self.assertEqual((mappings["訏"][0], mappings["𰡻"][0]), ("𬣙", "瑙"))
        self.assertFalse(issues)

    def test_partial_relation_guards_and_ambiguous_targets(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "木": "mā", "禾": "mā"},
                              "kSemanticVariant": {"𠀀": "U+6728"}})
        mappings, issues, rejected = rare.family_mappings(refs, {"木"})
        self.assertEqual(mappings["𠀀"][0], "木")
        self.assertFalse(issues or rejected)
        refs.fields["kSemanticVariant"]["𠀀"] += " U+79BE"
        mappings, issues, _ = rare.family_mappings(refs, {"木", "禾"})
        self.assertNotIn("𠀀", mappings)
        self.assertIn("tied-lineages", issues["𠀀"][0])
        refs.fields["kSemanticVariant"]["𠀀"] = "U+6728"
        mappings, issues, rejected = rare.family_mappings(refs, {"𠀀", "木"})
        self.assertFalse(mappings or issues)
        self.assertEqual(rejected, {"𠀀": [("木", "kSemanticVariant")]})

    def test_guard_failed_links_are_gaps_not_findings(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kMandarin": {"𠀀": "mā", "木": "mā"},
                              "kSemanticVariant": {"𠀀": "U+6728"}})
        rows, _, gaps, findings = rare.assemble(["𠀀"], refs, rare.Predictions({}, []), {"木"})
        self.assertEqual((rows, findings), ([("𠀀", "mā", "")], []))
        self.assertEqual(gaps, [("𠀀", "木", "木", "-", "guard-failed:kSemanticVariant")])

    def test_tied_candidates_of_one_family_need_no_choice(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "suì", "岁": "suì", "歲": "suì"},
                              "kSimplifiedVariant": {"歲": "U+5C81"}, "kSemanticVariant": {"𠀀": "U+5C81 U+6B72"}})
        mappings, issues, _ = rare.family_mappings(refs, {"岁", "歲"})
        self.assertFalse(issues)
        self.assertEqual(mappings["𠀀"][0], "岁")

    def test_tied_choice_names_the_family_target_of_one_candidate(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "lǔ", "櫓": "lǔ", "禾": "lǔ"},
                              "kSimplifiedVariant": {"櫓": "U+6A79"}, "kSemanticVariant": {"𠀀": "U+6AD3 U+79BE"}})
        mappings, issues, _ = rare.family_mappings(refs, {"櫓", "禾"})
        self.assertNotIn("𠀀", mappings)
        self.assertEqual(issues["𠀀"], ["variant-guard:tied-lineages=櫓→橹:1 禾:1"])
        for choice in ("櫓", "木"):
            self.assertNotIn("𠀀", rare.family_mappings(refs, {"櫓", "禾"}, {"𠀀": choice})[0])
        mappings, issues, _ = rare.family_mappings(refs, {"櫓", "禾"}, {"𠀀": "橹"})
        self.assertEqual(mappings["𠀀"], ("橹", "unihan:kSemanticVariant,unihan:kSimplifiedVariant"))
        self.assertEqual(issues["𠀀"], ["variant-guard:tied-lineages=櫓→橹:1 禾:1"])
        out = self.root / "family-choice"
        rare.write_assembly(out, "extb", ["𠀀"], refs, rare.Predictions({}, []), {"櫓", "禾"})
        table, provenance = out / "extb.tsv", out / "extb-provenance.tsv"
        table.write_text(table.read_text().replace("𠀀\tlǔ\t\n", "𠀀\tlǔ\t橹\n"))
        with provenance.open("a") as stream:
            stream.write("𠀀\t橹\tsingle\tunihan:kSemanticVariant,unihan:kSimplifiedVariant\t-\t0\t-\n")
        findings = rare.check_provenance(table, provenance, refs, rare.Predictions({}, []), {"櫓", "禾"})
        self.assertEqual([(f[3], f[4]) for f in findings], [("variant-guard", "橹")])
        argv = ["check", str(table), "--provenance", str(provenance), "--reviewed", str(self.reviewed)]
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=rare.Predictions({}, [])),
              mock.patch.object(rare, "authored_before", return_value={"櫓", "禾"}),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            self.reviewed.write_text("\t".join(data.finding_key(findings[0])) + "\tSynthetic family-target choice\n")
            self.assertEqual(rare.main(argv), 0)
            self.reviewed.write_text(self.reviewed.read_text().replace("橹", "櫓", 1))
            self.assertEqual(rare.main(argv), 1)

    def test_family_without_a_rule_target_uses_its_unique_simplified_member(self):
        refs = self.refs_for({"kSimplifiedVariant": {"𠿕": "U+2A70E", "劔": "U+2A6A5"}, "kZVariant": {"劎": "U+5294"}})
        mappings, issues, _ = rare.family_mappings(refs, set())
        self.assertEqual({ch: target for ch, (target, _) in mappings.items()}, {"𠿕": "𪜎"})
        self.assertEqual(issues, {ch: ["family-target:unresolved=劎 劔"] for ch in "劎劔𪚥"})
        refs.fields["kSimplifiedVariant"]["劎"] = "U+2A6A6"
        mappings, issues, _ = rare.family_mappings(refs, set())
        self.assertEqual(set(issues), {"劎", "劔", "𪚥", "𪚦"})
        self.assertNotIn("劎", mappings)

    def test_two_general_standard_members_cannot_fall_back_to_a_named_nonstandard_member(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1", "𠀁": "1"},
                              "kSimplifiedVariant": {"𠀀": "U+20002"}, "kZVariant": {"𠀁": "U+20000"}})
        for choices in (None, {"𠀀": "𠀂"}):
            with self.subTest(choices=choices):
                mappings, issues, _ = rare.family_mappings(refs, set(), choices)
                self.assertFalse(mappings)
                self.assertEqual(issues, {ch: ["family-target:unresolved=𠀀 𠀁"] for ch in "𠀀𠀁𠀂"})

    def test_opencc_only_all_astral_family_uses_its_named_simplified_member(self):
        refs = self.refs_for({}, opencc={"𠀀": ["𠀁"]})
        mappings, issues, _ = rare.family_mappings(refs, set())
        self.assertEqual(mappings, {"𠀀": ("𠀁", "opencc:TSCharacters")})
        self.assertFalse(issues)
        _, records, _, findings = rare.assemble(["𠀀", "𠀁"], refs, rare.Predictions({}, []), set())
        self.assertEqual([r.columns() for r in records],
                         [["𠀀", "𠀁", "single", "opencc:TSCharacters", "-", "0", "-"]])
        self.assertFalse(findings)

    def test_unresolved_family_choice_requires_one_exact_finding_per_member(self):
        refs = self.refs_for({"kSimplifiedVariant": {"劔": "U+2A6A5"}, "kZVariant": {"劎": "U+5294"}})
        members = list("劎劔𪚥")
        predictions = rare.Predictions({}, [])
        out = self.root / "family-choice"
        rare.write_assembly(out, "extb", members, refs, predictions, set())
        table, sidecar = out / "extb.tsv", out / "extb-provenance.tsv"
        table_header = "\n".join(line for line in table.read_text().splitlines()
                                 if line.startswith("#") and line != "# complete") + "\n"
        provenance_header = "\n".join(line for line in sidecar.read_text().splitlines()
                                      if line.startswith("#")) + "\n"
        def select(target):
            choices = {ch: target for ch in members if ch != target}
            rows, records, _, _ = rare.assemble(members, refs, predictions, set(), choices)
            table.write_text(table_header + "".join("\t".join(row) + "\n" for row in rows))
            sidecar.write_text(provenance_header + "".join("\t".join(r.columns()) + "\n" for r in records))
        select("劎")
        findings = rare.check_provenance(table, sidecar, refs, predictions, set())
        self.assertEqual([f[2:] for f in findings],
                         [(ch, "family-target", "-" if ch == "劎" else "劎", "family-target:unresolved=劎 劔")
                          for ch in members])
        _, records = rare.read_records(sidecar)
        self.assertEqual({r.ch: r.attested_by for r in records},
                         {"劔": "unihan:kZVariant", "𪚥": "unihan:kSimplifiedVariant,unihan:kZVariant"})
        argv = ["check", str(table), "--provenance", str(sidecar), "--reviewed", str(self.reviewed)]
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=predictions),
              mock.patch.object(rare, "authored_before", return_value=set()),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            self.reviewed.write_text("".join("\t".join(data.finding_key(f)) + "\tSynthetic family choice\n" for f in findings))
            self.assertEqual(rare.main(argv), 0)
            select("劔")
            self.assertEqual(rare.main(argv), 1)
        select("劎")
        sidecar.write_text(provenance_header)
        self.assertTrue([f for f in rare.check_provenance(table, sidecar, refs, predictions, set())
                         if f[3] == "provenance"])

    def test_unresolved_family_choices_cannot_disagree_or_override_earlier_candidates(self):
        refs = self.refs_for({"kSimplifiedVariant": {"劔": "U+2A6A5"}, "kZVariant": {"劎": "U+5294"}})
        for choices in ({"劎": "𪚥", "劔": "𪚥"}, {"劔": "劎", "𪚥": "劔"}, {"劔": "木"}):
            with self.subTest(choices=choices):
                mappings, issues, _ = rare.family_mappings(refs, set(), choices)
                self.assertFalse(mappings)
                self.assertEqual(set(issues), set("劎劔𪚥"))
        mappings, issues, _ = rare.family_mappings(refs, set(), {"劔": "劎"})
        self.assertEqual({ch: target for ch, (target, _) in mappings.items()}, {"劔": "劎", "𪚥": "劎"})
        self.assertEqual(set(issues), set("劎劔𪚥"))

    def test_orphan_family_reviewed_choice_lists_all_members_without_a_preference(self):
        refs = self.refs_for({"kZVariant": {"𠀀": "U+20001"}})
        mappings, issues, _ = rare.family_mappings(refs, set(), {"𠀀": "𠀁"})
        self.assertEqual(mappings, {"𠀀": ("𠀁", "unihan:kZVariant")})
        self.assertEqual(issues, {ch: ["family-target:unresolved=𠀀 𠀁"] for ch in "𠀀𠀁"})

    def test_reviewed_choice_for_an_unresolved_direct_family_can_select_a_partial_tie(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "劔": "mā", "禾": "mā"},
                              "kSimplifiedVariant": {"劔": "U+2A6A5"}, "kZVariant": {"劎": "U+5294"},
                              "kSemanticVariant": {"𠀀": "U+5294 U+79BE"}})
        mappings, issues, _ = rare.family_mappings(refs, {"劔", "禾"}, {"𠀀": "劎", "劔": "劎"})
        self.assertEqual(mappings["𠀀"][0], "劎")
        self.assertIn("variant-guard:tied-lineages=劔→劎:1 禾:1", issues["𠀀"])
        self.assertIn("family-target:unresolved=劎 劔", issues["𠀀"])

    def test_tgh_decisive_reading_precedes_general_evidence_without_dropping_alternatives(self):
        examples = [("㴔", "xī", "jí"), ("䐃", "zhūn", "jùn"), ("倴", "bēn", "bèn")]
        for ch, agreed, decisive in examples:
            with self.subTest(ideograph=ch):
                refs = self.refs_for({"kMandarin": {ch: agreed}, "kHanyuPinyin": {ch: "1:" + agreed},
                                      "kTGHZ2013": {ch: "1:" + decisive}}, {ch: {agreed}})
                records, _ = rare.ordered_readings(ch, refs, rare.Predictions({}, []))
                self.assertEqual([r.value for r in records], [agreed, decisive])
                records, issues = rare.ordered_readings(ch, refs, rare.Predictions({}, []), tier="tgh")
                self.assertEqual([r.value for r in records], [decisive, agreed])
                self.assertEqual([(r.basis, r.attested_by) for r in records], [("single", "unihan"), ("agreed", "cns,unihan")])
                self.assertFalse(issues)

    def test_tgh_priority_is_tier_specific_and_uses_the_first_valid_reference_reading(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā"}, "kTGHZ2013": {"𠀀": "1:hé,bá"}}, {"𠀀": {"mā"}})
        predictions = rare.Predictions({}, [])
        records, issues = rare.ordered_readings("𠀀", refs, predictions, ["mā", "hé", "bá"], tier="tgh")
        self.assertEqual(([r.value for r in records], issues), (["hé", "mā", "bá"], []))
        records, _ = rare.ordered_readings("𠀀", refs, predictions, tier="extb")
        self.assertEqual([r.value for r in records], ["mā", "bá", "hé"])
        refs.fields["kTGHZ2013"]["𠀀"] = "1:x5,hé"
        records, issues = rare.ordered_readings("𠀀", refs, predictions, tier="tgh")
        self.assertEqual([r.value for r in records], ["hé", "mā"])
        self.assertEqual(issues, [("x5", "unparseable kTGHZ2013 reading")])
        refs.fields["kTGHZ2013"].clear()
        records, _ = rare.ordered_readings("𠀀", refs, predictions, tier="tgh")
        self.assertEqual([r.value for r in records], ["mā"])

    def test_tgh_assembly_and_audit_enforce_decisive_default_without_suppressions(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kMandarin": {"𠀀": "mā"},
                              "kTGHZ2013": {"𠀀": "1:hé"}}, {"𠀀": {"mā"}})
        predictions = rare.Predictions({}, [])
        out = self.root / "tgh-priority"
        self.assertFalse(rare.write_assembly(out, "tgh", ["𠀀"], refs, predictions, set()))
        table, sidecar = out / "tgh.tsv", out / "tgh-provenance.tsv"
        self.assertIn("𠀀\thé mā\t", table.read_text())
        self.assertFalse(rare.check_provenance(table, sidecar, refs, predictions, set()))
        table.write_text(table.read_text().replace("hé mā", "mā hé"))
        findings = rare.check_provenance(table, sidecar, refs, predictions, set())
        self.assertTrue(any(f[4:] == ("mā hé / ", "hé mā / ") for f in findings))

    def test_partial_target_scores_independent_lineages_not_unihan_fields(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "木": "mā", "禾": "mā"},
                              "kHanyuPinyin": {"木": "1:mā"}, "kXHC1983": {"木": "1:mā"},
                              "kSemanticVariant": {"𠀀": "U+6728 U+79BE"}},
                             {"𠀀": {"mā"}, "禾": {"mā"}})
        mappings, issues, _ = rare.family_mappings(refs, {"木", "禾"})
        self.assertEqual(mappings["𠀀"][0], "禾")
        self.assertFalse(issues)
        refs.cns["木"] = {"mā"}
        mappings, issues, _ = rare.family_mappings(refs, {"木", "禾"})
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
        mappings, issues, _ = rare.family_mappings(refs, {"灰", "崎"})
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
        self.assertEqual(cns, {"𠀀": ["ê\u030c"], "𠀁": []})
        self.assertEqual(cns.held["𠀁"][0]["reading"], "mao\u02c7")
        records, issues = rare.ordered_readings("𠀀", self.refs_for({}, cns), rare.Predictions({}, []))
        self.assertEqual(([r.value for r in records], issues), (["ê\u030c"], []))

    def test_unparseable_reference_spellings_are_findings(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "x5 mā"}}, {"𠀀": {"mao\u02c7", "mā"}})
        rows, records, _, findings = rare.assemble(["𠀀"], refs, rare.Predictions({}, []), set())
        self.assertEqual(rows, [("𠀀", "mā", "")])
        self.assertEqual([(r.value, r.attested_by) for r in records], [("mā", "cns,unihan")])
        self.assertEqual(findings, [("𠀀", "provenance", "mao\u02c7", "unparseable cns reading"),
                                    ("𠀀", "provenance", "x5", "unparseable kMandarin reading")])
        predictions = rare.Predictions({}, [])
        out = self.root / "malformed-reference"
        rare.write_assembly(out, "extb", ["𠀀"], refs, predictions, set())
        table, sidecar = out / "extb.tsv", out / "extb-provenance.tsv"
        audited = rare.check_provenance(table, sidecar, refs, predictions, set())
        self.assertEqual([f[2:] for f in audited], findings)
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=predictions),
              mock.patch.object(rare, "authored_before", return_value=set()),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(["check", str(table), "--provenance", str(sidecar),
                                        "--reviewed", str(self.reviewed)]), 1)

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
        mappings, issues, _ = rare.family_mappings(refs, set())
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
        rare.write_assembly(out, "tgh", data.tiers(self.refs.fields, self.refs.cns)["tgh"], self.refs, self.predictions, set(), complete=True)
        return out, out / "tgh.tsv", out / "tgh-provenance.tsv"

    def test_partial_cli_writes_only_selected_members(self):
        selected = self.root / "members.txt"
        selected.write_text("𠀁\n")
        out = self.root / "partial-output"
        with contextlib.redirect_stderr(io.StringIO()):
            result = rare.main(["assemble", "--tier", "tgh", "--members", str(selected),
                                "--output", str(out), *self.args[:-2]])
        self.assertEqual(result, 0)
        table = out / "tgh.tsv"
        headers, rows = rare.document(table)
        self.assertNotIn("complete", headers)
        self.assertIn("# partial\n", table.read_text())
        self.assertEqual([columns[0] for _, columns in rows], ["𠀁"])
        _, records = rare.read_records(out / "tgh-provenance.tsv")
        self.assertEqual({r.ch for r in records}, {"𠀁"})
        selected.write_text("木\n")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rare.main(["assemble", "--tier", "tgh", "--members", str(selected),
                                        "--output", str(out), *self.args[:-2]]), 2)

    def test_cli_decisions_preserve_reviewed_mapping_and_reading_choices(self):
        refs = self.refs_for({"kTraditionalVariant": {"𠀀": "U+20001"}},
                             {"𠀀": {"mā", "má"}}, {"𠀀": ["𠀀", "𠀁", "𠀂"]})
        decisions = self.root / "decisions.tsv"
        decisions.write_text("𠀀\tfamily-target\t𠀁\tfamily-target:unresolved=𠀀 𠀁 𠀂\tRetain the witnessed pair.\n"
                             "𠀀\tprovenance\tmá mā\tCNS-only default requires a reviewed decision\tChoose the attested default.\n"
                             "木\tfamily-target\t禾\tfamily-target:unresolved=木 禾\tAnother tier.\n")
        selected = self.root / "members.txt"
        selected.write_text("𠀀\n")
        out = self.root / "decided"
        with mock.patch.object(rare, "read_references", return_value=refs), contextlib.redirect_stderr(io.StringIO()):
            status = rare.main(["assemble", "--tier", "extb", "--members", str(selected),
                                "--decisions", str(decisions), "--authored", str(self.root / "authored"), "--output", str(out)])
        self.assertEqual(status, 1)
        self.assertEqual(rare.document(out / "extb.tsv")[1][0][1], ["𠀀", "má mā", "𠀁"])
        self.assertEqual(len(rare.document(out / "extb-findings.tsv")[1]), 2)
        _, records = rare.read_records(out / "extb-provenance.tsv")
        self.assertEqual([(r.value, r.rank) for r in records], [("má", 1), ("mā", 2), ("𠀁", 0)])

    def test_decisions_reject_stale_conflicting_and_invalid_choices_before_writing(self):
        refs = self.refs_for({}, {"𠀀": {"mā", "má"}})
        selected = self.root / "members.txt"
        selected.write_text("𠀀\n")
        decisions = self.root / "decisions.tsv"
        base = "𠀀\tprovenance\tmá mā\tCNS-only default requires a reviewed decision\tChoose a default.\n"
        for text in (base.replace("decision", "choice"), base.replace("má mā", "bō mā"),
                     base + base.replace("má mā", "mā má"), base.rsplit("\t", 1)[0] + "\t\n", "𠀀\tprovenance\tmā\n"):
            with self.subTest(text=text):
                decisions.write_text(text)
                out = self.root / "invalid-choice"
                with mock.patch.object(rare, "read_references", return_value=refs), contextlib.redirect_stderr(io.StringIO()):
                    status = rare.main(["assemble", "--tier", "extb", "--members", str(selected),
                                        "--decisions", str(decisions), "--authored", str(self.root / "authored"), "--output", str(out)])
                self.assertEqual(status, 2)
                self.assertFalse(out.exists())

    def test_expected_changes_compare_search_defaults_readings_and_terminal_keys(self):
        baseline = {"木": (["mu"], "禾"), "禾": (["he"], ""), "火": (["huo"], ""),
                    "水": (["shui"], "禾"), "𠀀": (["yi"], "禾")}
        proposed = {"木": (["ma", "mu"], "禾"), "禾": (["he"], ""), "火": (["huo", "he"], "禾"),
                    "水": (["shui"], ""), "𠀀": (["yi"], "火")}
        changes = rare.expected_changes(baseline, proposed, sorted(proposed))
        self.assertIn(("木", "default-changed", "mu", "ma"), changes)
        self.assertIn(("木", "reading-added", "-", "ma"), changes)
        self.assertIn(("火", "mapping-added", "火", "禾"), changes)
        self.assertIn(("水", "mapping-removed", "禾", "水"), changes)
        self.assertFalse([r for r in changes if r[0] == "𠀀"])
        proposed["𠀀"] = (["yi"], "水")
        self.assertIn(("𠀀", "mapping-retargeted", "禾", "水"), rare.expected_changes(baseline, proposed, ["𠀀"]))

    def test_report_baseline_authored_empty_mapping_overrides_generated_data(self):
        generated = self.root / "generated"
        generated.mkdir()
        (generated / "han_pinyin.tsv").write_text("mu\t木\nhe\t禾\n")
        (generated / "han_simplified.tsv").write_text("禾\t木\n")
        authored = self.root / "authored"
        (authored / "tgh.tsv").write_text("木\tmá mù\t\n")
        state = rare.table_state(authored, generated)
        self.assertEqual(state["木"], (["ma", "mu"], ""))
        self.assertEqual(state["禾"], (["he"], ""))

    def test_pull_forward_reaches_a_fixpoint_for_cycles_and_joined_pairs(self):
        baseline = {"木": (["mu"], "禾"), "禾": (["he"], ""), "𠀀": (["yi"], "木"),
                    "火": (["huo"], "水"), "水": (["shui"], ""), "𠀁": (["er"], "火")}
        proposals = {"tgh": {"木": (["mu"], "𠀀"), "火": (["huo"], "")},
                     "extb": {"𠀀": (["yī"], "禾"), "𠀁": (["èr"], "火"), "水": (["shuǐ"], "火")}}
        report, pulls = rare.pull_forward(baseline, proposals)
        self.assertEqual({r[1] for r in pulls["tgh"]}, {"𠀀", "水"})
        self.assertIn("cycle", next(r[-1] for r in pulls["tgh"] if r[1] == "𠀀"))
        self.assertEqual(next(r[2] for r in pulls["tgh"] if r[1] == "𠀀"), "yī")
        self.assertIn("baseline-pair", next(r[-1] for r in pulls["tgh"] if r[1] == "水"))
        self.assertFalse(pulls["extb"])
        for step in report["steps"]:
            self.assertEqual(step["remaining_splits"], [])
            self.assertEqual(step["cycles"], [])
        self.assertEqual(report["permanent_splits"], [])

    def test_pull_forward_reports_permanent_losses_and_unfixable_cycles(self):
        baseline = {"木": ([], "禾"), "禾": ([], "")}
        proposals = {"tgh": {"木": ([], "")}}
        report, pulls = rare.pull_forward(baseline, proposals)
        self.assertEqual(report["permanent_splits"], [["木", "禾"]])
        self.assertEqual(report["steps"][0]["remaining_splits"], [["木", "禾"]])
        self.assertFalse(pulls["tgh"])
        report, _ = rare.pull_forward(baseline, {"tgh": {"禾": ([], "木")}})
        self.assertEqual(report["final_cycles"], ["木", "禾"])

    def test_pull_forward_repairs_later_intermediates_outside_the_baseline_pair(self):
        baseline = {"木": ([], "禾"), "禾": ([], ""), "𠀀": ([], "火"), "火": ([], ""),
                    "𠀁": ([], "水"), "水": ([], "")}
        proposals = {"tgh": {"木": ([], "𠀀")}, "extb": {"𠀀": ([], "𠀁"), "𠀁": ([], "禾")}}
        report, pulls = rare.pull_forward(baseline, proposals)
        self.assertEqual({r[1] for r in pulls["tgh"]}, {"𠀀", "𠀁"})
        self.assertFalse(report["steps"][0]["unexpected_splits"])

    def test_cli_emits_current_table_impact_and_explicit_unevaluated_closure(self):
        out = self.root / "impact-cli"
        generated = self.root / "generated"
        generated.mkdir()
        (generated / "han_pinyin.tsv").write_text("he\t𠀀\n")
        (generated / "han_simplified.tsv").write_text("")
        with contextlib.redirect_stderr(io.StringIO()):
            status = rare.main(["assemble", "--tier", "tgh", "--generated", str(generated),
                                "--output", str(out), *self.args])
        self.assertEqual(status, 0)
        changes = [r for _, r in rare.document(out / "tgh-expected-changes.tsv")[1]]
        self.assertIn(["𠀀", "default-changed", "he", "ba"], changes)
        self.assertIn(["𠀀", "reading-added", "-", "ba"], changes)
        report = json.loads((out / "tgh-impact.json").read_text())
        self.assertEqual(report["status"], "not-evaluated")
        self.assertEqual(len(report["baseline_sha256"]), 64)
        self.assertEqual(rare.document(out / "tgh-pull-forward.tsv")[1], [])

    def test_cli_closure_reads_all_home_files_and_preserves_toned_pull_rows(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kMandarin": {"𠀀": "yī", "𠀁": "èr"},
                              "kTraditionalVariant": {"𠀀": "U+20001"}})
        horizon = self.root / "horizon"
        horizon.mkdir()
        tiers, _ = rare.build_tiers(refs)
        for name in data.RARE_FILES:
            tier = pathlib.Path(name).stem
            (horizon / name).write_text("".join(f"{ch}\t{'yī' if ch == '𠀀' else 'èr'}\t\n" for ch in tiers[tier]))
        (horizon / "extb.tsv").write_text("𠀁\tèr\t𠀀\n")
        generated = self.root / "generated"
        generated.mkdir()
        (generated / "han_pinyin.tsv").write_text("yi\t𠀀\ner\t𠀁\n")
        (generated / "han_simplified.tsv").write_text("𠀁\t𠀀\n")
        out = self.root / "closure-cli"
        with mock.patch.object(rare, "read_references", return_value=refs), contextlib.redirect_stderr(io.StringIO()):
            status = rare.main(["assemble", "--tier", "tgh", "--generated", str(generated), "--assembly", str(horizon),
                                "--authored", str(self.root / "authored"), "--output", str(out)])
        self.assertEqual(status, 0)
        pulled = [r for _, r in rare.document(out / "tgh-pull-forward.tsv")[1]]
        self.assertEqual(pulled, [["extb", "𠀁", "èr", "𠀀", "baseline-pair"]])
        report = json.loads((out / "tgh-impact.json").read_text())
        self.assertEqual(report["status"], "evaluated")
        self.assertFalse(report["steps"][0]["unexpected_splits"])
        (horizon / "extb.tsv").write_text("")
        with self.assertRaisesRegex(ValueError, "every buildable tier member"):
            rare.read_assembly(horizon, refs)

    def test_complete_writer_refuses_a_slice(self):
        out = self.root / "incomplete"
        with self.assertRaisesRegex(ValueError, "every tier member"):
            rare.write_assembly(out, "tgh", ["𠀀"], self.refs, rare.Predictions({}, []), set(), complete=True)
        self.assertFalse(out.exists())

    def test_reference_graph_keeps_unassigned_extension_j_vertices(self):
        vertex = chr(0x331F0)
        refs = self.refs_for({"kZVariant": {"𠀀": "U+331F0", vertex: "U+6728"}})
        self.assertEqual(rare.endpoint(vertex), vertex)
        self.assertFalse(data.in_ideograph_blocks(vertex))
        self.assertFalse(data.is_unihan_ideograph(vertex))
        mappings, issues, _ = rare.family_mappings(refs, {"木"})
        self.assertEqual(mappings["𠀀"], ("木", "unihan:kZVariant"))
        self.assertEqual(issues, {})
        with self.assertRaisesRegex(ValueError, "CJK ideograph"):
            rare.assemble([vertex], refs, rare.Predictions({}, []), set())
        self.assertNotIn(vertex, data.tiers(refs.fields, {vertex: {"mù"}})["cns"])

    def test_unsupported_targets_are_withheld_without_changing_the_family(self):
        for target in (chr(0x331F0), chr(0x2B73E)):
            with self.subTest(target=target):
                refs = self.refs_for({"kSimplifiedVariant": {"𠀀": f"U+{ord(target):X}"},
                                      "kMandarin": {"𠀀": "mù"}})
                mappings, _, _ = rare.family_mappings(refs, set())
                self.assertEqual(mappings["𠀀"][0], target)
                rows, records, gaps, findings = rare.assemble(["𠀀"], refs, rare.Predictions({}, []), set())
                self.assertEqual(rows, [("𠀀", "mù", "")])
                self.assertFalse([r for r in records if not r.rank])
                self.assertIn(("𠀀", target, target, "-", "unsupported-target:unihan:kSimplifiedVariant"), gaps)
                self.assertFalse(findings)
                out = self.root / "withheld-target"
                rare.write_assembly(out, "extb", ["𠀀"], refs, rare.Predictions({}, []), set())
                self.assertFalse(rare.check_provenance(out / "extb.tsv", out / "extb-provenance.tsv",
                                                       refs, rare.Predictions({}, []), set()))

    def test_unassigned_tier_members_are_reported_in_complete_and_partial_output(self):
        ch = chr(0x2B73E)
        refs = self.refs_for({}, {ch: {"mù"}, "𠀀": {"mù"}})
        empty = rare.Predictions({}, [])
        members, withheld = rare.build_tiers(refs)
        self.assertEqual(members["cns"], ["𠀀"])
        self.assertEqual(withheld["cns"], [ch])
        for selected, complete in ((["𠀀"], True), ([ch], False)):
            with self.subTest(complete=complete):
                out = self.root / ("complete-members" if complete else "partial-members")
                rare.write_assembly(out, "cns", selected, refs, empty, set(), complete=complete)
                headers, rows = rare.document(out / "cns.tsv")
                self.assertEqual(bool(headers.get("complete")), complete)
                self.assertEqual([r[0] for _, r in rows], ["𠀀"] if complete else [])
                gaps = rare.document(out / "cns-gaps.tsv")[1]
                self.assertEqual([r[0] for _, r in gaps], [ch])
                self.assertTrue(gaps[0][1][-1].startswith("unsupported-member:"))
                self.assertFalse(rare.check_provenance(out / "cns.tsv", out / "cns-provenance.tsv", refs, empty, set()))
        with self.assertRaisesRegex(ValueError, "CJK ideograph"):
            rare.ideograph(ch)

    def test_full_cli_reports_unassigned_members_without_writing_rows(self):
        refs = self.refs_for({}, {chr(0x2B73E): {"mù"}, "𠀀": {"mù"}})
        out = self.root / "cli-withheld"
        with mock.patch.object(rare, "read_references", return_value=refs), contextlib.redirect_stderr(io.StringIO()):
            status = rare.main(["assemble", "--tier", "cns", "--authored", str(self.root / "authored"), "--output", str(out)])
        self.assertEqual(status, 0)
        self.assertEqual([r[0] for _, r in rare.document(out / "cns.tsv")[1]], ["𠀀"])
        self.assertIn(chr(0x2B73E), (out / "cns-gaps.tsv").read_text())

    def test_xian_policy_excludes_exact_relations_and_retains_evidence(self):
        refs = self.refs_for({"kSimplifiedVariant": {"線": "U+7F10"},
                              "kTraditionalVariant": {"缐": "U+7DDA", "线": "U+7DAB"},
                              "kMandarin": {ch: "xiàn" for ch in "線缐线"}},
                             opencc={"線": ["线", "缐"]})
        rows, records, gaps, findings = rare.assemble(list("線缐"), refs, rare.Predictions({}, []), set())
        self.assertEqual(rows, [("線", "xiàn", "线"), ("缐", "xiàn", "")])
        self.assertEqual(findings, [])
        self.assertEqual([(r.ch, r.value) for r in records if not r.rank], [("線", "线")])
        self.assertEqual({(ch, target, reason) for ch, target, _, _, reason in gaps},
                         {("線", "缐", "policy-excluded:kSimplifiedVariant"),
                          ("缐", "線", "policy-excluded:kTraditionalVariant"),
                          ("線", "缐", "policy-excluded:opencc:TSCharacters")})
        self.assertEqual(refs.fields["kSimplifiedVariant"]["線"], "U+7F10")

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
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("source terms unresolved", result.stdout)
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

    def test_evidence_tie_accepts_an_alternative_only_with_its_exact_reviewed_row(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kHanyuPinyin": {"𠀀": "1:hé,mā"}})
        predictions = rare.Predictions({}, [])
        out = self.root / "evidence-choice"
        rare.write_assembly(out, "tgh", ["𠀀"], refs, predictions, set())
        table, sidecar = out / "tgh.tsv", out / "tgh-provenance.tsv"
        table.write_text(table.read_text().replace("hé mā", "mā hé"))
        headers, records = rare.read_records(sidecar)
        text = sidecar.read_text()
        for record in records:
            columns = record.columns()
            columns[5] = "1" if record.value == "mā" else "2"
            text = text.replace("\t".join(record.columns()), "\t".join(columns))
        sidecar.write_text(text)
        findings = rare.check_provenance(table, sidecar, refs, predictions, set())
        self.assertEqual([(f[3], f[4], f[5]) for f in findings],
                         [("provenance", "mā hé", "evidence-order tie requires a reviewed decision")])
        argv = ["check", str(table), "--provenance", str(sidecar), "--reviewed", str(self.reviewed)]
        with (mock.patch.object(rare, "read_references", return_value=refs),
              mock.patch.object(rare, "read_predictions", return_value=predictions),
              mock.patch.object(rare, "authored_before", return_value=set()),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            self.reviewed.write_text("\t".join(data.finding_key(findings[0])) + "\tSynthetic choice\n")
            self.assertEqual(rare.main(argv), 0)
            self.reviewed.write_text(self.reviewed.read_text().replace("mā hé", "hé mā"))
            self.assertEqual(rare.main(argv), 1)

    def test_reviewed_permutations_preserve_region_evidence_and_attested_membership(self):
        refs = self.refs_for({"kHanyuPinyin": {"𠀀": "1:hé,mā"}}, {"𠀀": {"bá"}})
        predictions = rare.Predictions({}, [])
        records, issues = rare.ordered_readings("𠀀", refs, predictions, ["mā", "hé", "bá"])
        self.assertEqual([r.value for r in records], ["mā", "hé", "bá"])
        self.assertEqual(issues, [("mā hé bá", "evidence-order tie requires a reviewed decision")])
        for choice in (["bá", "mā", "hé"], ["mā", "hé"], ["mā", "mā", "bá"], ["mā", "hé", "bō"]):
            with self.subTest(choice=choice):
                records, _ = rare.ordered_readings("𠀀", refs, predictions, choice)
                self.assertEqual([r.value for r in records], ["hé", "mā", "bá"])
        refs.cns["𠀀"] = {"bá", "hé"}
        records, issues = rare.ordered_readings("𠀀", refs, predictions, ["mā", "hé", "bá"])
        self.assertEqual(([r.value for r in records], issues), (["hé", "mā", "bá"], []))

    def test_prediction_coordinates_and_text_reject_private_locations(self):
        for value in ("home/user/private", "../private", "./private", "C:\\private", "release/one"):
            with self.subTest(coordinate=value), self.assertRaises(ValueError):
                rare.coordinate(value)
        for url in ("http://localhost/a", "http://localhost./a", "http://host.local/a",
                    "http://127.0.0.1/a", "http://10.0.0.1/a", "http://172.16.0.1/a",
                    "http://192.168.1.1/a", "http://[::1]/a", "http://[fc00::1]/a",
                    "http://[::ffff:127.0.0.1]/a", "http://2130706433/a", "http://127.1/a", "http://0x7f.0.0.1/a", "http://host.home.arpa/a"):
            with self.subTest(source=url), self.assertRaises(ValueError):
                rare.coordinate(url, source=True)
        for url in ("https://example.org/source/archive.zip", "https://www.unicode.org/Public/", "fixture:synthetic"):
            rare.coordinate(url, source=True)
        for column in (3, 4, 6):
            for value in ("from home/user/private", "./inputs", "notes /home/example/file", "C:\\private\\file"):
                with self.subTest(column=column, value=value):
                    path = prediction_file(self.root / "private.tsv", [("𠀀", "mā", "")])
                    lines = path.read_text().splitlines()
                    columns = lines[-1].split("\t")
                    columns[column] = value
                    lines[-1] = "\t".join(columns)
                    path.write_text("\n".join(lines) + "\n")
                    with self.assertRaisesRegex(ValueError, "paths or session identifiers"):
                        rare.read_predictions([path])

    def test_notice_requires_attribution_and_file_scope_for_readings_and_mappings(self):
        refs = self.refs_for({"kTGH": {"𠀀": "1"}, "kMandarin": {"𠀀": "mā"}},
                             {"𠀀": {"mā"}}, {"𠀀": ["木"]})
        predictions = rare.Predictions({}, [])
        out = self.root / "notices"
        rare.write_assembly(out, "tgh", ["𠀀"], refs, predictions, set())
        table, sidecar = out / "tgh.tsv", out / "tgh-provenance.tsv"
        notice = self.root / "NOTICE"
        original = (data.DEFAULT_AUTHORED_DIR / "NOTICE").read_text()
        notice.write_text(original)
        self.assertFalse(rare.check_provenance(table, sidecar, refs, predictions, set(), notice))
        for source in ("unihan", "cns", "opencc"):
            scopes = copy.deepcopy(self.scopes)
            scopes[source].remove("tgh.tsv")
            notice.write_text(rare.notice_terms.render_notice(self.registry, scopes))
            findings = rare.check_provenance(table, sidecar, refs, predictions, set(), notice)
            self.assertEqual(len(findings), 1)
            self.assertIn(source + ": NOTICE attribution outside table scope", findings[0][5])
        for removed in (self.registry["sources"]["unihan"]["attribution"]["copyright"], "OGDL-Taiwan-1.0", "Apache-2.0"):
            notice.write_text(original.replace(removed, "absent"))
            self.assertTrue(rare.check_provenance(table, sidecar, refs, predictions, set(), notice))
        notice.unlink()
        self.assertEqual(rare.check_provenance(table, sidecar, refs, predictions, set(), notice)[0][4], "invalid NOTICE")

    def test_notice_missing_from_shipping_scope_fails_the_checker(self):
        out, table, sidecar = self.assemble_fixture()
        notice = self.root / "NOTICE"
        original = (data.DEFAULT_AUTHORED_DIR / "NOTICE").read_text()
        argv = ["check", str(table), "--provenance", str(sidecar), "--reviewed", str(self.reviewed),
                "--authored", str(self.root / "authored")]
        with (mock.patch.object(rare, "read_references", return_value=self.refs),
              mock.patch.object(rare, "read_predictions", return_value=self.predictions),
              mock.patch.object(data, "DEFAULT_AUTHORED_DIR", self.root),
              contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            self.assertEqual(rare.main(argv), 1)
            notice.write_text(original)
            self.assertEqual(rare.main(argv), 0)
            notice.write_text("\n".join(line for line in original.splitlines()
                                        if not (line.startswith("# attribution: ") and
                                                json.loads(line.removeprefix("# attribution: "))["reference"] == "cns")) + "\n")
            self.assertEqual(rare.main(argv), 1)

    def test_notice_rejects_missing_provider_and_duplicate_reference(self):
        records = [rare.Record("𠀀", "mā", "single", "unihan", "unihan", 1, "-")]
        original = (data.DEFAULT_AUTHORED_DIR / "NOTICE").read_text()
        notice = self.root / "NOTICE"
        line = next(line for line in original.splitlines() if line.startswith("# attribution: "))
        entry = json.loads(line.removeprefix("# attribution: "))
        entry["provider"] = ""
        for text in (original.replace(line, "# attribution: " + rare.canonical(entry)), original + line + "\n"):
            notice.write_text(text)
            self.assertEqual(rare.check_notice(pathlib.Path("tgh.tsv"), records, notice, self.refs.pins)[0][4], "invalid NOTICE")

    def test_cached_smoke_skips_absent_inputs_but_rejects_a_wrong_checksum(self):
        paths = {name: self.root / name for name in ("unihan", "cns-properties", "cns-mappings", "opencc")}
        with (mock.patch.object(data, "reference_cache", return_value=paths),
              mock.patch.object(data.urllib.request, "urlretrieve", side_effect=AssertionError("network"))):
            self.assertIsNone(pinned_references())
            for path in paths.values():
                path.write_bytes(b"invalid cached fixture")
            with self.assertRaisesRegex(ValueError, "cached reference sha256 mismatch"):
                pinned_references()

    def integrated_check(self, table, refs, authored, provenance=None, predictions=None):
        with (mock.patch.object(data, "unihan_archive", return_value=object()),
              mock.patch.object(data, "unihan_fields", return_value=refs.fields),
              mock.patch.object(data, "cns_archives", return_value=(object(), object())),
              mock.patch.object(data, "cns_readings", return_value=refs.cns),
              mock.patch.object(data, "opencc_archive", return_value=object()),
              mock.patch.object(data, "opencc_characters", return_value=refs.opencc),
              mock.patch.object(rare, "shipped_pins", return_value=refs.pins),
              contextlib.redirect_stdout(output := io.StringIO()), contextlib.redirect_stderr(io.StringIO())):
            result = data.check([table], self.reviewed, authored_dir=authored,
                                provenance_dir=provenance, predictions_dir=predictions or self.root / "no-predictions")
        return result, [line.split("\t") for line in output.getvalue().splitlines()]

    def test_marked_tables_use_provenance_rules_and_unmarked_tables_keep_legacy_rules(self):
        refs = self.refs_for({"kMandarin": {"𠀀": "mā", "𠀂": "mā", "木": "mā", "禾": "mā"},
                              "kHanyuPinyin": {"𠀀": "1:bā,hē"},
                              "kSimplifiedVariant": {"𠀀": "U+20001", "𠀃": "U+2F803"},
                              "kZVariant": {"𠀁": "U+6728"}, "kTGH": {"𠄢": "1"},
                              "kSemanticVariant": {"𠀂": "U+6728 U+79BE"}},
                             {"𠀀": {"bā"}, "𠀂": {"mā"}, "禾": {"mā"}})
        out = self.root / "legacy"
        out.mkdir()
        (out / "gb2312.tsv").write_text("木\tmā\t\n禾\tmā\t\n")
        rare.write_assembly(out, "extb", ["𠀀", "𠀂", "𠀃"], refs, rare.Predictions({}, []), {"木", "禾"})
        table = out / "extb.tsv"
        table.write_text(table.read_text().replace("# complete\n", ""))
        sidecars = self.root / "legacy-sidecars"
        sidecars.mkdir()
        (sidecars / table.name).write_bytes((out / "extb-provenance.tsv").read_bytes())
        result, findings = self.integrated_check(table, refs, out, sidecars)
        self.assertEqual((result, findings), (0, []))
        table.write_text(table.read_text().replace("# schema: fold-han-provenance/1\n", ""))
        result, findings = self.integrated_check(table, refs, out, sidecars)
        self.assertEqual(result, 1)
        self.assertTrue({"default", "archaic", "simplified", "family-target", "variant-guard"}
                        <= {f[3] for f in findings}, findings)

    def test_provenance_directory_does_not_audit_unmarked_essential_files(self):
        refs = self.refs_for({"kMandarin": {"木": "mù"}})
        table = self.root / "gb2312.tsv"
        table.write_text("木\tmù\t\n")
        with mock.patch.object(rare, "check_provenance", side_effect=AssertionError("unmarked essential audit")):
            result, findings = self.integrated_check(table, refs, self.root, self.root / "missing-sidecars")
        self.assertEqual((result, findings), (0, []))
        table.write_text("木\thé\t禾\n")
        result, findings = self.integrated_check(table, refs, self.root, self.root / "missing-sidecars")
        self.assertEqual(result, 1)
        self.assertTrue({"default", "simplified", "unattested", "tone"} <= {f[3] for f in findings})
        self.assertNotIn("provenance", {f[3] for f in findings})

    def test_prediction_batch_selection_uses_exact_tier(self):
        for name in ("big5-fixture-1.tsv", "big5-2-fixture-1.tsv", "big5-2-fixture-gaps.tsv", "tgh-other.tsv"):
            (self.root / name).write_text("deliberately invalid excluded input")
        self.assertEqual([p.name for p in data.prediction_logs(self.root, "big5")], ["big5-fixture-1.tsv"])
        self.assertEqual([p.name for p in data.prediction_logs(self.root, "big5-2")], ["big5-2-fixture-1.tsv"])
        refs = self.refs_for({"kMandarin": {"木": "mù"}})
        out = self.root / "marked-essential"
        rare.write_assembly(out, "big5", ["木"], refs, rare.Predictions({}, []), set())
        table = out / "big5.tsv"
        table.write_text(table.read_text().replace("# complete\n", ""))
        (self.root / "big5-fixture-1.tsv").unlink()
        sidecars = self.root / "essential-sidecars"
        sidecars.mkdir()
        (sidecars / "big5.tsv").write_bytes((out / "big5-provenance.tsv").read_bytes())
        with mock.patch.object(rare, "check_notice", return_value=[]):
            result, findings = self.integrated_check(table, refs, out, sidecars, self.root)
        self.assertEqual((result, findings), (0, []))

    def test_integrated_checker_requires_provenance_and_uses_evidence_default(self):
        out, table, provenance = self.assemble_fixture()
        shutil.copyfile(self.notice_root / "NOTICE", out / "NOTICE")
        shutil.copytree(self.notice_root / "licenses", out / "licenses")
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
    """Return verified cached references without downloading, or None when one is absent."""
    paths = data.reference_cache()
    pins = rare.shipped_pins()
    if not all(path.is_file() for path in paths.values()):
        return None
    for name, path in paths.items():
        if rare.digest(path) != pins[name]["sha256"]:
            raise ValueError(f"{name}: cached reference sha256 mismatch")
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
        cls.tiers, cls.withheld = rare.build_tiers(cls.refs)

    def test_pinned_mou3_rule_counts_and_nonattestation(self):
        self.assertEqual(self.refs.cns.rules["cns-mou3/1"]["counts"],
                         {"keep": 22, "exclude": 3860, "quarantine": 130})
        self.assertEqual(len(self.refs.cns.rule_rows), 4012)
        self.assertIn("mǒu", self.refs.attestations("牡"))
        self.assertNotIn("mǒu", self.refs.attestations("蓟"))
        for code in data.CNS_MOU3_QUARANTINE:
            row = next(r for r in self.refs.cns.rule_rows if r["cns"] == code)
            self.assertEqual(row["status"], "quarantine")

    def test_sms_tier_moves_share_unihan_lineage_without_mainland_precedence(self):
        for ch, tier in (("䖬", "exta"), ("䪡", "exta"), ("䵍", "exta"), ("𦍋", "core")):
            self.assertIn(ch, self.tiers[tier])
            self.assertNotIn(ch, self.tiers["cns"])
        fields = {name: {} for name in rare.FIELDS}
        fields["kSMSZD2003Readings"] = {"㐀": "mā粵maa1"}
        refs = rare.References(fields, {}, {}, self.refs.pins)
        self.assertEqual(refs.attestations("㐀"), {"mā": {"unihan"}})
        self.assertEqual(refs.mainland("㐀"), set())

    def test_syllable_inventory_matches_the_pinned_unihan_fields(self):
        derived = {base for field in ("kMandarin", "kHanyuPinyin", "kXHC1983", "kTGHZ2013", "kHanyuPinlu")
                   for value in self.refs.fields[field].values()
                   for reading in data.field_readings(field, value)
                   if (base := data.pinyin_base(reading)) is not None}
        self.assertEqual(data.MANDARIN_SYLLABLES, derived | {"pia", "yai", "nui", "sei", "gue"})

    def test_every_tier_assembles(self):
        for name in (path.removesuffix(".tsv") for path in data.RARE_FILES):
            with self.subTest(tier=name):
                members = self.tiers[name]
                self.assertTrue(all(data.in_ideograph_blocks(ch) for ch in members))
                authored = rare.authored_before(data.DEFAULT_AUTHORED_DIR, name)
                rows, _, _, findings = rare.assemble(members, self.refs, rare.Predictions({}, []), authored, tier=name)
                self.assertEqual([row[0] for row in rows], members)
                self.assertLessEqual({f[1] for f in findings}, {"provenance", "variant-guard", "family-target"})
                self.assertFalse([f for f in findings if f[3].startswith("unparseable")])
                self.assertFalse([f for f in findings if f[1] == "variant-guard"
                                  and not f[3].startswith("variant-guard:tied-lineages=")])


if __name__ == "__main__":
    unittest.main()
