#!/usr/bin/env python3
"""Assemble and audit rare Han proposals from pinned references and blind logs."""

from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import ipaddress
import itertools
import json
import pathlib
import re
import sys
import unicodedata
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
from datetime import date
from urllib.parse import urlsplit


def load_checker():
    if "fold_han_data" not in sys.modules:
        spec = importlib.util.spec_from_file_location("fold_han_data", pathlib.Path(__file__).with_name("fold-han-data.py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    return sys.modules["fold_han_data"]


data = load_checker()


def load_notice():
    if "fold_han_notice" not in sys.modules:
        spec = importlib.util.spec_from_file_location("fold_han_notice", pathlib.Path(__file__).with_name("fold-han-notice.py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    return sys.modules["fold_han_notice"]


notice_terms = load_notice()
PREDICTION_SCHEMA = "fold-han-predictions/2"
PROVENANCE_SCHEMA = "fold-han-provenance/1"
REFERENCE_SCHEMA = "fold-han-references/1"
PREDICTION_COLUMNS = "ideograph readings confidence sound_component stated_basis decomposition_sha256 hint normalized_readings simplified".split()
PROVENANCE_COLUMNS = "ideograph value basis attested_by tone_from rank batch".split()
DIRECT_FIELDS = (*data.VARIANT_FIELDS, "kZVariant")
PARTIAL_FIELDS = ("kSemanticVariant", "kSpecializedSemanticVariant")
# These written forms retain separate keys despite their source relations.
EXCLUDED_PAIRS = {frozenset(("線", "缐"))}
RULES = pathlib.Path(__file__).with_name("fold-han-reviewed-rules.json")
TYPED_SHA256 = "843a97bc8bcab86a9295a29074cd074863fe877d0f7d4648931d49a9e7674fa7"
MAINLAND_FIELDS = ("kHanyuPinyin", "kXHC1983", "kTGHZ2013")
FIELDS = (*data.READING_FIELDS, *DIRECT_FIELDS, *PARTIAL_FIELDS, "kTGH", "kUnihanCore2020")
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+#:-]*\Z")
SESSION_ID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", re.I)
PATH_TOKEN = re.compile(r"(?:[A-Za-z]:)?(?:[.~]?[/\\]|[A-Za-z0-9_.~-]+[/\\])\S*")


def canonical(value) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def keys(value: dict, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise ValueError(f"{label}: expected keys {', '.join(sorted(expected))}")


def coordinate(value: str, source: bool = False) -> None:
    if not isinstance(value, str) or not value or re.search(r"\s|[\x00-\x1f]", value) or SESSION_ID.search(value):
        raise ValueError("invalid portable source coordinate")
    if source:
        url = urlsplit(value)
        if url.scheme == "fixture" and TOKEN.fullmatch(value):
            return
        if url.scheme not in {"https", "http"} or not url.hostname or url.username or url.password:
            raise ValueError("source must be a public HTTP URL or fixture identifier")
        host = url.hostname.lower().rstrip(".")
        try:
            public = ipaddress.ip_address(host).is_global
        except ValueError:
            public = ("." in host
                      and not re.fullmatch(r"(?:0x[0-9a-f]+|[0-9]+)(?:\.(?:0x[0-9a-f]+|[0-9]+))*", host)
                      and not host.endswith((".localhost", ".local", ".internal", ".lan", ".home", ".home.arpa"))
                      and host != "localhost")
        if not public:
            raise ValueError("source must name a public host")
    elif not TOKEN.fullmatch(value) or ".." in value:
        raise ValueError("coordinate must be a portable identifier")


def portable_text(value: str) -> None:
    if PATH_TOKEN.search(value) or SESSION_ID.search(value):
        raise ValueError("descriptive text must not contain paths or session identifiers")


def source_pin(value: dict) -> None:
    keys(value, {"source", "revision", "sha256"}, "source pin")
    coordinate(value["source"], source=True)
    coordinate(value["revision"])
    if not isinstance(value["sha256"], str) or not SHA256.fullmatch(value["sha256"]):
        raise ValueError("source pin: invalid sha256")


def validate_pins(pins: dict) -> None:
    keys(pins, {"schema", "unihan", "cns-properties", "cns-mappings", "opencc"}, "references")
    if pins["schema"] != REFERENCE_SCHEMA:
        raise ValueError("unsupported reference schema")
    for name in ("unihan", "cns-properties", "cns-mappings", "opencc"):
        source_pin(pins[name])


def shipped_pins() -> dict:
    return {
        "schema": REFERENCE_SCHEMA,
        "unihan": {"source": data.UNIHAN_URL, "revision": data.UNIHAN_VERSION, "sha256": data.UNIHAN_SHA256},
        **{f"cns-{kind}": {"source": data.CNS_URL + name, "revision": "2026-08-05", "sha256": data.CNS_SHA256[name]}
           for kind, name in (("properties", "Properties.zip"), ("mappings", "MapingTables.zip"))},
        "opencc": {"source": data.OPENCC_URL, "revision": data.OPENCC_URL.split("/")[5], "sha256": data.OPENCC_SHA256},
    }


def ideograph(value: str) -> None:
    if len(value) != 1 or not data.is_unihan_ideograph(value):
        raise ValueError("expected one CJK ideograph")


def nfc(value: str) -> str:
    return unicodedata.normalize("NFC", value)


def syllable(value: str) -> str:
    """Remove tones, preserving the diaeresis and circumflex vowel distinctions."""
    return nfc("".join(ch for ch in unicodedata.normalize("NFD", value) if ch not in data.TONE_MARKS))


def reading(value: str) -> str:
    base = data.pinyin_base(value)
    if not base or not data.is_tone_marked_pinyin(value, {base}, {nfc(value)}):
        raise ValueError(f"invalid pinyin reading: {value}")
    return nfc(value)


@functools.cache
def reference_reading(value: str) -> str | None:
    """Return a reference spelling as NFC pinyin, or None when it is not pinyin."""
    try:
        return reading(value)
    except ValueError:
        return None


def endpoint(value: str) -> str | None:
    """Return the unified ideograph a relation names, or None outside the ideograph blocks.

    NFC maps each compatibility ideograph to its unified counterpart."""
    value = nfc(value)
    return value if len(value) == 1 and data.in_reference_ideograph_blocks(value) else None


def links(pairs):
    """Yield canonical (source, target) relations between distinct ideographs."""
    for ch, targets in pairs:
        source = endpoint(ch)
        for target in map(endpoint, targets):
            if source and target and source != target:
                yield source, target


def unihan_links(fields: dict[str, dict[str, str]], field: str):
    return links((ch, data.variant_codes(raw)) for ch, raw in fields.get(field, {}).items())


def document(path: pathlib.Path) -> tuple[dict[str, object], list[tuple[int, list[str]]]]:
    headers = {}
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("# "):
            if line == "# complete":
                headers["complete"] = True
                continue
            key, separator, value = line[2:].partition(": ")
            if key in {"schema", "metadata", "references", "inputs", "authored", "counts", "columns"} and separator:
                if key in headers:
                    raise ValueError(f"{path.name}:{number}: duplicate {key} header")
                headers[key] = json.loads(value) if key not in {"schema", "columns"} else value
            continue
        if line.lstrip().startswith("#"):
            continue
        rows.append((number, line.split("\t")))
    return headers, rows


@dataclass(frozen=True)
class Prediction:
    ch: str
    raw: tuple[str, ...]
    normalized: tuple[str, ...]
    simplified: str
    batch: str
    decomposition_sha256: str


@dataclass
class Predictions:
    rows: dict[str, list[Prediction]]
    inputs: list[dict[str, str]]


def read_predictions(paths: list[pathlib.Path]) -> Predictions:
    rows = defaultdict(list)
    inputs = []
    batches = set()
    for path in paths:
        headers, lines = document(path)
        if headers.get("schema") != PREDICTION_SCHEMA or headers.get("columns") != "\t".join(PREDICTION_COLUMNS):
            raise ValueError(f"{path.name}: missing prediction schema or columns")
        meta = headers.get("metadata")
        keys(meta, {"batch", "model", "effort", "date", "prompt_sha256", "decomposition", "hint"}, "prediction metadata")
        for key in ("batch", "model", "effort"):
            coordinate(meta[key])
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", meta["batch"]):
            raise ValueError("batch must be a filename-safe identifier")
        date.fromisoformat(meta["date"])
        if not SHA256.fullmatch(meta["prompt_sha256"]):
            raise ValueError("invalid prompt sha256")
        decomposition = meta["decomposition"]
        keys(decomposition, {"id", "source", "revision", "sha256"}, "decomposition pointer")
        coordinate(decomposition["id"])
        source_pin({key: value for key, value in decomposition.items() if key != "id"})
        source_pin(meta["hint"])
        batch = meta["batch"]
        if batch in batches:
            raise ValueError(f"duplicate prediction batch: {batch}")
        batches.add(batch)
        inputs.append({"batch": batch, "sha256": digest(path)})
        seen = set()
        for number, columns in lines:
            if len(columns) != len(PREDICTION_COLUMNS):
                raise ValueError(f"{path.name}:{number}: expected nine prediction columns")
            ch, raw, confidence, component, basis, decomposition_sha, hint, normalized, simple = columns
            ideograph(ch)
            if ch in seen:
                raise ValueError(f"{path.name}:{number}: duplicate ideograph")
            seen.add(ch)
            if confidence not in {"c", "g", "unknown"} or not basis:
                raise ValueError(f"{path.name}:{number}: invalid confidence or empty stated basis")
            for text in (component, basis, hint):
                portable_text(text)
            if any(any(ord(c) < 32 for c in text) for text in columns):
                raise ValueError(f"{path.name}:{number}: control character")
            if decomposition_sha != "-" and not SHA256.fullmatch(decomposition_sha):
                raise ValueError(f"{path.name}:{number}: expected decomposition sha256 or -")
            raw_tokens = () if raw == "-" else tuple(raw.split())
            forms = tuple(reading(token) for token in raw_tokens)
            if not raw or len(forms) != len(set(forms)) or normalized != (" ".join(forms) or "-"):
                raise ValueError(f"{path.name}:{number}: readings must be distinct with exact NFC audit forms")
            if simple:
                ideograph(simple)
                if simple == ch:
                    raise ValueError("predicted mapping must name another ideograph")
            rows[ch].append(Prediction(ch, raw_tokens, forms, simple, batch, decomposition_sha))
    return Predictions({ch: sorted(values, key=lambda p: p.batch) for ch, values in rows.items()}, sorted(inputs, key=lambda p: p["batch"]))


@dataclass(frozen=True)
class Record:
    ch: str
    value: str
    basis: str
    attested_by: str
    tone_from: str
    rank: int
    batch: str

    def columns(self) -> list[str]:
        return [self.ch, self.value, self.basis, self.attested_by, self.tone_from, str(self.rank), self.batch]


@dataclass
class References:
    fields: dict[str, dict[str, str]]
    cns: dict[str, list[str]]
    opencc: dict[str, list[str]]
    pins: dict
    typed: frozenset[str] | None = None
    retained_mappings: dict[str, str] | None = None

    def unihan_spellings(self, ch: str, fields: tuple[str, ...]) -> list[str]:
        return [raw for field in fields for raw in data.field_readings(field, self.fields.get(field, {}).get(ch, ""))]

    def spellings(self, ch: str) -> list[tuple[str, str]]:
        """Return (source, raw spelling) pairs; the source is a Unihan field or `cns`."""
        cns = self.cns.get(ch, ())
        if isinstance(cns, set):
            cns = sorted(cns)
        return ([(field, raw) for field in data.READING_FIELDS for raw in self.unihan_spellings(ch, (field,))]
                + [("cns", raw) for raw in cns])

    def attestations(self, ch: str) -> dict[str, set[str]]:
        result = defaultdict(set)
        for source, raw in self.spellings(ch):
            if value := reference_reading(raw):
                result[value].add("cns" if source == "cns" else "unihan")
        return dict(result)

    def unparseable(self, ch: str) -> list[tuple[str, str]]:
        return sorted({(source, raw) for source, raw in self.spellings(ch) if reference_reading(raw) is None})

    def mandarin(self, ch: str) -> list[str]:
        return [value for raw in self.unihan_spellings(ch, ("kMandarin",)) if (value := reference_reading(raw))]

    def mainland(self, ch: str) -> set[str]:
        """Return readings from mainland fields and the first kMandarin spelling."""
        raw = self.unihan_spellings(ch, MAINLAND_FIELDS) + self.unihan_spellings(ch, ("kMandarin",))[:1]
        return {value for spelling in raw if (value := reference_reading(spelling))}


def reviewed_rules(refs: References) -> dict:
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    if rules["schema"] != "fold-han-reviewed-rules/1" or rules["references"] != refs.pins:
        raise ValueError("reviewed rules reference pins differ")
    return rules


def typed_members(refs: References, authored: set[str]) -> frozenset[str]:
    if refs.typed is not None:
        return refs.typed
    if all(pin["source"].startswith("fixture:") for name, pin in refs.pins.items() if name != "schema"):
        return frozenset(authored | set(refs.fields.get("kTGH", {})))
    pin = reviewed_rules(refs)["typed"]
    path = RULES.with_name(pin["file"])
    if pin["sha256"] != TYPED_SHA256 or digest(path) != TYPED_SHA256:
        raise ValueError("frozen typed membership checksum differs")
    members = path.read_text(encoding="utf-8").splitlines()
    if len(members) != pin["count"] or members != sorted(set(members)):
        raise ValueError("invalid frozen typed membership")
    return frozenset(members)


def retained_mapping_choices(refs: References) -> dict[str, str]:
    if refs.retained_mappings is not None:
        return refs.retained_mappings
    if not all(pin["source"].startswith("fixture:") for name, pin in refs.pins.items() if name != "schema"):
        return reviewed_rules(refs)["retained_mappings"]
    return {}


def classify(ch: str, value: str, lineages: set[str], predictions: Predictions) -> Record:
    candidates = [(p.batch, i, token) for p in predictions.rows.get(ch, ()) for i, token in enumerate(p.normalized)]
    exact = [p for p in candidates if p[2] == value]
    matches = exact or [p for p in candidates if syllable(p[2]) == syllable(value)]
    sources = ",".join(sorted(lineages))
    if matches:
        return Record(ch, value, "predicted", sources, "-" if exact else sources, 1, min(matches)[0])
    return Record(ch, value, "agreed" if len(lineages) == 2 else "single", sources, sources, 1, "-")


def ordered_readings(ch: str, refs: References, predictions: Predictions,
                     choice: list[str] | None = None, tier: str | None = None
                     ) -> tuple[list[Record], list[tuple[str, str]]]:
    """Prefer the decisive tgh reading, then region and evidence precedence.

    Reviewed permutations reorder only tied readings; their decision finding remains."""
    attestations = refs.attestations(ch)
    records = {value: classify(ch, value, sources, predictions) for value, sources in attestations.items()}
    mandarin = refs.mandarin(ch)
    candidates = [(p.batch, i, token) for p in predictions.rows.get(ch, ()) for i, token in enumerate(p.normalized)]
    mainland = refs.mainland(ch)
    tgh = [value for raw in refs.unihan_spellings(ch, ("kTGHZ2013",))
           if (value := reference_reading(raw))] if tier == "tgh" else []
    decisive = tgh[0] if tgh else None

    def precedence(value: str):
        record = records[value]
        group = 0 if len(attestations[value]) == 2 else 1 if record.basis == "predicted" else 2
        model = min(((batch, i) for batch, i, token in candidates if syllable(token) == syllable(value)), default=("~", sys.maxsize))
        return value != decisive, value not in mainland, group, mandarin.index(value) if value in mandarin else sys.maxsize, model

    placed = sorted(records, key=lambda value: (precedence(value), value))
    undecided = None
    cns_only = all(attested == {"cns"} for attested in attestations.values())
    if len(placed) > 1 and cns_only and not any(r.basis == "predicted" for r in records.values()):
        undecided = "CNS-only default requires a reviewed decision"
    elif len(placed) > 1 and precedence(placed[0]) == precedence(placed[1]):
        undecided = "evidence-order tie requires a reviewed decision"
    if (undecided and choice and sorted(choice) == sorted(placed)
            and all(precedence(a) <= precedence(b) for a, b in itertools.pairwise(choice))):
        placed = list(choice)
    issues = [(" ".join(placed), undecided)] if undecided else []
    issues.extend((raw, f"unparseable {source} reading") for source, raw in refs.unparseable(ch))
    for rank, value in enumerate(placed, 1):
        r = records[value]
        records[value] = Record(r.ch, r.value, r.basis, r.attested_by, r.tone_from, rank, r.batch)
    return [records[value] for value in placed], issues


def family_mappings(refs: References, authored: set[str], choices: dict[str, str] | None = None
                    ) -> tuple[dict[str, tuple[str, str]], dict[str, list[str]], dict[str, list[tuple[str, str]]]]:
    """Resolve direct families, then join guarded partial links ranked by independent lineages.

    Returns each joined member's target and relation path, blocking issues, and the
    partial links the guard rejects, which join nothing."""
    choices = {**retained_mapping_choices(refs), **(choices or {})}
    graph = defaultdict(dict)
    simplified = set()
    general = set(refs.fields.get("kTGH", {}))
    issues = defaultdict(list)
    rejected = defaultdict(list)

    def edge(a: str, b: str, label: str):
        graph[a].setdefault(b, set()).add(label)
        graph[b].setdefault(a, set()).add(label)

    def components():
        """Yield each connected family, ordered by its least member."""
        visited = set()
        for start in sorted(graph):
            if start in visited:
                continue
            component = set()
            queue = [start]
            while queue:
                ch = queue.pop()
                if ch not in component:
                    component.add(ch)
                    queue.extend(graph[ch])
            visited.update(component)
            yield component

    def resolve(component: set[str]) -> tuple[str | None, set[str]]:
        """Return the family target and the candidates of the first applicable rule.

        Only a family without BMP or General Standard members uses its unique
        directly named simplified member as an automatic fallback."""
        bmp = {ch for ch in component if ord(ch) < 0x10000}
        rule = bmp & simplified or component & general or bmp
        candidates = rule or component & simplified
        return (next(iter(candidates)) if len(candidates) == 1 else None), rule

    def family_choice(component: set[str], rule: set[str]) -> str | None:
        requested = {choices[ch] for ch in component if ch in (choices or {})}
        if len(requested) == 1 and requested <= (rule or component):
            return next(iter(requested))
        return None

    for field in DIRECT_FIELDS:
        for ch, target in unihan_links(refs.fields, field):
            if frozenset((ch, target)) in EXCLUDED_PAIRS:
                rejected[ch].append((target, "policy-excluded:" + field))
                continue
            edge(ch, target, "unihan:" + field)
            if field == "kSimplifiedVariant":
                simplified.add(target)
    for ch, target in links(refs.opencc.items()):
        if frozenset((ch, target)) in EXCLUDED_PAIRS:
            rejected[ch].append((target, "policy-excluded:opencc:TSCharacters"))
            continue
        edge(ch, target, "opencc:TSCharacters")
        simplified.add(target)
    family = {}
    for component in components():
        target, rule = resolve(component)
        if target is None:
            target = family_choice(component, rule)
        for ch in component:
            family[ch] = min(component), target

    typed = typed_members(refs, authored)
    partial = defaultdict(dict)
    for field in PARTIAL_FIELDS:
        for ch, target in unihan_links(refs.fields, field):
            if frozenset((ch, target)) in EXCLUDED_PAIRS:
                rejected[ch].append((target, "policy-excluded:" + field))
                continue
            if target in graph.get(ch, {}):
                continue
            if (ch not in typed and target in typed
                    and {syllable(r) for r in refs.attestations(ch)} & {syllable(r) for r in refs.attestations(target)}):
                partial[ch].setdefault(target, set()).add("unihan:" + field)
            else:
                rejected[ch].append((target, field))

    def lineage_syllables(member: str, lineage: str) -> set[str]:
        return {syllable(value) for value, sources in refs.attestations(member).items() if lineage in sources}

    for ch, targets in partial.items():
        scores = {target: sum(bool(lineage_syllables(ch, lineage) & lineage_syllables(target, lineage))
                              for lineage in ("unihan", "cns")) for target in targets}
        maximum = max(scores.values())
        best = sorted(target for target, score in scores.items() if score == maximum)
        joined = {target: family.get(target, (target, target)) for target in best}
        if len({key for key, _ in joined.values()}) > 1:
            issues[ch].append("variant-guard:tied-lineages=" + " ".join(
                f"{target}:{maximum}" if joined[target][1] == target else f"{target}→{joined[target][1] or '-'}:{maximum}"
                for target in best))
            choice = (choices or {}).get(ch)
            best = [target for target in best if choice and joined[target][1] == choice]
        for target in best:
            for label in targets[target]:
                edge(ch, target, label)

    result = {}
    for component in components():
        target, rule = resolve(component)
        if target is None:
            for ch in component:
                issues[ch].append("family-target:unresolved=" + " ".join(sorted(rule or component)))
            target = family_choice(component, rule)
            if target is None:
                continue
        queue = deque([(target, ())])
        reached = {target}
        while queue:
            ch, relations = queue.popleft()
            for neighbor in sorted(graph[ch]):
                if neighbor in reached:
                    continue
                reached.add(neighbor)
                path = tuple(sorted(set(relations) | graph[ch][neighbor]))
                result[neighbor] = (target, ",".join(path))
                queue.append((neighbor, path))
    return result, dict(issues), dict(rejected)


def assemble(members: list[str], refs: References, predictions: Predictions, authored: set[str],
             mapping_choices: dict[str, str] | None = None, reading_choices: dict[str, list[str]] | None = None,
             tier: str | None = None):
    if len(members) != len(set(members)):
        raise ValueError("duplicate tier member")
    for ch in members:
        if len(ch) != 1 or not data.in_ideograph_blocks(ch):
            ideograph(ch)
    outside = set(predictions.rows) - set(members)
    if outside:
        raise ValueError("predictions outside tier: " + " ".join(sorted(outside)))
    mappings, family_issues, rejected = family_mappings(refs, authored, mapping_choices)
    rows, records, gaps, findings = [], [], [], []
    for ch in members:
        if not data.is_unihan_ideograph(ch):
            gaps.append(member_gap(ch))
            continue
        readings, issues = ordered_readings(ch, refs, predictions, (reading_choices or {}).get(ch), tier)
        text = " ".join(r.value for r in readings) or "-"
        target, relations = mappings.get(ch, ("", ""))
        if target and not data.is_unihan_ideograph(target):
            gaps.append((ch, target, target, "-", "unsupported-target:" + relations))
            target = ""
        rows.append((ch, text, target))
        records.extend(readings)
        if target:
            blind = [p.batch for p in predictions.rows.get(ch, ()) if p.simplified == target]
            records.append(Record(ch, target, "predicted" if blind else "single", relations, "-", 0, min(blind) if blind else "-"))
        findings.extend((ch, "provenance", ours, reference) for ours, reference in issues)
        findings.extend((ch, issue.split(":", 1)[0], target or "-", issue) for issue in family_issues.get(ch, ()))
        available = {syllable(r.value) for r in readings}
        for prediction in predictions.rows.get(ch, ()):
            for raw, normalized in zip(prediction.raw, prediction.normalized):
                if syllable(normalized) not in available:
                    gaps.append((ch, raw, normalized, prediction.batch, "unattested-syllable"))
            if prediction.simplified and prediction.simplified != target:
                gaps.append((ch, prediction.simplified, prediction.simplified, prediction.batch, "unconfirmed-mapping"))
        gaps.extend((ch, link, link, "-", field if field.startswith("policy-excluded:") else "guard-failed:" + field)
                    for link, field in rejected.get(ch, ()))
        passes = {p.normalized for p in predictions.rows.get(ch, ())}
        if len(passes) > 1:
            findings.append((ch, "provenance", text, "prediction-pass disagreement"))
    return rows, records, gaps, findings


def build_tiers(refs: References) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """Return shared build-supported membership and witnessed unsupported candidates."""
    tiers = data.tiers(refs.fields, refs.cns)
    return tiers, tiers.withheld


def member_gap(ch: str) -> tuple[str, str, str, str, str]:
    return ch, ch, ch, "-", f"unsupported-member:UCD-{unicodedata.unidata_version}:category={unicodedata.category(ch)}"


def read_decisions(path: pathlib.Path, members: list[str]):
    """Extract choices for selected members from exact five-column reviewed rows."""
    mappings, readings, expected = {}, {}, set()
    selected = set(members)
    for number, columns in document(path)[1]:
        if len(columns) != 5 or not columns[4].strip():
            raise ValueError(f"{path.name}:{number}: expected five reviewed columns with a reason")
        ch, kind, ours, reference, _ = columns
        if ch not in selected:
            continue
        if kind in {"family-target", "variant-guard"}:
            ideograph(ours)
            choices, value = mappings, ours
        elif kind == "provenance":
            value = ours.split()
            if not value or len(value) != len(set(value)) or any(reading(r) != r for r in value):
                raise ValueError("decision readings must be distinct NFC pinyin")
            choices = readings
        else:
            continue
        if ch in choices and choices[ch] != value:
            raise ValueError(f"conflicting decisions for {ch}")
        choices[ch] = value
        expected.add((ch, kind, ours, reference))
    return mappings, readings, expected


def authored_before(directory: pathlib.Path, tier: str) -> set[str]:
    """Only preceding tiers establish typed targets for a rare-tier proposal."""
    names = [path.removesuffix(".tsv") for path in data.TIER_FILES]
    if tier not in names:
        raise ValueError("unknown authored tier")
    result = set()
    for name in names[:names.index(tier)]:
        path = directory / (name + ".tsv")
        if path.is_file():
            for _, columns in document(path)[1]:
                if len(columns) != 3:
                    raise ValueError("invalid preceding authored row")
                ideograph(columns[0])
                result.add(columns[0])
    return result


def authored_pin(authored: set[str]) -> dict:
    sha = hashlib.sha256("".join(ch + "\n" for ch in sorted(authored)).encode("utf-8")).hexdigest()
    return {"source": "preceding-authored-tier-membership", "revision": sha, "sha256": sha}


def header(schema: str, refs: References, predictions: Predictions, authored: set[str]) -> str:
    return (f"# schema: {schema}\n# references: {canonical(refs.pins)}\n# inputs: {canonical(predictions.inputs)}\n"
            + f"# authored: {canonical(authored_pin(authored))}\n"
            + f"# Python Unicode data: {unicodedata.unidata_version}.\n")


def table_rows(path: pathlib.Path) -> dict[str, tuple[list[str], str]]:
    rows = {}
    for number, columns in document(path)[1]:
        if len(columns) != 3:
            raise ValueError(f"{path.name}:{number}: expected three table columns")
        ch, text, target = columns
        ideograph(ch)
        if target:
            ideograph(target)
        if ch in rows:
            raise ValueError(f"{path.name}:{number}: duplicate table member")
        values = [] if text == "-" else [reading(r) for r in text.split()]
        if not text or (text != "-" and not values):
            raise ValueError(f"{path.name}:{number}: empty table readings")
        rows[ch] = list(dict.fromkeys(values)), target
    return rows


def table_state(authored: pathlib.Path, generated: pathlib.Path) -> dict[str, tuple[list[str], str]]:
    """Read effective search rows, including authored overrides of empty mappings."""
    rows = {}
    for name in ("han_pinyin.tsv", "han_simplified.tsv"):
        for number, columns in document(generated / name)[1]:
            if len(columns) != 2:
                raise ValueError(f"{name}:{number}: expected two generated columns")
            value, chars = columns
            for ch in chars:
                values, target = rows.get(ch, ([], ""))
                rows[ch] = ([data.toneless(value)], target) if name == "han_pinyin.tsv" else (values, value)
    for path in sorted(authored.glob("*.tsv")):
        if path.name != "words.tsv":
            for ch, row in table_rows(path).items():
                values, target = row
                rows[ch] = list(dict.fromkeys(data.toneless(r) for r in values)), target
    return rows


def terminal_keys(rows: dict[str, tuple[list[str], str]]) -> dict[str, str | None]:
    """Resolve mapping chains; a cycle and every path into it have no terminal key."""
    mapping = {ch: target for ch, (_, target) in rows.items() if target}
    keys = {}
    for start in sorted(set(rows) | set(mapping.values())):
        path, visited = [], set()
        ch = start
        while ch not in keys and ch not in visited and ch in mapping:
            visited.add(ch)
            path.append(ch)
            ch = mapping[ch]
        end = keys[ch] if ch in keys else None if ch in visited else ch
        keys[start] = end
        for member in path:
            keys[member] = end
    return keys


def expected_changes(baseline, proposed, members):
    """Compare toneless readings and terminal keys at the search table boundary."""
    before, after = terminal_keys(baseline), terminal_keys(proposed)
    result = []
    for ch in sorted(members):
        old = list(dict.fromkeys(data.toneless(r) for r in baseline.get(ch, ([], ""))[0]))
        new = list(dict.fromkeys(data.toneless(r) for r in proposed.get(ch, ([], ""))[0]))
        if old and new and old[0] != new[0]:
            result.append((ch, "default-changed", old[0], new[0]))
        result.extend((ch, "reading-added", "-", value) for value in sorted(set(new) - set(old)))
        result.extend((ch, "reading-removed", value, "-") for value in sorted(set(old) - set(new)))
        old_key, new_key = before.get(ch, ch), after.get(ch, ch)
        if old_key != new_key:
            kind = "mapping-added" if old_key == ch else "mapping-removed" if new_key == ch else "mapping-retargeted"
            result.append((ch, kind, old_key or "cycle", new_key or "cycle"))
    return result


def joined_pairs(rows):
    keys = terminal_keys(rows)
    groups = defaultdict(list)
    for ch, key in sorted(keys.items()):
        if key is not None:
            groups[key].append(ch)
    return {pair for members in groups.values() for pair in itertools.combinations(members, 2)}, sorted(
        ch for ch, key in keys.items() if key is None)


def pull_forward(baseline, proposals):
    """Carry later rows to preserve final-retained baseline pairs and avoid cycles."""
    order = [name.removesuffix(".tsv") for name in data.RARE_FILES if name.removesuffix(".tsv") in proposals]
    if set(order) != set(proposals):
        raise ValueError("unknown report tier")
    home, final = {}, dict(baseline)
    for tier in order:
        for ch, row in proposals[tier].items():
            if ch in home:
                raise ValueError("duplicate assembly member across tiers")
            home[ch] = tier
            final[ch] = row
    base_pairs, base_cycles = joined_pairs(baseline)
    final_pairs, final_cycles = joined_pairs(final)
    permanent = base_pairs - final_pairs
    current, steps, pulls = dict(baseline), [], {}
    for index, tier in enumerate(order):
        current.update(proposals[tier])
        reasons = defaultdict(set)
        later = set(order[index + 1:])

        def pending_target(start):
            visited = set()
            ch = start
            while ch not in visited:
                visited.add(ch)
                target = current.get(ch, ([], ""))[1]
                if home.get(ch) in later and target != final[ch][1]:
                    return ch
                if not target:
                    break
                ch = target
            return None

        while True:
            pairs, cycles = joined_pairs(current)
            missing = base_pairs - pairs - permanent
            needed = defaultdict(set)
            for a, b in missing:
                for ch in (a, b):
                    if (pending := pending_target(ch)) is not None:
                        needed[pending].add("baseline-pair")
            for ch in cycles:
                needed[ch].add("cycle")
            needed = {ch: why for ch, why in needed.items()
                      if home.get(ch) in later and current.get(ch, ([], ""))[1] != final[ch][1]}
            if not needed:
                break
            for ch, why in needed.items():
                current[ch] = final[ch]
                reasons[ch].update(why)
        pulls[tier] = [(home[ch], ch, " ".join(final[ch][0]) or "-", final[ch][1], ",".join(sorted(why)))
                       for ch, why in sorted(reasons.items())]
        steps.append({"tier": tier, "pulled_forward": len(reasons),
                      "pulled_from": dict(sorted(Counter(home[ch] for ch in reasons).items())),
                      "remaining_splits": [list(pair) for pair in sorted(base_pairs - pairs)],
                      "unexpected_splits": [list(pair) for pair in sorted(missing)], "cycles": cycles})
    return {"baseline_cycles": base_cycles, "final_cycles": final_cycles,
            "permanent_splits": [list(pair) for pair in sorted(permanent)], "steps": steps}, pulls


def read_assembly(directory: pathlib.Path, refs: References):
    tiers, _ = build_tiers(refs)
    proposals = {}
    for name in data.RARE_FILES:
        path = directory / name
        if not path.is_file():
            path = directory / pathlib.Path(name).stem / name
        rows = table_rows(path)
        if set(rows) != set(tiers[path.stem]):
            raise ValueError(f"{name}: report assembly requires every buildable tier member")
        proposals[path.stem] = rows
    return proposals


def impact_reports(tier, rows, baseline, proposals=None):
    """Return reviewable impact artifacts without writing any authored table."""
    selected = {ch: (text.split() if text != "-" else [], target)
                for ch, text, target in rows}
    final = dict(baseline)
    if proposals is not None:
        proposals = {name: dict(values) for name, values in proposals.items()}
        proposals[tier].update(selected)
        for values in proposals.values():
            final.update(values)
        report, pulls = pull_forward(baseline, proposals)
        report["status"] = "evaluated"
        pulled = pulls[tier]
    else:
        final.update(selected)
        report, pulled = {"status": "not-evaluated", "reason": "provide --assembly for staged closure"}, []
    changes = expected_changes(baseline, final, selected)
    report.update({"schema": "fold-han-assembly-impact/1", "tier": tier,
                   "expected_changes": dict(sorted(Counter(row[1] for row in changes).items())),
                   "baseline_sha256": hashlib.sha256(canonical(baseline).encode()).hexdigest(),
                   "assembly_sha256": hashlib.sha256(canonical(final).encode()).hexdigest()})
    return changes, pulled, report


def write_assembly(out: pathlib.Path, tier: str, members: list[str], refs: References, predictions: Predictions,
                   authored: set[str], *, complete: bool = False,
                   mapping_choices: dict[str, str] | None = None,
                   reading_choices: dict[str, list[str]] | None = None,
                   decisions: pathlib.Path | None = None,
                   baseline: dict | None = None, report_assembly: dict | None = None):
    tiers, withheld = build_tiers(refs)
    if complete and {ch for ch in members if len(ch) == 1 and data.is_unihan_ideograph(ch)} != set(tiers[tier]):
        raise ValueError("complete assembly requires every tier member")
    expected_decisions = set()
    if decisions:
        if mapping_choices is not None or reading_choices is not None:
            raise ValueError("provide a decisions file or API choices, not both")
        mapping_choices, reading_choices, expected_decisions = read_decisions(decisions, members)
    rows, records, gaps, findings = assemble(members, refs, predictions, authored,
                                           mapping_choices, reading_choices, tier=tier)
    if expected_decisions - set(findings):
        raise ValueError("decision does not match an emitted choice finding")
    if complete:
        gaps.extend(member_gap(ch) for ch in withheld[tier] if ch not in members)
    impact = impact_reports(tier, rows, baseline, report_assembly) if baseline is not None else None
    counts = dict(sorted(Counter(r.basis for r in records if r.rank).items()))
    out.mkdir(parents=True, exist_ok=True)
    common = header(PROVENANCE_SCHEMA, refs, predictions, authored) + "# Source attributions and license texts: NOTICE.\n"
    modified = notice_terms.MODIFIED_NOTICE + "\n" if notice_terms.transformed_opencc(records) else ""
    files = {
        f"{tier}.tsv": common + modified + ("# complete\n" if complete else "# partial\n")
        + "# Mainland readings precede Taiwan-only readings; unresolved order choices require reviewed rows.\n"
        + "# Readings marked single are transcribed from the named source; see NOTICE.\n"
        + f"# counts: {canonical(counts)}\n# columns: ideograph\treadings\tsimplified\n"
        + "".join("\t".join(row) + "\n" for row in rows),
        f"{tier}-provenance.tsv": common + modified + "# columns: " + "\t".join(PROVENANCE_COLUMNS) + "\n"
        + "".join("\t".join(r.columns()) + "\n" for r in records),
        f"{tier}-gaps.tsv": header("fold-han-gaps/1", refs, predictions, authored)
        + "# columns: ideograph\traw\tnormalized\tbatch\treason\n"
        + "".join("\t".join(row) + "\n" for row in gaps),
        f"{tier}-findings.tsv": common + "# columns: ideograph\tkind\tours\treference\n"
        + "".join("\t".join(row) + "\n" for row in findings),
    }
    if impact:
        changes, pulled, report = impact
        files.update({
            f"{tier}-expected-changes.tsv": header("fold-han-expected-changes/1", refs, predictions, authored)
            + "# columns: ideograph\tkind\tbaseline\tproposed\n"
            + "".join("\t".join(row) + "\n" for row in changes),
            f"{tier}-pull-forward.tsv": header("fold-han-pull-forward/1", refs, predictions, authored)
            + "# columns: file\tideograph\treadings\tsimplified\treason\n"
            + "".join("\t".join(row) + "\n" for row in pulled),
            f"{tier}-impact.json": json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        })
    for name, content in files.items():
        (out / name).write_text(content, encoding="utf-8", newline="\n")
    return findings


def read_records(path: pathlib.Path) -> tuple[dict, list[Record]]:
    headers, lines = document(path)
    if headers.get("schema") != PROVENANCE_SCHEMA or headers.get("columns") != "\t".join(PROVENANCE_COLUMNS):
        raise ValueError("missing provenance schema or columns")
    records = []
    seen = set()
    for number, columns in lines:
        if len(columns) != 7:
            raise ValueError(f"{path.name}:{number}: expected seven provenance columns")
        ch, value, basis, attested, tone, rank_text, batch = columns
        ideograph(ch)
        if not re.fullmatch(r"0|[1-9][0-9]*", rank_text):
            raise ValueError("invalid provenance rank")
        rank = int(rank_text)
        if rank:
            if value != reading(value):
                raise ValueError("provenance reading must be NFC")
            sources = attested.split(",")
            if sources != sorted(set(sources)) or not set(sources) <= {"unihan", "cns"}:
                raise ValueError("invalid reading attestation")
        else:
            ideograph(value)
            if value == ch or not attested or tone != "-":
                raise ValueError("invalid mapping provenance")
        if basis not in {"predicted", "agreed", "single"} or not batch:
            raise ValueError("invalid provenance basis or batch")
        if (ch, value) in seen:
            raise ValueError("duplicate provenance record")
        seen.add((ch, value))
        records.append(Record(ch, value, basis, attested, tone, rank, batch))
    return headers, records


def check_notice(table: pathlib.Path, records: list[Record], notice: pathlib.Path, pins: dict | None = None) -> list[tuple]:
    """Bind scoped attributions and delivered legal texts to exact input pins."""
    try:
        if pins is None:
            pins = document(table)[0].get("references")
        notice_terms.check_table(table, records, notice, pins)
        return []
    except (OSError, ValueError, TypeError, KeyError) as exc:
        return [(table.name, "-", "-", "source-terms", "invalid NOTICE", str(exc))]


def check_provenance(table: pathlib.Path, provenance: pathlib.Path, refs: References, predictions: Predictions,
                     authored: set[str], notice: pathlib.Path | None = None) -> list[tuple[str, str, str, str, str, str]]:
    def finding(ch: str, ours: str, reference: str, kind: str = "provenance"):
        return table.name, "-", ch, kind, ours, reference

    try:
        headers, lines = document(table)
        ph, actual = read_records(provenance)
        for h in (headers, ph):
            validate_pins(h.get("references"))
            if h["references"] != refs.pins or h.get("inputs") != predictions.inputs:
                raise ValueError("provenance source pins or prediction checksums differ")
            if h.get("authored") != authored_pin(authored):
                raise ValueError("preceding authored membership checksum differs")
        if headers.get("schema") != PROVENANCE_SCHEMA:
            raise ValueError("missing table provenance schema")
        if headers.get("columns") != "ideograph\treadings\tsimplified":
            raise ValueError("missing authored table columns")
        members = []
        observed = {}
        for number, columns in lines:
            if len(columns) != 3:
                raise ValueError(f"table row {number}: expected three columns")
            ch, text, simple = columns
            ideograph(ch)
            forms = [] if text == "-" else text.split()
            if not text or len(forms) != len(set(forms)) or any(reading(r) != r for r in forms):
                raise ValueError("table readings must be distinct NFC pinyin")
            if simple:
                ideograph(simple)
            members.append(ch)
            observed[ch] = text, simple
        tiers, _ = build_tiers(refs)
        if table.stem in tiers and set(predictions.rows) - set(tiers[table.stem]):
            raise ValueError("prediction contains an ideograph outside the tier")
        selected_predictions = Predictions({ch: values for ch, values in predictions.rows.items() if ch in observed}, predictions.inputs)
        mapping_choices = {ch: simple for ch, (_, simple) in observed.items() if simple}
        reading_choices = {ch: text.split() for ch, (text, _) in observed.items() if text != "-"}
        proposed, expected, gaps, issues = assemble(members, refs, selected_predictions, authored,
                                                    mapping_choices, reading_choices, tier=table.stem)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        return [finding("-", "invalid records", str(exc))]

    findings = [finding(ch, ours, reference, kind) for ch, kind, ours, reference in issues]
    findings.extend(check_notice(table, expected + actual, notice or data.DEFAULT_AUTHORED_DIR / "NOTICE", refs.pins))
    try:
        notice_terms.check_modified_file(provenance, expected + actual)
    except (OSError, ValueError) as exc:
        findings.append(finding("-", "invalid modified-file notice", str(exc), "source-terms"))
    if headers.get("complete"):
        if table.stem not in tiers:
            findings.append(finding("-", table.stem, "unknown complete tier", "coverage"))
        else:
            required = set(tiers[table.stem])
            for ch in sorted(required - set(members)):
                findings.append(finding(ch, "-", "required by tier", "coverage"))
            for ch in sorted(set(members) - required):
                findings.append(finding(ch, "listed", "not in tier", "coverage"))
    actual_by_key = {(r.ch, r.value): r for r in actual}
    for r in expected:
        found = actual_by_key.pop((r.ch, r.value), None)
        if found != r:
            findings.append(finding(r.ch, canonical(found.columns()) if found else "-", canonical(r.columns())))
    for r in actual_by_key.values():
        findings.append(finding(r.ch, canonical(r.columns()), "no shipped attested value"))
    for ch, text, simple in proposed:
        ours = observed[ch]
        if ours != (text, simple):
            findings.append(finding(ch, " / ".join(ours), " / ".join((text, simple))))
    counts = dict(sorted(Counter(r.basis for r in actual if r.rank).items()))
    if headers.get("counts") != counts:
        findings.append(finding("-", canonical(headers.get("counts")), canonical(counts)))
    return findings


def reference_arguments(parser):
    parser.add_argument("--references", type=pathlib.Path, help="pin manifest for explicit offline reference files")
    parser.add_argument("--unihan", type=pathlib.Path)
    parser.add_argument("--cns-properties", type=pathlib.Path)
    parser.add_argument("--cns-mappings", type=pathlib.Path)
    parser.add_argument("--opencc", type=pathlib.Path)
    parser.add_argument("--authored", type=pathlib.Path, default=data.DEFAULT_AUTHORED_DIR)
    parser.add_argument("--predictions", nargs="*", type=pathlib.Path, default=[])


def read_references(args) -> References:
    explicit = (args.unihan, args.cns_properties, args.cns_mappings, args.opencc)
    pins = json.loads(args.references.read_text(encoding="utf-8")) if args.references else shipped_pins()
    validate_pins(pins)
    if args.references and not all(explicit):
        raise ValueError("a reference manifest requires all four explicit files")
    if bool(args.cns_properties) != bool(args.cns_mappings):
        raise ValueError("provide both CNS archives")
    unihan = args.unihan or data.unihan_archive(None)
    cns = (args.cns_properties, args.cns_mappings) if args.cns_properties else data.cns_archives()
    opencc = args.opencc or data.opencc_archive()
    for name, path in zip(("unihan", "cns-properties", "cns-mappings", "opencc"), (unihan, *cns, opencc)):
        if digest(path) != pins[name]["sha256"]:
            raise ValueError(f"{name}: reference sha256 mismatch")
    return References(data.unihan_fields(unihan, *FIELDS), data.cns_readings(cns), data.opencc_characters(opencc), pins)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    assembly = commands.add_parser("assemble", help="write tier proposals, findings and change/closure reports")
    reference_arguments(assembly)
    assembly.add_argument("--tier", choices=[p.removesuffix(".tsv") for p in data.RARE_FILES], required=True)
    assembly.add_argument("--output", type=pathlib.Path, required=True)
    assembly.add_argument("--members", type=pathlib.Path, help="one selected ideograph per line; write a partial proposal")
    assembly.add_argument("--decisions", type=pathlib.Path, help="five-column reviewed TSV supplying exact mapping and reading choices")
    assembly.add_argument("--generated", type=pathlib.Path, default=data.DEFAULT_UNIHAN_DIR,
                          help="current generated Han tables for expected-change reports")
    assembly.add_argument("--assembly", type=pathlib.Path,
                          help="full proposed rare tier tables for staged pull-forward and final impact reports")
    check = commands.add_parser("check", help="audit a table and its provenance without word checks")
    reference_arguments(check)
    check.add_argument("--provenance", type=pathlib.Path, required=True)
    check.add_argument("--reviewed", type=pathlib.Path, default=data.DEFAULT_REVIEWED_FILE)
    check.add_argument("table", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        refs = read_references(args)
        predictions = read_predictions(args.predictions)
        authored = authored_before(args.authored, args.tier if args.command == "assemble" else args.table.stem)
        if args.command == "assemble":
            tiers, withheld = build_tiers(refs)
            tier_members = tiers[args.tier]
            members = args.members.read_text(encoding="utf-8").splitlines() if args.members else tier_members
            if set(members) - set(tier_members) - set(withheld[args.tier]):
                raise ValueError("selected member outside tier")
            findings = write_assembly(args.output, args.tier, members, refs, predictions, authored,
                                      complete=args.members is None, decisions=args.decisions,
                                      baseline=table_state(args.authored, args.generated),
                                      report_assembly=read_assembly(args.assembly, refs) if args.assembly else None)
        else:
            findings = check_provenance(args.table, args.provenance, refs, predictions, authored)
            reviewed = data.read_reviewed(args.reviewed)
            findings = [f for f in findings if f[3] == "source-terms" or data.finding_key(f) not in reviewed]
            for f in findings:
                print("\t".join(data.clean_tsv(value) for value in f))
        print(f"{args.command}: blocking findings={len(findings)}", file=sys.stderr)
        return int(bool(findings))
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
