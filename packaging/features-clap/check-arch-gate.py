#!/usr/bin/env python3
"""Report whether the distro-native CLAP provider route can reopen.

The ONNX runtime provider needs only NumPy, ONNX Runtime, and Tokenizers at
serving time, so the old NumPy/Numba/LAION-CLAP compatibility chain no longer
gates distro packaging. What gates it now:

1. every runtime dependency must be resolvable on Arch, from the official
   repositories (directly or through ``provides`` variants such as
   ``python-onnxruntime-cpu``) or, acceptably for an AUR package, from the
   AUR itself;
2. the published PyPI provider's ``[model]`` extra must be the ONNX runtime,
   proving the release actually shipped; and
3. a hosted converted-artifact bundle must be configured
   (``MODEL_ARTIFACTS_URL``), because a distro package cannot ask users to
   install the one-time ``[convert]`` stack that is itself not packageable.

Index compatibility is necessary, not sufficient. A ``candidate`` result must
still pass the clean-chroot runtime gates documented beside this script
before any AUR provider package is published.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Sequence

SCHEMA_VERSION = 2
# Each runtime dependency with the official package names that satisfy it.
# The ONNX Runtime variants (-cpu/-cuda/-rocm) all `provides` the plain name.
ARCH_RUNTIME_DEPENDENCIES = (
    ("python-numpy", ("python-numpy",)),
    ("python-onnxruntime", ("python-onnxruntime", "python-onnxruntime-cpu")),
    ("python-tokenizers", ("python-tokenizers",)),
)
PLANNED_PACKAGES = ("muzaiten-features-clap",)
PROVIDER_PROJECT = "muzaiten-features-clap"
PROVIDER_MODEL_PATH = (
    Path(__file__).resolve().parents[2]
    / "tools/features-clap/src/muzaiten_features_clap/model.py"
)
USER_AGENT = "muzaiten-arch-provider-gate/2"


class PackageIndexError(RuntimeError):
    """An index response was unavailable or did not match its public schema."""


def _fetch_json(url: str) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:  # noqa: S310
            payload = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        raise PackageIndexError(f"failed to read {url}: {exc}") from exc
    if not isinstance(payload, dict):
        raise PackageIndexError(f"expected a JSON object from {url}")
    return payload


def _arch_package(name: str) -> dict[str, object]:
    query = urllib.parse.urlencode({"name": name})
    url = f"https://archlinux.org/packages/search/json/?{query}"
    payload = _fetch_json(url)
    results = payload.get("results")
    if not isinstance(results, list):
        raise PackageIndexError(f"Arch response for {name} has no results array")
    exact = [
        item
        for item in results
        if isinstance(item, dict)
        and item.get("pkgname") == name
        and not str(item.get("repo", "")).endswith("-testing")
    ]
    if not exact:
        return {"available": False, "url": url}
    exact.sort(key=lambda item: (str(item.get("repo")), str(item.get("arch"))))
    item = exact[0]
    return {
        "available": True,
        "version": str(item.get("pkgver", "")),
        "release": str(item.get("pkgrel", "")),
        "repository": str(item.get("repo", "")),
        "architecture": str(item.get("arch", "")),
        "url": url,
    }


def _pypi_project(name: str) -> dict[str, object]:
    url = f"https://pypi.org/pypi/{urllib.parse.quote(name)}/json"
    payload = _fetch_json(url)
    info = payload.get("info")
    if not isinstance(info, dict):
        raise PackageIndexError(f"PyPI response for {name} has no info object")
    requirements = info.get("requires_dist") or []
    if not isinstance(requirements, list):
        raise PackageIndexError(f"PyPI requirements for {name} are malformed")
    return {
        "version": str(info.get("version", "")),
        "requires_dist": [str(requirement) for requirement in requirements],
        "url": url,
    }


def _aur_package(name: str) -> dict[str, object]:
    query = urllib.parse.urlencode({"arg[]": name})
    url = f"https://aur.archlinux.org/rpc/v5/info?{query}"
    payload = _fetch_json(url)
    results = payload.get("results")
    if not isinstance(results, list):
        raise PackageIndexError(f"AUR response for {name} has no results array")
    exact = [item for item in results if isinstance(item, dict) and item.get("Name") == name]
    if not exact:
        return {"available": False, "url": url}
    item = exact[0]
    return {
        "available": True,
        "version": str(item.get("Version", "")),
        "maintainer": item.get("Maintainer"),
        "out_of_date": item.get("OutOfDate"),
        "url": url,
    }


def model_extra_requirements(requirements: Sequence[str]) -> list[str]:
    """Lower-case project names required under the ``model`` extra."""
    names: list[str] = []
    for requirement in requirements:
        parts = requirement.split(";", 1)
        marker = parts[1] if len(parts) == 2 else ""
        if re.search(r"""extra\s*==\s*['"]model['"]""", marker) is None:
            continue
        match = re.match(r"\s*([A-Za-z0-9][A-Za-z0-9._-]*)", parts[0])
        if match is not None:
            names.append(match.group(1).lower())
    return names


def repo_artifacts_url(path: Path = PROVIDER_MODEL_PATH) -> str | None:
    """MODEL_ARTIFACTS_URL as configured in the repository checkout."""
    import ast

    tree = ast.parse(path.read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        targets = []
        if isinstance(node, ast.AnnAssign) and node.value is not None:
            targets = [node.target]
        elif isinstance(node, ast.Assign):
            targets = node.targets
        if any(getattr(target, "id", None) == "MODEL_ARTIFACTS_URL" for target in targets):
            try:
                value = ast.literal_eval(node.value)
            except ValueError as exc:
                raise PackageIndexError(
                    f"MODEL_ARTIFACTS_URL is not a literal in {path}"
                ) from exc
            if value is None or isinstance(value, str):
                return value
            raise PackageIndexError(f"MODEL_ARTIFACTS_URL has an unsupported value: {value!r}")
    raise PackageIndexError(f"MODEL_ARTIFACTS_URL is not declared in {path}")


def evaluate_gate(
    indexes: dict[str, dict[str, Any]],
    hosted_artifacts_url: str | None,
) -> dict[str, object]:
    arch = indexes["arch"]
    pypi = indexes["pypi"]
    aur = indexes["aur"]

    reasons: list[str] = []
    runtime_state: dict[str, dict[str, object]] = {}
    for dependency, official_names in ARCH_RUNTIME_DEPENDENCIES:
        official = next(
            (
                (name, arch[name])
                for name in official_names
                if bool(arch.get(name, {}).get("available"))
            ),
            None,
        )
        if official is not None:
            name, item = official
            runtime_state[dependency] = {
                "realm": "official",
                "package": name,
                "version": item.get("version"),
            }
        elif bool(aur.get(dependency, {}).get("available")):
            # An AUR dependency is acceptable for an AUR package.
            runtime_state[dependency] = {
                "realm": "aur",
                "package": dependency,
                "version": aur[dependency].get("version"),
            }
        else:
            runtime_state[dependency] = {"realm": "missing", "package": None, "version": None}
            reasons.append(f"runtime dependency is unavailable on Arch: {dependency}")

    provider = pypi[PROVIDER_PROJECT]
    extra_names = model_extra_requirements(provider["requires_dist"])
    provider_is_onnx = "onnxruntime" in extra_names and "laion-clap" not in extra_names
    if not provider_is_onnx:
        reasons.append(
            f"published provider {provider['version']} [model] extra is not the "
            "ONNX runtime"
        )

    if hosted_artifacts_url is None:
        reasons.append(
            "hosted artifact bundle is not configured (MODEL_ARTIFACTS_URL is unset); "
            "a distro package cannot depend on the [convert] stack"
        )

    status = "blocked" if reasons else "candidate"
    next_step = (
        "publish the ONNX provider release and host the artifact bundle"
        if status == "blocked"
        else "run the documented clean-chroot runtime gates"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "checked_at": datetime.now(UTC).isoformat(),
        "status": status,
        "reasons": reasons,
        "next_step": next_step,
        "compatibility": {
            "runtime_packages": runtime_state,
            "provider": {
                "version": provider["version"],
                "model_extra": extra_names,
                "onnx_runtime": provider_is_onnx,
            },
            "hosted_artifacts": {
                "configured": hosted_artifacts_url is not None,
                "url": hosted_artifacts_url,
            },
        },
        "indexes": indexes,
        "planned_packages": {
            package: {
                "published": bool(aur[package].get("available")),
                "disposition": (
                    "consume if compatible" if aur[package].get("available") else "to package"
                ),
            }
            for package in PLANNED_PACKAGES
        },
    }


def collect_indexes() -> dict[str, dict[str, Any]]:
    arch: dict[str, Any] = {}
    aur: dict[str, Any] = {}
    for dependency, official_names in ARCH_RUNTIME_DEPENDENCIES:
        for name in official_names:
            arch[name] = _arch_package(name)
        if not any(bool(arch[name].get("available")) for name in official_names):
            aur[dependency] = _aur_package(dependency)
    pypi = {PROVIDER_PROJECT: _pypi_project(PROVIDER_PROJECT)}
    for package in PLANNED_PACKAGES:
        aur[package] = _aur_package(package)
    return {"arch": arch, "pypi": pypi, "aur": aur}


def _human_report(report: dict[str, object]) -> str:
    compatibility = report["compatibility"]
    assert isinstance(compatibility, dict)
    runtime = compatibility["runtime_packages"]
    provider = compatibility["provider"]
    hosted = compatibility["hosted_artifacts"]
    assert isinstance(runtime, dict) and isinstance(provider, dict) and isinstance(hosted, dict)
    lines = [f"Arch CLAP provider gate: {str(report['status']).upper()}"]
    for dependency, state in runtime.items():
        if state["realm"] == "missing":
            label = "missing"
        else:
            label = f"{state['package']} {state['version']} ({state['realm']})"
        lines.append(f"  {dependency}: {label}")
    lines.append(
        f"  PyPI provider {provider['version']}: "
        f"{'ONNX runtime' if provider['onnx_runtime'] else 'not the ONNX runtime'}"
    )
    lines.append(
        f"  hosted artifacts: {hosted['url'] if hosted['configured'] else 'not configured'}"
    )
    reasons = report["reasons"]
    assert isinstance(reasons, list)
    lines.extend(f"  - {reason}" for reason in reasons)
    lines.append(f"Next: {report['next_step']}")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit the stable JSON report")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 when blocked (ordinary reporting exits 0)",
    )
    args = parser.parse_args(argv)
    try:
        report = evaluate_gate(collect_indexes(), repo_artifacts_url())
    except (PackageIndexError, KeyError, TypeError, ValueError, OSError) as exc:
        error = {"schema_version": SCHEMA_VERSION, "status": "error", "error": str(exc)}
        if args.json:
            print(json.dumps(error, indent=2, sort_keys=True))
        else:
            print(f"Arch CLAP provider gate: ERROR\n  {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(_human_report(report))
    return 1 if args.strict and report["status"] == "blocked" else 0


if __name__ == "__main__":
    raise SystemExit(main())
