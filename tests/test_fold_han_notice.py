#!/usr/bin/env python3
"""Source-pin attribution, legal-copy and CMake staging fixtures."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fold_han_notice", ROOT / "tools/fold-han-notice.py")
notice = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notice)

# Package component licenses of published releases, keyed by version. muzaiten-bin
# downloads one of these artifacts rather than building the current source.
RELEASED_COMPONENT_LICENSES = {"2026.9.11": {"Unlicense", "Unicode-3.0"}}


class HanNoticeTests(unittest.TestCase):
    def setUp(self):
        build = pathlib.Path(os.environ.get("TMPDIR", ROOT / "build"))
        build.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="han-notices-", dir=build)
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.registry = notice.read_registry()
        self.pins = {key: pin for source in self.registry["sources"].values() for key, pin in source["pins"].items()}
        self.scopes = {name: ["tgh.tsv"] for name in self.registry["sources"]}
        self.table = self.root / "tgh.tsv"
        self.table.write_text("# fixture table\n", encoding="utf-8")
        self.notice = self.root / "NOTICE"
        self.notice.write_text(notice.render_notice(self.registry, self.scopes), encoding="utf-8")
        shutil.copytree(notice.HAN / "licenses", self.root / "licenses")
        self.records = [SimpleNamespace(attested_by="cns,unihan")]

    def check(self, records=None):
        notice.check_table(self.table, records if records is not None else self.records,
                           self.notice, self.pins, self.registry)

    def pins_for(self, *sources):
        return {key: pin for name in sources for key, pin in self.registry["sources"][name]["pins"].items()}

    def write_scopes(self, scopes):
        self.notice.write_text(notice.render_notice(self.registry, scopes), encoding="utf-8")

    def write_table(self, name):
        self.table = self.root / name
        self.table.write_text("# fixture table\n", encoding="utf-8")

    def unihan_notice(self, files):
        return notice.render_notice(self.registry, {"unihan": files, "cns": [], "opencc": []})

    def test_registered_pins_and_complete_notices(self):
        self.scopes = {name: ["tgh.tsv", "big5-2.tsv"] for name in self.registry["sources"]}
        self.notice.write_text(notice.render_notice(self.registry, self.scopes))
        self.check()

    def test_mixed_generated_and_authored_notice_scopes(self):
        self.scopes["unihan"] += ["unihan/han_pinyin.tsv", "unihan/han_simplified.tsv"]
        self.notice.write_text(notice.render_notice(self.registry, self.scopes))
        self.check()
        self.scopes["unihan"].append("../han_pinyin.tsv")
        self.notice.write_text(notice.render_notice(self.registry, self.scopes))
        with self.assertRaisesRegex(ValueError, "invalid NOTICE table scope"):
            self.check()

    def test_unknown_pin_and_missing_cns_component_block(self):
        self.pins["unihan"] = {**self.pins["unihan"], "sha256": "0" * 64}
        with self.assertRaisesRegex(ValueError, "source terms unresolved"):
            self.check()
        self.pins.pop("unihan")
        self.pins.pop("cns-mappings")
        with self.assertRaisesRegex(ValueError, "all component pins"):
            self.check()

    def test_required_cns_attribution_and_wrong_license(self):
        for field in ("agency", "year", "title", "version", "statement", "license_url", "website_credit"):
            with self.subTest(field=field):
                registry = copy.deepcopy(self.registry)
                del registry["sources"]["cns"]["attribution"][field]
                path = self.root / "terms.json"
                path.write_text(json.dumps(registry), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "cns attribution"):
                    notice.read_registry(path)
        registry = copy.deepcopy(self.registry)
        registry["sources"]["cns"]["license"] = "OFL-1.1"
        path.write_text(json.dumps(registry), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "license must be OGDL-Taiwan-1.0"):
            notice.read_registry(path)

    def test_render_drift_and_icu_copyright_substitution(self):
        original = self.notice.read_text()
        for text in (original.replace("數位發展部", "agency omitted"),
                     original.replace("© 2026 Unicode®, Inc.", "Copyright © 2016-2025 Unicode, Inc.")):
            self.notice.write_text(text)
            with self.assertRaisesRegex(ValueError, "differs from source terms"):
                self.check()

    def test_missing_table_scope(self):
        self.scopes["cns"] = []
        self.notice.write_text(notice.render_notice(self.registry, self.scopes))
        with self.assertRaisesRegex(ValueError, "outside table scope"):
            self.check()

    def test_duplicate_json_keys_and_reference(self):
        text = self.notice.read_text()
        line = next(line for line in text.splitlines() if line.startswith("# attribution: "))
        for changed in (text.replace('"reference":"unihan"', '"reference":"unihan","reference":"cns"'), text + line + "\n"):
            self.notice.write_text(changed)
            with self.assertRaisesRegex(ValueError, "duplicate"):
                self.check()

    def test_missing_and_truncated_license_copies(self):
        path = self.root / "licenses/APACHE-2.0.txt"
        path.unlink()
        with self.assertRaisesRegex(ValueError, "license copy:.*missing"):
            self.check()
        path.write_text("Apache License, Version 2.0\n")
        with self.assertRaisesRegex(ValueError, "license copy:.*hash differs"):
            self.check()

    def test_symlinked_and_escaping_license_assets(self):
        path = self.root / "licenses/UNICODE-3.0.txt"
        path.unlink()
        path.symlink_to(notice.HAN / "licenses/UNICODE-3.0.txt")
        with self.assertRaisesRegex(ValueError, "symlinked"):
            self.check()
        with self.assertRaisesRegex(ValueError, "unsafe"):
            notice.regular_file(self.root, "../NOTICE")

    def test_policy_copies_are_distinguished_from_legal_requirements(self):
        (self.root / "licenses/OGDL-Taiwan-1.0.txt").unlink()
        with self.assertRaisesRegex(ValueError, "project-policy copy:"):
            self.check()
        self.assertIn("not a license-required copy", self.notice.read_text())

    def test_opencc_modified_notice_present_iff_transformed_content(self):
        self.table.write_text(notice.MODIFIED_NOTICE + "\n")
        with self.assertRaisesRegex(ValueError, "must match transformed"):
            self.check()
        self.check([SimpleNamespace(attested_by="opencc:TSCharacters,unihan:kSimplifiedVariant")])
        self.table.write_text("# fixture table\n")
        with self.assertRaisesRegex(ValueError, "must match transformed"):
            self.check([SimpleNamespace(attested_by="opencc:TSCharacters")])

    def test_prediction_only_and_empty_records_still_require_all_pins(self):
        self.check([])
        self.scopes["cns"] = []
        self.notice.write_text(notice.render_notice(self.registry, self.scopes))
        with self.assertRaisesRegex(ValueError, "outside table scope"):
            self.check([])

    def test_actual_inventory_and_legacy_unicode_copy(self):
        scopes = notice.check_source(self.registry)
        partial = sorted(["tgh.tsv", "big5-2.tsv", "core.tsv", "uro.tsv", "extb.tsv", "extc.tsv"])
        self.assertEqual(scopes["unihan"], sorted(partial + ["unihan/han_pinyin.tsv", "unihan/han_simplified.tsv"]))
        self.assertEqual(scopes["cns"], partial)
        self.assertEqual(scopes["opencc"], partial)

    def test_unihan18_terms_bind_generator_pin_and_archive_notice(self):
        spec = importlib.util.spec_from_file_location("notice_fixture_data", ROOT / "tools/fold-han-data.py")
        data = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(data)
        source = self.registry["sources"]["unihan"]
        self.assertEqual(source["pins"]["unihan"], {
            "revision": "18.0.0",
            "sha256": "4c93ea9c1f636451729a840978f1667a53886af37ba854fdcce109721c63d43e",
            "source": "https://www.unicode.org/Public/18.0.0/ucd/Unihan.zip",
        })
        self.assertEqual(source["pins"]["unihan"], {"revision": data.UNIHAN_VERSION,
                         "sha256": data.UNIHAN_SHA256, "source": data.UNIHAN_URL})
        self.assertEqual(source["attribution"]["version"], "18.0.0")
        self.assertEqual(source["attribution"]["copyright"], "© 2026 Unicode®, Inc.")
        self.pins["unihan"] = {"revision": "16.0.0",
            "sha256": "b8f000df69de7828d21326a2ffea462b04bc7560022989f7cc704f10521ef3e0",
            "source": "https://www.unicode.org/Public/16.0.0/ucd/Unihan.zip"}
        with self.assertRaisesRegex(ValueError, "source terms unresolved"):
            self.check()

    def test_shipped_notice_scopes_match_source_inventory(self):
        text = (notice.HAN / "NOTICE").read_text(encoding="utf-8")
        scopes = notice.notice_scopes(text, self.registry)
        self.assertEqual(scopes, notice.scope_inventory(self.registry))
        self.assertLessEqual({"unihan/han_pinyin.tsv", "unihan/han_simplified.tsv"}, set(scopes["unihan"]))

    def test_mixed_inventory_attributes_each_table_to_listed_sources(self):
        self.scopes = {
            "unihan": ["unihan/han_pinyin.tsv", "unihan/han_simplified.tsv", "shared_table.tsv"],
            "cns": ["han_pinyin.tsv", "shared_table.tsv"],
            "opencc": [],
        }
        self.write_scopes(self.scopes)
        self.write_table("han_pinyin.tsv")
        self.pins = self.pins_for("cns")
        self.check()
        self.write_table("shared_table.tsv")
        self.pins = self.pins_for("unihan", "cns")
        self.check()

    def test_mixed_inventory_rejects_unlisted_sources_and_prefixed_names(self):
        self.scopes = {
            "unihan": ["unihan/han_pinyin.tsv", "unihan/han_simplified.tsv", "shared_table.tsv"],
            "cns": ["han_pinyin.tsv", "shared_table.tsv"],
            "opencc": [],
        }
        self.write_scopes(self.scopes)
        for name, sources, message in (
            ("han_pinyin.tsv", ("unihan",), "unihan: NOTICE attribution outside table scope"),
            ("han_pinyin.tsv", ("unihan", "cns"), "unihan: NOTICE attribution outside table scope"),
            ("shared_table.tsv", ("unihan", "cns", "opencc"), "opencc: NOTICE attribution outside table scope"),
        ):
            with self.subTest(table=name, sources=sources):
                self.write_table(name)
                self.pins = self.pins_for(*sources)
                with self.assertRaisesRegex(ValueError, message):
                    self.check()
        self.write_table("shared_table.tsv")
        pins = self.pins_for("unihan", "cns")
        pins["unihan"] = {**pins["unihan"], "sha256": "0" * 64}
        self.pins = pins
        with self.assertRaisesRegex(ValueError, "source terms unresolved"):
            self.check()

    def test_scope_name_shapes_stay_closed(self):
        for name in ("../han_pinyin.tsv", "unihan/../han_pinyin.tsv", "unihan/./han_pinyin.tsv",
                     "/unihan/han_pinyin.tsv", "unihan//han_pinyin.tsv", "unihan/nested/han_pinyin.tsv",
                     "unihan\\han_pinyin.tsv", "han/pinyin.tsv", "_han_pinyin.tsv", "1han_pinyin.tsv",
                     "han_pinyin.TSV", "han_pinyin.tsv.bak", "han_pinyin", "han.pinyin.tsv",
                     "han..pinyin.tsv", "han pinyin.tsv", "hän_pinyin.tsv", "han_pinyin.tsv "):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "invalid NOTICE table scope"):
                    notice.notice_scopes(self.unihan_notice([name]), self.registry)

    def test_underscored_scopes_keep_duplicate_and_pin_rejections(self):
        valid = ["unihan/han_pinyin.tsv", "unihan/han_simplified.tsv"]
        text = self.unihan_notice(valid)
        self.assertEqual(notice.notice_scopes(text, self.registry)["unihan"], valid)
        with self.assertRaisesRegex(ValueError, "invalid NOTICE table scope"):
            notice.notice_scopes(self.unihan_notice([valid[0], valid[0]]), self.registry)
        pin = self.registry["sources"]["unihan"]["pins"]["unihan"]["sha256"]
        self.assertEqual(text.count(pin), 1)
        with self.assertRaisesRegex(ValueError, "differs from source terms"):
            notice.notice_scopes(text.replace(pin, "0" * 64), self.registry)

    def test_package_metadata_matches_row_bearing_components(self):
        scopes = notice.scope_inventory(self.registry)
        licenses = {"Unlicense"} | {self.registry["sources"][name]["license"] for name, files in scopes.items() if files}
        for name in ("muzaiten-bin", "muzaiten-git"):
            text = (ROOT / "packaging/aur" / name / ".SRCINFO").read_text()
            declared = {line.split("=", 1)[1].strip() for line in text.splitlines() if line.strip().startswith("license =")}
            build = (ROOT / "packaging/aur" / name / "PKGBUILD").read_text()
            expected = licenses
            if name == "muzaiten-bin":
                # The binary recipe repackages a published artifact, so it declares the
                # components of that release; a new release ships the current inventory.
                version = re.search(r"^pkgver=(\S+)$", build, re.M).group(1)
                expected = RELEASED_COMPONENT_LICENSES.get(version, licenses)
            self.assertEqual(declared, expected, name)
            build_licenses = set(re.search(r"^license=\(([^)]*)\)$", build, re.M).group(1).replace("'", "").split())
            self.assertEqual(build_licenses, expected, name)
        expression = ET.parse(ROOT / "packaging/org.11xx.muzaiten.metainfo.xml").getroot().findtext("project_license")
        self.assertEqual(set(expression.split(" AND ")), licenses)
        for license in licenses:
            self.assertIn("'" + license + "'", (ROOT / "packaging/build-release.sh").read_text())

    def stage_install(self):
        source = self.root / "cmake-source"
        source.mkdir()
        cmake = source / "CMakeLists.txt"
        cmake.write_text(f'''cmake_minimum_required(VERSION 3.20)
project(HanNoticeInstall NONE)
set(CMAKE_INSTALL_LIBDIR lib)
include(GNUInstallDirs)
include("{ROOT.as_posix()}/cmake/HanNotices.cmake")
muzaiten_install_han_notices("{ROOT.as_posix()}" "{pathlib.Path(sys.executable).as_posix()}")
''')
        build = self.root / "cmake-build"
        stage = self.root / "stage"
        for command in (["cmake", "-S", str(source), "-B", str(build)],
                        ["cmake", "--install", str(build), "--prefix", str(stage), "--component", "HanNotices"]):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return stage / "share/licenses/muzaiten"

    def test_cmake_staging_install_delivers_notices_and_exact_legal_texts(self):
        stage = self.stage_install()
        notice.check_source(self.registry, package=stage)
        expected = {"UNLICENSE", "NOTICE", *self.registry["files"]}
        self.assertEqual({p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()}, expected)

    def test_staged_notice_or_license_omission_blocks_package(self):
        stage = self.stage_install()
        for asset in ("NOTICE", "licenses/UNICODE-3.0.txt", "licenses/APACHE-2.0.txt"):
            with self.subTest(asset=asset):
                path = stage / asset
                original = path.read_bytes()
                path.unlink()
                with self.assertRaises(ValueError):
                    notice.check_source(self.registry, package=stage)
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
