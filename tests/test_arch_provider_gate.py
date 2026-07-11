#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

CHECKER_PATH: Path | None = None


def load_gate(path: Path):
    spec = importlib.util.spec_from_file_location("arch_provider_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ArchProviderGateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if CHECKER_PATH is None:
            raise RuntimeError("checker path was not initialized")
        cls.gate = load_gate(CHECKER_PATH)

    def indexes(self, numpy: str = "2.5.1", *, missing: str | None = None):
        arch = {
            name: {"version": version}
            for name, version in {
                "python-numpy": numpy,
                "python-numba": "0.66.0",
                "python-pytorch": "2.12.1",
                "python-torchvision": "0.27.1",
                "python-librosa": "0.11.0",
            }.items()
        }
        aur = {
            name: {"available": name != missing, "version": "1.0-1"}
            for name in (*self.gate.AUR_RUNTIME_DEPENDENCIES, *self.gate.PLANNED_PACKAGES)
        }
        return {
            "arch": arch,
            "pypi": {
                "numba": {"version": "0.66.0", "requires_dist": ["numpy<2.5,>=1.22"]},
                "laion-clap": {
                    "version": "1.1.7",
                    "requires_dist": ["numpy<2.0.0,>=1.23.5", "torch"],
                },
            },
            "aur": aur,
        }

    def test_current_indexes_report_both_numpy_blockers(self) -> None:
        report = self.gate.evaluate_gate(self.indexes())
        self.assertEqual(report["status"], "blocked")
        self.assertFalse(report["compatibility"]["numba"]["satisfied"])
        self.assertFalse(report["compatibility"]["laion_clap"]["satisfied"])
        self.assertEqual(len(report["reasons"]), 2)

    def test_compatible_indexes_are_only_a_candidate(self) -> None:
        report = self.gate.evaluate_gate(self.indexes("1.26.4"))
        self.assertEqual(report["status"], "candidate")
        self.assertIn("clean-chroot", report["next_step"])

    def test_missing_runtime_dependency_blocks_candidate(self) -> None:
        report = self.gate.evaluate_gate(self.indexes("1.26.4", missing="python-webdataset"))
        self.assertEqual(report["status"], "blocked")
        self.assertIn(
            "required AUR dependency is unavailable: python-webdataset",
            report["reasons"],
        )

    def test_planned_packages_are_outputs_not_index_blockers(self) -> None:
        indexes = self.indexes("1.26.4")
        for name in self.gate.PLANNED_PACKAGES:
            indexes["aur"][name] = {"available": False}
        report = self.gate.evaluate_gate(indexes)
        self.assertEqual(report["status"], "candidate")
        self.assertTrue(
            all(item["disposition"] == "to package" for item in report["planned_packages"].values())
        )

    def test_requirement_parser_accepts_parenthesized_metadata(self) -> None:
        requirement = self.gate._numpy_requirement(["numpy (<2.5,>=1.22); python_version >= '3.10'"])
        self.assertEqual(requirement, "<2.5,>=1.22")
        self.assertTrue(self.gate._satisfies("2.4.2", requirement))
        self.assertFalse(self.gate._satisfies("2.5.0", requirement))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise RuntimeError("usage: test_arch_provider_gate.py CHECKER")
    CHECKER_PATH = Path(sys.argv[1])
    sys.argv = [sys.argv[0]]
    unittest.main()
