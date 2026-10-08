#!/usr/bin/env python3
"""Render and check scoped Han source notices and installed license copies."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "tools/fold-han-source-terms.json"
HAN = ROOT / "src/search/fold/tables/han"
UNIHAN = ROOT / "src/search/fold/unihan"
MODIFIED_NOTICE = "# Simplified mappings attested by OpenCC are selected and transformed from OpenCC TSCharacters.txt."
LICENSES = {"unihan": "Unicode-3.0", "cns": "OGDL-Taiwan-1.0", "opencc": "Apache-2.0"}
PIN_NAMES = {"unihan": {"unihan"}, "cns": {"cns-properties", "cns-mappings"}, "opencc": {"opencc"}}
ATTRIBUTES = {
    "unihan": {"title", "version", "copyright", "trademark", "terms_url"},
    "cns": {"agency", "year", "title", "version", "statement", "license_url", "website_credit"},
    "opencc": {"title", "version", "commit", "source_header"},
}
INTRO = """Han source attribution

The attribution records scope each source to the listed table files. Empty scopes
attribute no distributed table. Third-party-derived data retains its source
terms and is outside the project's Unlicense dedication. Source terms do not
establish a distribution license or compatibility for an assembled corpus.
Per-value provenance distinguishes predictions, agreement and transcription;
those labels do not determine public-domain status or remove source obligations.
Decomposition inputs, prompts and raw responses require their own source terms.

# schema: fold-han-notice/1
"""


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON key: {name}")
        result[name] = value
    return result


def loads(text):
    return json.loads(text, object_pairs_hook=unique_object)


def exact_keys(value, expected, label):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise ValueError(f"{label}: missing or unexpected fields")


def regular_file(root, name):
    if not isinstance(name, str):
        raise ValueError("notice asset path must be text")
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or not path.parts or any(p in {".", ".."} for p in name.split("/")):
        raise ValueError(f"unsafe notice asset path: {name}")
    target = root
    for part in path.parts:
        target /= part
        if target.is_symlink():
            raise ValueError(f"symlinked notice asset: {name}")
    if not target.is_file():
        raise ValueError(f"notice asset missing: {name}")
    return target


def read_registry(path=None):
    registry = loads((path or REGISTRY).read_text(encoding="utf-8"))
    exact_keys(registry, {"schema", "sources", "files"}, "source terms")
    if registry["schema"] != "fold-han-source-terms/1":
        raise ValueError("unsupported source terms schema")
    if not isinstance(registry["files"], dict) or not registry["files"]:
        raise ValueError("source terms license copies missing")
    exact_keys(registry["sources"], LICENSES, "source terms sources")
    for name, source in registry["sources"].items():
        required = {"pins", "license", "attribution", "license_file", "policy_files", "witnesses"}
        required |= {"route", "year_basis"} if name == "cns" else {"provider", "retained_notices"} if name == "opencc" else {"provider"}
        exact_keys(source, required, name + " source terms")
        exact_keys(source["pins"], PIN_NAMES[name], name + " pins")
        if source["license"] != LICENSES[name]:
            raise ValueError(f"{name} license must be {LICENSES[name]}")
        for pin in source["pins"].values():
            exact_keys(pin, {"source", "revision", "sha256"}, name + " pin")
            if any(not isinstance(v, str) or not v.strip() for v in pin.values()) or not re.fullmatch("[a-f0-9]{64}", pin["sha256"]):
                raise ValueError(name + " invalid source pin")
        if name != "cns" and (not isinstance(source["provider"], str) or not source["provider"].strip()):
            raise ValueError(name + " provider missing")
        a = source["attribution"]
        exact_keys(a, ATTRIBUTES[name], name + " attribution")
        for field, value in a.items():
            if field == "year":
                valid = type(value) is int and 1000 <= value <= 9999
            elif field == "source_header":
                valid = isinstance(value, list) and bool(value) and all(isinstance(v, str) and v.strip() for v in value)
            else:
                valid = isinstance(value, str) and bool(value.strip())
            if not valid:
                raise ValueError(f"{name} attribution {field} missing or invalid")
        for field in ("policy_files", "witnesses"):
            if not isinstance(source[field], list) or any(not isinstance(v, str) or not v for v in source[field]):
                raise ValueError(f"{name} invalid {field}")
        if name != "cns" and not isinstance(source["license_file"], str):
            raise ValueError(f"{name} required license copy missing")
        if name == "cns" and any(not isinstance(source[f], str) or not source[f].strip() for f in ("route", "year_basis")):
            raise ValueError("CNS route or year basis missing")
        if name == "opencc" and (a["commit"] != source["pins"]["opencc"]["revision"] or source["retained_notices"] != []):
            raise ValueError("OpenCC commit or retained notices need source-term investigation")
        paths = source["policy_files"] + ([source["license_file"]] if source["license_file"] else [])
        for asset in paths:
            if asset not in registry["files"]:
                raise ValueError(f"{name} notice asset not registered: {asset}")
            required = "license" if asset == source["license_file"] else "project-policy"
            if registry["files"][asset]["obligation"] != required:
                raise ValueError(f"{name} incorrect notice copy obligation")
    for asset, info in registry["files"].items():
        exact_keys(info, {"sha256", "obligation"}, "notice asset")
        if not re.fullmatch("[a-f0-9]{64}", info["sha256"]) or info["obligation"] not in {"license", "project-policy"}:
            raise ValueError(f"invalid notice asset: {asset}")
    return registry


def resolve(pins, registry):
    if not isinstance(pins, dict) or ("schema" in pins and pins["schema"] != "fold-han-references/1"):
        raise ValueError("invalid source reference schema")
    sources = set()
    for key, pin in pins.items():
        if key == "schema":
            continue
        found = [name for name, source in registry["sources"].items() if source["pins"].get(key) == pin]
        if len(found) != 1:
            raise ValueError(f"{key}: source terms unresolved for exact pin")
        sources.add(found[0])
    for name in sources:
        if any(pins.get(key) != pin for key, pin in registry["sources"][name]["pins"].items()):
            raise ValueError(f"{name}: source terms require all component pins")
    if not sources:
        raise ValueError("source terms require input pins")
    return sources


def validate_assets(root, registry):
    for asset, info in registry["files"].items():
        try:
            path = regular_file(root, asset)
            if hashlib.sha256(path.read_bytes()).hexdigest() != info["sha256"]:
                raise ValueError(f"notice asset hash differs: {asset}")
        except (OSError, ValueError) as exc:
            raise ValueError(f"{info['obligation']} copy: {exc}") from exc


def scope_inventory(registry, han=HAN, unihan=UNIHAN):
    scopes = {name: [] for name in registry["sources"]}
    paths = [(path, path.name) for path in sorted(han.glob("*.tsv"))]
    paths += [(unihan / name, "unihan/" + name) for name in ("han_pinyin.tsv", "han_simplified.tsv")]
    for path, label in paths:
        headers = [line.removeprefix("# references: ") for line in path.read_text(encoding="utf-8").splitlines() if line.startswith("# references: ")]
        if not headers:
            if label.startswith("unihan/"):
                raise ValueError(f"{label}: source pin header missing")
            continue
        if len(headers) != 1:
            raise ValueError(f"{label}: duplicate reference header")
        for name in resolve(loads(headers[0]), registry):
            scopes[name].append(label)
    return {name: sorted(files) for name, files in scopes.items()}


def render_notice(registry, scopes):
    scopes = {name: sorted(files) for name, files in scopes.items()}
    text = INTRO
    for name, source in registry["sources"].items():
        a = source["attribution"]
        provider = a["agency"] if name == "cns" else source["provider"]
        entry = {"reference": name, "provider": provider, "license": source["license"], "pins": source["pins"], "files": scopes[name]}
        text += "# attribution: " + canonical(entry) + "\n"
    for name, source in registry["sources"].items():
        a = source["attribution"]
        text += "\n" + a["title"] + "\n"
        text += "  Scoped files: " + (", ".join(scopes[name]) or "none") + ".\n"
        text += "  License: " + source["license"] + ".\n"
        if name == "unihan":
            lines = ["Version: " + a["version"], a["copyright"], a["trademark"], "Terms of use: " + a["terms_url"]]
        elif name == "cns":
            lines = [f"{a['agency']} [{a['year']}] [{a['title']}, 版本 {a['version']}]", a["statement"],
                     "政府資料開放授權條款：" + a["license_url"], "Source credit: " + a["website_credit"],
                     "Components: Properties.zip and MapingTables.zip, version " + a["version"] + ".",
                     source["route"], "Attribution year: " + source["year_basis"]]
        else:
            lines = ["Commit: " + a["commit"], *a["source_header"], "No upstream NOTICE exists at this pinned commit."]
        text += "".join("  " + line + "\n" for line in lines)
        if source["license_file"]:
            text += "  Required license copy: " + source["license_file"] + ".\n"
        for path in source["policy_files"]:
            text += "  Project policy offline copy (not a license-required copy): " + path + ".\n"
    return text


def notice_scopes(text, registry):
    scopes = {}
    for line in text.splitlines():
        if not line.startswith("# attribution: "):
            continue
        entry = loads(line.removeprefix("# attribution: "))
        exact_keys(entry, {"reference", "provider", "license", "pins", "files"}, "NOTICE attribution")
        name = entry["reference"]
        if name not in registry["sources"] or name in scopes:
            raise ValueError("invalid or duplicate NOTICE reference")
        files = entry["files"]
        if not isinstance(files, list) or any(not isinstance(f, str) or not re.fullmatch(r"(?:unihan/)?[a-z][a-z0-9_-]*\.tsv", f) for f in files) or len(files) != len(set(files)):
            raise ValueError("invalid NOTICE table scope")
        scopes[name] = sorted(files)
    exact_keys(scopes, registry["sources"], "NOTICE sources")
    if text != render_notice(registry, scopes):
        raise ValueError("NOTICE attribution differs from source terms; regenerate NOTICE")
    return scopes


def transformed_opencc(records):
    return any("opencc" in {s.split(":", 1)[0] for s in r.attested_by.split(",")} for r in records)


def check_modified_file(path, records):
    if (MODIFIED_NOTICE in path.read_text(encoding="utf-8").splitlines()) != transformed_opencc(records):
        raise ValueError(f"{path.name}: OpenCC modified-file notice must match transformed content")


def check_table(table, records, notice, pins, registry=None):
    registry = registry or read_registry()
    sources = resolve(pins, registry)
    scopes = notice_scopes(notice.read_text(encoding="utf-8"), registry)
    for name in sorted(sources):
        if table.name not in scopes[name]:
            raise ValueError(f"{name}: NOTICE attribution outside table scope: {table.name}")
    validate_assets(notice.parent, registry)
    check_modified_file(table, records)


def check_source(registry, han=HAN, unihan=UNIHAN, package=None):
    scopes = scope_inventory(registry, han, unihan)
    root = package or han
    text = regular_file(root, "NOTICE").read_text(encoding="utf-8")
    if text != render_notice(registry, scopes):
        raise ValueError("NOTICE differs from the source table inventory")
    validate_assets(root, registry)
    if package:
        if regular_file(package, "UNLICENSE").read_bytes() != (ROOT / "UNLICENSE").read_bytes():
            raise ValueError("packaged UNLICENSE differs")
    elif (unihan / "UNICODE-LICENSE.txt").read_bytes() != regular_file(han, "licenses/UNICODE-3.0.txt").read_bytes():
        raise ValueError("Unihan license copy differs from the registered Unicode notice")
    return scopes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("render", "check", "assets"))
    parser.add_argument("--write", action="store_true", help="write the generated source NOTICE")
    parser.add_argument("--package", type=pathlib.Path, help="check a staged share/licenses/muzaiten directory")
    args = parser.parse_args(argv)
    try:
        registry = read_registry()
        if args.command == "render":
            text = render_notice(registry, scope_inventory(registry))
            if args.write:
                (HAN / "NOTICE").write_text(text, encoding="utf-8", newline="\n")
            else:
                print(text, end="")
        elif args.command == "assets":
            check_source(registry)
            print("NOTICE")
            print("\n".join(registry["files"]))
        else:
            check_source(registry, package=args.package)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
