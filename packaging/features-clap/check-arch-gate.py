#!/usr/bin/env python3
"""Report whether Arch's package indexes justify reopening CLAP packaging.

Index compatibility is necessary, not sufficient. A ``candidate`` result must
still pass the clean-chroot CPU and isolated CUDA gates documented beside this
script before any AUR provider package is published.
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
from typing import Any, Sequence

SCHEMA_VERSION = 1
ARCH_PACKAGES = (
    "python-numpy",
    "python-numba",
    "python-pytorch",
    "python-torchvision",
    "python-librosa",
)
AUR_RUNTIME_DEPENDENCIES = (
    "python-ftfy",
    "python-braceexpand",
    "python-webdataset",
    "python-wget",
    "python-wandb",
    "python-transformers",
)
PLANNED_PACKAGES = (
    "python-torchlibrosa",
    "python-laion-clap",
    "muzaiten-features-clap",
)
USER_AGENT = "muzaiten-arch-provider-gate/1"


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
        raise PackageIndexError(f"official Arch package is unavailable: {name}")
    exact.sort(key=lambda item: (str(item.get("repo")), str(item.get("arch"))))
    item = exact[0]
    return {
        "version": str(item.get("pkgver", "")),
        "release": str(item.get("pkgrel", "")),
        "repository": str(item.get("repo", "")),
        "architecture": str(item.get("arch", "")),
        "url": url,
    }


def _pypi_project(name: str, version: str | None = None) -> dict[str, object]:
    suffix = f"/{version}" if version else ""
    url = f"https://pypi.org/pypi/{urllib.parse.quote(name)}{suffix}/json"
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


def _release_tuple(version: str) -> tuple[int, ...]:
    plain = version.split(":", 1)[-1].split("+", 1)[0]
    match = re.match(r"^(\d+(?:\.\d+)*)", plain)
    if match is None:
        raise ValueError(f"unsupported version: {version!r}")
    return tuple(int(part) for part in match.group(1).split("."))


def _compare(left: str, right: str) -> int:
    left_parts = list(_release_tuple(left))
    right_parts = list(_release_tuple(right))
    width = max(len(left_parts), len(right_parts))
    left_parts.extend([0] * (width - len(left_parts)))
    right_parts.extend([0] * (width - len(right_parts)))
    return (left_parts > right_parts) - (left_parts < right_parts)


def _numpy_requirement(requirements: Sequence[str]) -> str | None:
    for requirement in requirements:
        match = re.match(
            r"^numpy(?:\[[^]]+\])?\s*(?:\(([^)]*)\)|([^;]*))",
            requirement,
            flags=re.IGNORECASE,
        )
        if match is not None:
            return (match.group(1) or match.group(2) or "").strip() or None
    return None


def _satisfies(version: str, specification: str | None) -> bool:
    if specification is None:
        return True
    clauses = [clause.strip() for clause in specification.split(",") if clause.strip()]
    for clause in clauses:
        match = re.fullmatch(r"(<=|>=|==|!=|<|>)\s*([0-9][0-9A-Za-z.+_-]*)", clause)
        if match is None:
            raise ValueError(f"unsupported version clause: {clause!r}")
        operator, target = match.groups()
        comparison = _compare(version, target)
        accepted = {
            "<": comparison < 0,
            "<=": comparison <= 0,
            ">": comparison > 0,
            ">=": comparison >= 0,
            "==": comparison == 0,
            "!=": comparison != 0,
        }[operator]
        if not accepted:
            return False
    return True


def evaluate_gate(indexes: dict[str, dict[str, object]]) -> dict[str, object]:
    arch = indexes["arch"]
    pypi = indexes["pypi"]
    aur = indexes["aur"]
    numpy_version = str(arch["python-numpy"]["version"])
    numba_version = str(arch["python-numba"]["version"])
    numba_requirement = _numpy_requirement(
        pypi["numba"]["requires_dist"]  # type: ignore[arg-type]
    )
    laion_requirement = _numpy_requirement(
        pypi["laion-clap"]["requires_dist"]  # type: ignore[arg-type]
    )
    numba_compatible = _satisfies(numpy_version, numba_requirement)
    laion_compatible = _satisfies(numpy_version, laion_requirement)

    reasons: list[str] = []
    if not numba_compatible:
        reasons.append(
            f"Arch NumPy {numpy_version} does not satisfy Numba {numba_version} "
            f"requirement {numba_requirement}"
        )
    if not laion_compatible:
        reasons.append(
            f"Arch NumPy {numpy_version} does not satisfy LAION-CLAP "
            f"{pypi['laion-clap']['version']} requirement {laion_requirement}"
        )
    for package in AUR_RUNTIME_DEPENDENCIES:
        if not bool(aur[package].get("available")):
            reasons.append(f"required AUR dependency is unavailable: {package}")

    status = "blocked" if reasons else "candidate"
    next_step = (
        "wait for compatible upstream Arch and LAION-CLAP releases"
        if status == "blocked"
        else "run the documented clean-chroot CPU, parity, and CUDA gates"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "checked_at": datetime.now(UTC).isoformat(),
        "status": status,
        "reasons": reasons,
        "next_step": next_step,
        "compatibility": {
            "arch_numpy": numpy_version,
            "numba": {
                "version": numba_version,
                "numpy_requirement": numba_requirement,
                "satisfied": numba_compatible,
            },
            "laion_clap": {
                "version": pypi["laion-clap"]["version"],
                "numpy_requirement": laion_requirement,
                "satisfied": laion_compatible,
            },
        },
        "indexes": indexes,
        "planned_packages": {
            package: {
                "published": bool(aur[package].get("available")),
                "disposition": "consume if compatible" if aur[package].get("available") else "to package",
            }
            for package in PLANNED_PACKAGES
        },
    }


def collect_indexes() -> dict[str, dict[str, object]]:
    arch = {package: _arch_package(package) for package in ARCH_PACKAGES}
    numba_version = str(arch["python-numba"]["version"])
    pypi = {
        "numba": _pypi_project("numba", numba_version),
        "laion-clap": _pypi_project("laion-clap"),
    }
    aur_names = (*AUR_RUNTIME_DEPENDENCIES, *PLANNED_PACKAGES)
    aur = {package: _aur_package(package) for package in aur_names}
    return {"arch": arch, "pypi": pypi, "aur": aur}


def _human_report(report: dict[str, object]) -> str:
    compatibility = report["compatibility"]
    assert isinstance(compatibility, dict)
    numba = compatibility["numba"]
    laion = compatibility["laion_clap"]
    assert isinstance(numba, dict) and isinstance(laion, dict)
    lines = [
        f"Arch CLAP provider gate: {str(report['status']).upper()}",
        f"  Arch NumPy: {compatibility['arch_numpy']}",
        f"  Numba {numba['version']}: numpy{numba['numpy_requirement']} "
        f"({'ok' if numba['satisfied'] else 'blocked'})",
        f"  LAION-CLAP {laion['version']}: numpy{laion['numpy_requirement']} "
        f"({'ok' if laion['satisfied'] else 'blocked'})",
    ]
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
        report = evaluate_gate(collect_indexes())
    except (PackageIndexError, KeyError, TypeError, ValueError) as exc:
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
