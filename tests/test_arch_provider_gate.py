#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
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


HOSTED_URL = "https://example.invalid/muzaiten-clap-onnx-v1"
ONNX_MODEL_EXTRA = [
    'numpy>=1.23.5; extra == "model"',
    'onnxruntime>=1.23; extra == "model"',
    'tokenizers>=0.22; extra == "model"',
]
TORCH_MODEL_EXTRA = [
    'laion-clap==1.1.7; extra == "model"',
    'torch>=2.3; extra == "model"',
]


class ArchProviderGateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if CHECKER_PATH is None:
            raise RuntimeError("checker path was not initialized")
        cls.gate = load_gate(CHECKER_PATH)

    def indexes(
        self,
        *,
        missing: str | None = None,
        model_extra: list[str] | None = None,
        aur_published: bool = False,
    ):
        # Mirrors live reality: ONNX Runtime ships as an official provides
        # variant, and Tokenizers is resolvable only from the AUR.
        arch = {
            "python-numpy": {"available": True, "version": "2.5.1"},
            "python-onnxruntime": {"available": False},
            "python-onnxruntime-cpu": {"available": True, "version": "1.27.1"},
            "python-tokenizers": {"available": False},
        }
        aur = {"python-tokenizers": {"available": True, "version": "0.23.1-1"}}
        if missing is not None:
            for name in arch:
                if name.startswith(missing):
                    arch[name] = {"available": False}
            aur.pop(missing, None)
        pypi = {
            self.gate.PROVIDER_PROJECT: {
                "version": "2026.7.12",
                "requires_dist": ONNX_MODEL_EXTRA if model_extra is None else model_extra,
            }
        }
        for name in self.gate.PLANNED_PACKAGES:
            aur[name] = {"available": aur_published, "version": "2026.7.12-1"}
        return {"arch": arch, "pypi": pypi, "aur": aur}

    def test_complete_indexes_and_hosted_bundle_are_a_candidate(self) -> None:
        report = self.gate.evaluate_gate(self.indexes(), HOSTED_URL)
        self.assertEqual(report["status"], "candidate")
        self.assertIn("clean-chroot", report["next_step"])
        self.assertTrue(report["compatibility"]["provider"]["onnx_runtime"])
        runtime = report["compatibility"]["runtime_packages"]
        self.assertEqual(runtime["python-onnxruntime"]["realm"], "official")
        self.assertEqual(runtime["python-onnxruntime"]["package"], "python-onnxruntime-cpu")
        self.assertEqual(runtime["python-tokenizers"]["realm"], "aur")

    def test_unconfigured_hosted_bundle_blocks(self) -> None:
        report = self.gate.evaluate_gate(self.indexes(), None)
        self.assertEqual(report["status"], "blocked")
        self.assertTrue(
            any("MODEL_ARTIFACTS_URL" in reason for reason in report["reasons"])
        )

    def test_missing_runtime_package_blocks(self) -> None:
        report = self.gate.evaluate_gate(
            self.indexes(missing="python-onnxruntime"), HOSTED_URL
        )
        self.assertEqual(report["status"], "blocked")
        self.assertIn(
            "runtime dependency is unavailable on Arch: python-onnxruntime",
            report["reasons"],
        )

    def test_aur_only_dependency_is_not_a_blocker(self) -> None:
        report = self.gate.evaluate_gate(self.indexes(), HOSTED_URL)
        self.assertFalse(
            any("python-tokenizers" in reason for reason in report["reasons"])
        )
        report = self.gate.evaluate_gate(
            self.indexes(missing="python-tokenizers"), HOSTED_URL
        )
        self.assertEqual(report["status"], "blocked")
        self.assertIn(
            "runtime dependency is unavailable on Arch: python-tokenizers",
            report["reasons"],
        )

    def test_published_torch_provider_blocks(self) -> None:
        report = self.gate.evaluate_gate(
            self.indexes(model_extra=TORCH_MODEL_EXTRA), HOSTED_URL
        )
        self.assertEqual(report["status"], "blocked")
        self.assertFalse(report["compatibility"]["provider"]["onnx_runtime"])

    def test_planned_package_is_an_output_not_a_blocker(self) -> None:
        report = self.gate.evaluate_gate(self.indexes(aur_published=False), HOSTED_URL)
        self.assertEqual(report["status"], "candidate")
        planned = report["planned_packages"][self.gate.PROVIDER_PROJECT]
        self.assertEqual(planned["disposition"], "to package")

    def test_model_extra_parser_reads_markers_and_names(self) -> None:
        names = self.gate.model_extra_requirements(
            [
                "numpy>=1.23.5",
                "onnxruntime (>=1.23) ; extra == 'model'",
                'tokenizers>=0.22; python_version >= "3.11" and extra == "model"',
            ]
        )
        self.assertEqual(names, ["onnxruntime", "tokenizers"])

    def test_repo_artifacts_url_reads_unset_and_configured_values(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "model.py"
            path.write_text("MODEL_ARTIFACTS_URL: str | None = None\n", encoding="utf-8")
            self.assertIsNone(self.gate.repo_artifacts_url(path))
            path.write_text(
                'MODEL_ARTIFACTS_URL: str | None = "https://example.invalid/x"\n',
                encoding="utf-8",
            )
            self.assertEqual(
                self.gate.repo_artifacts_url(path), "https://example.invalid/x"
            )
            path.write_text(
                'MODEL_ARTIFACTS_URL: str | None = (\n'
                '    "https://example.invalid/wrapped"\n'
                ')\n',
                encoding="utf-8",
            )
            self.assertEqual(
                self.gate.repo_artifacts_url(path), "https://example.invalid/wrapped"
            )

    def test_live_repo_configuration_parses(self) -> None:
        # Whatever the checkout currently pins must parse without error.
        self.gate.repo_artifacts_url()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise RuntimeError("usage: test_arch_provider_gate.py CHECKER")
    CHECKER_PATH = Path(sys.argv[1])
    sys.argv = [sys.argv[0]]
    unittest.main()
