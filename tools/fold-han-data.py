#!/usr/bin/env python3
"""Generate Unihan-based Han folds and check authored Han tables.

`generate` writes the per-character pinyin and simplified mappings in
`src/search/fold/unihan/`. Ideographs listed in authored Han tables are
excluded from both generated files. The pinned Unihan archive is checked
before its fields are used.

`check` validates authored rows, compares their readings and simplified forms
with Unihan 16.0.0 (a reading counts as attested when the character or its
simplified or traditional counterpart has it, and as archaic when only Hanyu
Da Zidian does), checks national-standard tier coverage, and applies exact
reviewed-disagreement suppressions. Authored rows use
`ideograph<TAB>readings<TAB>simplified`; readings are tone-marked Mandarin
pinyin or `-`, and the simplified field is empty when no form applies.

Usage: tools/fold-han-data.py [generate] [--unihan Unihan.zip]
                              [--authored DIR] [output-dir]
       tools/fold-han-data.py check [--authored DIR] [--reviewed FILE]
                                    [FILE ...]
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys
import unicodedata
import urllib.request
import zipfile
from collections import Counter, defaultdict

UNIHAN_VERSION = "16.0.0"
UNIHAN_SHA256 = "b8f000df69de7828d21326a2ffea462b04bc7560022989f7cc704f10521ef3e0"
UNIHAN_URL = f"https://www.unicode.org/Public/{UNIHAN_VERSION}/ucd/Unihan.zip"

ROOT = pathlib.Path(__file__).resolve().parent.parent
FOLD_DIR = ROOT / "src" / "search" / "fold"
DEFAULT_AUTHORED_DIR = FOLD_DIR / "tables" / "han"
DEFAULT_UNIHAN_DIR = FOLD_DIR / "unihan"
DEFAULT_REVIEWED_FILE = ROOT / "tools" / "fold-han-reviewed.tsv"

IDEOGRAPH_BLOCKS = [
    (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0x20000, 0x2A6DF), (0x2A700, 0x2B73F),
    (0x2B740, 0x2B81F), (0x2B820, 0x2CEAF), (0x2CEB0, 0x2EBEF), (0x2EBF0, 0x2EE5F),
    (0x30000, 0x3134F), (0x31350, 0x323AF),
]

READING_FIELDS = (
    "kMandarin", "kHanyuPinyin", "kXHC1983", "kTGHZ2013", "kHanyuPinlu",
)
ATTESTING_FIELDS = READING_FIELDS
MODERN_DICTIONARY_FIELDS = ("kTGHZ2013", "kXHC1983")
# Evidence of current use; a reading only kHanyuPinyin (Hanyu Da Zidian, a
# historical dictionary) attests is reported as archaic.
CURRENT_FIELDS = ("kMandarin", "kXHC1983", "kTGHZ2013", "kHanyuPinlu")
VARIANT_FIELDS = ("kSimplifiedVariant", "kTraditionalVariant")
TONE_MARKS = {"\u0304", "\u0301", "\u030c", "\u0300"}
DIACRITIC = "\u0308"
PINYIN_CIRCUMFLEX = "\u0302"
CHECK_KINDS = ("format", "coverage", "default", "tone", "unattested", "archaic", "missing", "simplified")


def unihan_archive(path: pathlib.Path | None) -> pathlib.Path:
    if path is None:
        path = pathlib.Path.home() / ".cache" / "muzaiten" / f"Unihan-{UNIHAN_VERSION}.zip"
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(UNIHAN_URL, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != UNIHAN_SHA256:
        sys.exit(f"{path}: sha256 {digest}, expected Unihan {UNIHAN_VERSION}'s {UNIHAN_SHA256}")
    if unicodedata.unidata_version != UNIHAN_VERSION:
        sys.exit(
            f"Python Unicode data is {unicodedata.unidata_version}; "
            f"Unihan {UNIHAN_VERSION} is required"
        )
    return path


def unihan_fields(archive: pathlib.Path, *wanted: str) -> dict[str, dict[str, str]]:
    """Map each selected Unihan field to its raw value by ideograph."""
    fields: dict[str, dict[str, str]] = {name: {} for name in wanted}
    with zipfile.ZipFile(archive) as z:
        for member in ("Unihan_Readings.txt", "Unihan_Variants.txt"):
            for line in z.read(member).decode("utf-8").splitlines():
                if not line or line.startswith("#"):
                    continue
                code, field, value = line.split("\t", 2)
                if field in fields:
                    fields[field][chr(int(code[2:], 16))] = value
    return fields


def toneless(reading: str) -> str:
    """Return the lowercase reading without combining tone or vowel marks."""
    plain = "".join(
        char for char in unicodedata.normalize("NFD", reading)
        if not unicodedata.combining(char)
    )
    return plain.lower()


def field_readings(field: str, value: str) -> list[str]:
    """Extract pinyin tokens from a Unihan reading-field value."""
    if field == "kMandarin":
        return [token for token in re.split(r"[\s,;，]+", value.strip()) if token]

    if field in {"kHanyuPinyin", "kXHC1983", "kTGHZ2013"}:
        readings = []
        for entry in re.split(r"[\s;]+", value.strip()):
            if not entry:
                continue
            pronunciation = entry.rsplit(":", 1)[-1]
            readings.extend(token for token in re.split(r"[,，]+", pronunciation) if token)
        return readings

    if field == "kHanyuPinlu":
        return re.findall(r"([^\s(),;，]+)\s*\(\d+\)", value)

    return []


def default_reading(ch: str, fields: dict[str, dict[str, str]]) -> str | None:
    """Return the first kMandarin reading or first kHanyuPinyin fallback."""
    for field in ("kMandarin", "kHanyuPinyin"):
        value = fields[field].get(ch)
        if value:
            readings = field_readings(field, value)
            if readings:
                reading = toneless(readings[0])
                if reading.isascii() and reading.isalpha():
                    return reading
    return None


def simplified_form(ch: str, fields: dict[str, dict[str, str]]) -> str | None:
    """Return the common simplified variant unless the character lists itself."""
    variants = [
        chr(int(value[2:], 16))
        for value in fields["kSimplifiedVariant"].get(ch, "").split()
    ]
    if not variants or ch in variants:
        return None
    return variants[0]


def is_unihan_ideograph(ch: str) -> bool:
    cp = ord(ch)
    return (
        any(lo <= cp <= hi for lo, hi in IDEOGRAPH_BLOCKS)
        and unicodedata.category(ch) == "Lo"
    )


def authored_ideographs(directory: pathlib.Path) -> set[str]:
    """Read ideograph keys from authored TSV rows, ignoring comments and blanks."""
    if not directory.is_dir():
        return set()
    chars = set()
    for path in sorted(directory.glob("*.tsv")):
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            ch = line.split("\t", 1)[0]
            if len(ch) == 1 and is_unihan_ideograph(ch):
                chars.add(ch)
    return chars


def generate(unihan_path: pathlib.Path | None, authored_dir: pathlib.Path, out_dir: pathlib.Path) -> None:
    archive = unihan_archive(unihan_path)
    fields = unihan_fields(
        archive, "kMandarin", "kHanyuPinyin", "kSimplifiedVariant",
    )
    excluded = authored_ideographs(authored_dir)
    chars = [
        chr(cp)
        for lo, hi in IDEOGRAPH_BLOCKS
        for cp in range(lo, hi + 1)
        if unicodedata.category(chr(cp)) == "Lo"
    ]

    readings: dict[str, list[str]] = defaultdict(list)
    simplified: dict[str, list[str]] = defaultdict(list)
    for ch in chars:
        if ch in excluded:
            continue
        if reading := default_reading(ch, fields):
            readings[reading].append(ch)
        if simple := simplified_form(ch, fields):
            simplified[simple].append(ch)

    out_dir.mkdir(parents=True, exist_ok=True)
    header = (
        f"# Generated by tools/fold-han-data.py from Unihan {UNIHAN_VERSION}.\n"
        "# Unicode License v3: see UNICODE-LICENSE.txt.\n"
    )
    with (out_dir / "han_pinyin.tsv").open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(header + "# toneless pinyin<TAB>ideographs with that default reading\n")
        for reading in sorted(readings):
            stream.write(f"{reading}\t{''.join(readings[reading])}\n")
    with (out_dir / "han_simplified.tsv").open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(header + "# simplified<TAB>ideographs folded onto it\n")
        for simple in sorted(simplified):
            stream.write(f"{simple}\t{''.join(simplified[simple])}\n")


# The essential tier follows the four codecs in order, retaining each
# ideograph only in the earliest set that contains it.
def is_ideograph(ch: str) -> bool:
    cp = ord(ch)
    return (0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF) and unicodedata.category(ch) == "Lo"


def _decode(raw: bytes, codec: str) -> str | None:
    try:
        ch = raw.decode(codec)
    except UnicodeDecodeError:
        return None
    return ch if len(ch) == 1 and is_ideograph(ch) else None


def gb2312(level: int | None = None) -> list[str]:
    """GB2312 rows 16-55 (level 1, in pinyin order) and 56-87 (level 2)."""
    rows = {1: range(16, 56), 2: range(56, 88), None: range(16, 88)}[level]
    return [c for r in rows for k in range(1, 95) if (c := _decode(bytes([0xA0 + r, 0xA0 + k]), "gb2312"))]


def big5_level1() -> list[str]:
    out = []
    for code in range(0xA440, 0xC67F):
        if 0x40 <= code & 0xFF <= 0x7E or 0xA1 <= code & 0xFF <= 0xFE:
            if c := _decode(code.to_bytes(2, "big"), "big5"):
                out.append(c)
    return out


def jis_x0208() -> list[str]:
    return [c for r in range(16, 85) for k in range(1, 95) if (c := _decode(bytes([0xA0 + r, 0xA0 + k]), "euc_jp"))]


def ks_x1001() -> list[str]:
    return [c for r in range(0xCA, 0xFE) for k in range(0xA1, 0xFF) if (c := _decode(bytes([r, k]), "euc_kr"))]


def tiers() -> dict[str, list[str]]:
    """gb2312, big5, jis, ksx: each set's ideographs not in an earlier set, in set order."""
    seen: set[str] = set()
    result = {}
    for name, chars in (("gb2312", gb2312()), ("big5", big5_level1()), ("jis", jis_x0208()), ("ksx", ks_x1001())):
        new = []
        for c in chars:
            if c not in seen:
                seen.add(c)
                new.append(c)
        result[name] = new
    return result


def pinyin_base(reading: str) -> str | None:
    """Return the ASCII toneless spelling when a value contains pinyin letters."""
    letters = []
    for char in unicodedata.normalize("NFD", reading.lower()):
        if "a" <= char <= "z":
            letters.append(char)
        elif unicodedata.combining(char) and char in TONE_MARKS | {DIACRITIC, PINYIN_CIRCUMFLEX}:
            continue
        else:
            return None
    base = "".join(letters)
    return base if base and base.isascii() and base.isalpha() else None


def pinyin_inventory(fields: dict[str, dict[str, str]]) -> set[str]:
    """Collect Mandarin syllable spellings represented by Unihan readings."""
    inventory = set()
    for field in READING_FIELDS:
        for value in fields[field].values():
            inventory.update(
                base for reading in field_readings(field, value)
                if (base := pinyin_base(reading)) is not None
            )
    return inventory


def expected_tone_position(letters: str) -> int | None:
    """Return the position where standard pinyin writes its tone mark."""
    if "a" in letters:
        return letters.index("a")
    if "e" in letters:
        return letters.index("e")
    if "ou" in letters:
        return letters.index("o")
    vowels = [index for index, char in enumerate(letters) if char in "iou"]
    if vowels:
        return vowels[-1]
    if letters in {"m", "n"}:
        return 0
    return None


def is_tone_marked_pinyin(
    reading: str,
    inventory: set[str],
    neutral_spellings: set[str],
    special_spellings: set[str],
) -> bool:
    """Check tone placement and membership in Unihan's Mandarin syllables."""
    if not reading or reading != reading.lower():
        return False
    letters = []
    tone_positions = []
    last_letter = None
    for char in unicodedata.normalize("NFD", reading):
        if "a" <= char <= "z":
            letters.append(char)
            last_letter = len(letters) - 1
        elif unicodedata.combining(char):
            if last_letter is None:
                return False
            if char in TONE_MARKS:
                tone_positions.append(last_letter)
            elif char == DIACRITIC and letters[last_letter] == "u":
                continue
            elif char == PINYIN_CIRCUMFLEX and letters[last_letter] == "e":
                continue
            else:
                return False
        else:
            return False

    base = "".join(letters)
    if base not in inventory:
        return False
    if not tone_positions:
        # Unihan-attested neutral-tone syllables follow the standard unmarked spelling.
        return unicodedata.normalize("NFC", reading) in neutral_spellings
    expected = expected_tone_position(base)
    if expected is None:
        return unicodedata.normalize("NFC", reading) in special_spellings
    return len(tone_positions) == 1 and tone_positions[0] == expected


def read_reviewed(path: pathlib.Path) -> set[tuple[str, str, str]]:
    """Load exact finding keys from a reviewed-disagreements TSV."""
    reviewed = set()
    for row_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw_line.strip() or raw_line.lstrip().startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"{path}:{row_number}: expected four reviewed columns")
        reviewed.add((fields[0], fields[1], fields[2]))
    return reviewed


def clean_tsv(value: str) -> str:
    return value.replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def check(files: list[pathlib.Path], reviewed_file: pathlib.Path) -> int:
    archive = unihan_archive(None)
    fields = unihan_fields(archive, *READING_FIELDS, *VARIANT_FIELDS)
    inventory = pinyin_inventory(fields)
    reviewed = read_reviewed(reviewed_file)
    generated_tiers = tiers()
    findings: list[tuple[str, str, str, str, str, str]] = []
    first_occurrence: dict[str, tuple[str, int]] = {}

    def add(path: pathlib.Path, row: int | str, ch: str, kind: str, ours: str, reference: str) -> None:
        findings.append((str(path), str(row), ch, kind, ours, reference))

    for path in files:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            return 2

        listed: set[str] = set()
        per_file_start = len(findings)
        for row_number, line in enumerate(lines, 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            columns = line.split("\t")
            raw_ch = columns[0] if columns else ""
            display_ch = raw_ch if raw_ch else "-"
            valid_ch = len(raw_ch) == 1 and is_unihan_ideograph(raw_ch)

            if len(columns) != 3:
                add(path, row_number, display_ch, "format", f"columns={len(columns)}", "3 columns")
            if not valid_ch:
                add(path, row_number, display_ch, "format", raw_ch or "-", "one CJK ideograph")
            else:
                listed.add(raw_ch)
                previous = first_occurrence.get(raw_ch)
                if previous is not None:
                    add(
                        path, row_number, raw_ch, "format", raw_ch,
                        f"first listed at {previous[0]}:{previous[1]}",
                    )
                else:
                    first_occurrence[raw_ch] = (str(path), row_number)

            if len(columns) != 3 or not valid_ch:
                continue

            reading_text, simplified_text = columns[1], columns[2]
            readings = [] if reading_text in {"", "-"} else reading_text.split()
            if reading_text == "":
                add(path, row_number, raw_ch, "format", "", "tone-marked readings or -")
            if reading_text not in {"", "-"} and not readings:
                add(path, row_number, raw_ch, "format", reading_text, "tone-marked readings or -")

            seen_readings = set()
            attested_spellings = {
                unicodedata.normalize("NFC", reading)
                for field in ATTESTING_FIELDS
                for reading in field_readings(field, fields[field].get(raw_ch, ""))
            }
            neutral_spellings = {
                reading for reading in attested_spellings
                if not any(char in TONE_MARKS for char in unicodedata.normalize("NFD", reading))
            }
            for reading in readings:
                normalized = unicodedata.normalize("NFC", reading)
                if not is_tone_marked_pinyin(reading, inventory, neutral_spellings, attested_spellings):
                    add(path, row_number, raw_ch, "format", reading, "tone-marked Mandarin syllable")
                if normalized in seen_readings:
                    add(path, row_number, raw_ch, "format", reading, "reading listed once")
                seen_readings.add(normalized)

            if simplified_text and not (
                len(simplified_text) == 1
                and simplified_text != raw_ch
                and is_unihan_ideograph(simplified_text)
            ):
                add(path, row_number, raw_ch, "format", simplified_text, "one other CJK ideograph or empty")

            mandarin_values = field_readings("kMandarin", fields["kMandarin"].get(raw_ch, ""))
            mandarin = mandarin_values[0] if mandarin_values else None
            our_first = readings[0] if readings else None
            ours_base = toneless(our_first) if our_first else None
            reference_base = toneless(mandarin) if mandarin else None
            if mandarin and ours_base != reference_base:
                add(path, row_number, raw_ch, "default", our_first or "-", mandarin)
            elif mandarin and our_first and unicodedata.normalize("NFC", our_first) != unicodedata.normalize("NFC", mandarin):
                add(path, row_number, raw_ch, "tone", our_first, mandarin)

            # A reading is often recorded only under the character's simplified
            # or traditional counterpart, so those count as attestation too.
            family = {raw_ch} | {
                chr(int(code[2:], 16))
                for field in VARIANT_FIELDS
                for code in fields[field].get(raw_ch, "").split()
            }

            def readings_in(field_names: tuple[str, ...]) -> set[str]:
                return {
                    toneless(reading)
                    for member in family
                    for field in field_names
                    for reading in field_readings(field, fields[field].get(member, ""))
                }

            attested = readings_in(ATTESTING_FIELDS)
            current = readings_in(CURRENT_FIELDS)
            our_by_base = {}
            for reading in readings:
                base = toneless(reading)
                if base:
                    our_by_base.setdefault(base, reading)
            for base, reading in our_by_base.items():
                if base not in attested:
                    add(path, row_number, raw_ch, "unattested", reading, "-")
                elif base not in current:
                    add(path, row_number, raw_ch, "archaic", reading, "kHanyuPinyin only")

            authored_bases = set(our_by_base)
            modern_values: dict[str, dict[str, list[str]]] = defaultdict(dict)
            for field in MODERN_DICTIONARY_FIELDS:
                matching_by_base: dict[str, list[str]] = defaultdict(list)
                for reading in field_readings(field, fields[field].get(raw_ch, "")):
                    matching_by_base[toneless(reading)].append(reading)
                for base, values in matching_by_base.items():
                    modern_values[base][field] = list(dict.fromkeys(values))
            for base, sources in modern_values.items():
                if base in authored_bases:
                    continue
                reference = ";".join(
                    f"{field}={','.join(values)}"
                    for field, values in sources.items()
                )
                add(path, row_number, raw_ch, "missing", reading_text or "-", reference)

            expected_simple = simplified_form(raw_ch, fields) or ""
            if simplified_text != expected_simple:
                add(path, row_number, raw_ch, "simplified", simplified_text or "-", expected_simple or "-")

        stem = path.stem
        if stem in generated_tiers:
            tier_order = generated_tiers[stem]
            tier_members = set(tier_order)
            for missing_ch in tier_order:
                if missing_ch not in listed:
                    add(path, "-", missing_ch, "coverage", "-", "required by tier")
            for extra_ch in sorted(listed - tier_members):
                add(path, "-", extra_ch, "coverage", "listed", "not in tier")

        unsuppressed = [
            finding for finding in findings[per_file_start:]
            if (finding[2], finding[3], finding[4]) not in reviewed
        ]
        counts = Counter(finding[3] for finding in unsuppressed)
        print(
            f"{path}: " + " ".join(f"{kind}={counts[kind]}" for kind in CHECK_KINDS),
            file=sys.stderr,
        )

    unsuppressed = [
        finding for finding in findings
        if (finding[2], finding[3], finding[4]) not in reviewed
    ]
    for finding in unsuppressed:
        print("\t".join(clean_tsv(value) for value in finding))
    return 1 if unsuppressed else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    if not argv or argv[0] not in {"generate", "check"}:
        argv = ["generate", *argv]

    parser = argparse.ArgumentParser(description="Generate and check the Han search-fold data.")
    commands = parser.add_subparsers(dest="command", required=True)

    generate_parser = commands.add_parser("generate", help="generate mappings from Unihan 16.0.0")
    generate_parser.add_argument("--unihan", type=pathlib.Path, help="Unihan.zip to read instead of the cached archive")
    generate_parser.add_argument("--authored", type=pathlib.Path, default=DEFAULT_AUTHORED_DIR,
                                 help="directory whose TSV ideographs are excluded")
    generate_parser.add_argument("out_dir", nargs="?", type=pathlib.Path, default=DEFAULT_UNIHAN_DIR,
                                 help="output directory (default: src/search/fold/unihan)")

    check_parser = commands.add_parser("check", help="check authored readings and variants against Unihan")
    check_parser.add_argument("--authored", type=pathlib.Path, default=DEFAULT_AUTHORED_DIR,
                              help="directory scanned when no FILE arguments are given")
    check_parser.add_argument("--reviewed", type=pathlib.Path, default=DEFAULT_REVIEWED_FILE,
                              help="reviewed-disagreements TSV")
    check_parser.add_argument("files", nargs="*", type=pathlib.Path,
                              help="specific files to check; these replace the directory scan")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    if args.command == "generate":
        generate(args.unihan, args.authored, args.out_dir)
        return 0

    files = args.files
    if not files and args.authored.is_dir():
        files = sorted(args.authored.glob("*.tsv"))
    try:
        return check(files, args.reviewed)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
