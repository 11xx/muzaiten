#!/usr/bin/env python3
"""Generate Unihan-based Han folds and check authored Han tables.

`generate` writes the per-character pinyin and simplified mappings in
`src/search/fold/unihan/`. Ideographs listed in authored Han tables are
excluded from both generated files. The pinned Unihan archive is checked
before its fields are used.

`check` validates authored rows, compares their readings and simplified forms
with Unihan 16.0.0 (a reading counts as attested when the character or its
simplified or traditional counterpart has it, and a reading after the first
as archaic when only Hanyu Da Zidian does), checks every reading's exact tone, checks national-standard
tier coverage, and applies exact reviewed-disagreement suppressions. Authored
rows use `ideograph<TAB>readings<TAB>simplified`; readings are tone-marked
Mandarin pinyin or `-`, and the simplified field is empty when no form applies.
Reviewed rows use `ideograph-or-word<TAB>kind<TAB>ours<TAB>reference<TAB>reason`.
Directory mode requires all four tier files; explicit FILE arguments select
the character files. The checker also checks `words.tsv`, whose rows use
simplified words and one or two tone-marked readings, against authored or
Unihan character readings and CC-CEDICT. It caches CC-CEDICT under
`~/.cache/muzaiten/`, never copies it into the repository, and prints its date
line with word findings.

Usage: tools/fold-han-data.py [generate] [--unihan Unihan.zip]
                              [--authored DIR] [output-dir]
       tools/fold-han-data.py check [--authored DIR] [--reviewed FILE]
                                    [--words FILE] [FILE ...]
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
CEDICT_URL = "https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.zip"
OPENCC_URL = "https://raw.githubusercontent.com/BYVoid/OpenCC/025f371dc76b598d77384fbdab90c937471844d8/data/dictionary/TSCharacters.txt"
OPENCC_SHA256 = "737c21c66f55a419dd6956cb3089476cdefc5a36877452631617696df1e5d925"

ROOT = pathlib.Path(__file__).resolve().parent.parent
FOLD_DIR = ROOT / "src" / "search" / "fold"
DEFAULT_AUTHORED_DIR = FOLD_DIR / "tables" / "han"
DEFAULT_UNIHAN_DIR = FOLD_DIR / "unihan"
DEFAULT_REVIEWED_FILE = ROOT / "tools" / "fold-han-reviewed.tsv"
CEDICT_CACHE_FILE = "cedict.zip"

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
CHECK_KINDS = (
    "format", "coverage", "default", "tone", "unattested", "archaic", "missing", "simplified",
    "reading", "cedict", "cedict-extra", "uncommon", "edge-particle", "opencc",
)
TIER_FILES = ("gb2312.tsv", "big5.tsv", "jis.tsv", "ksx.tsv")
# At a word edge, these characters must retain a reading used when they are particles.
PARTICLE_READINGS = {"着": "zhe", "著": "zhe", "了": "le", "的": "de", "地": "de",
                     "得": "de", "都": "dou", "和": "he", "没": "mei", "还": "hai"}


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


def cedict_archive(path: pathlib.Path | None = None) -> pathlib.Path:
    """Return the cached CC-CEDICT archive, downloading it on first use."""
    if path is None:
        path = pathlib.Path.home() / ".cache" / "muzaiten" / CEDICT_CACHE_FILE
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            urllib.request.urlretrieve(CEDICT_URL, path)
    if not zipfile.is_zipfile(path):
        raise ValueError(f"{path}: not a CC-CEDICT zip archive")
    return path


def opencc_archive(path: pathlib.Path | None = None) -> pathlib.Path:
    if path is None:
        path = pathlib.Path.home() / ".cache" / "muzaiten" / "opencc-1.4.2" / "TSCharacters.txt"
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            import tempfile
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".TSCharacters-", delete=False) as temporary:
                staged = pathlib.Path(temporary.name)
            try:
                urllib.request.urlretrieve(OPENCC_URL, staged)
                digest = hashlib.sha256(staged.read_bytes()).hexdigest()
                if digest != OPENCC_SHA256:
                    raise ValueError(f"{staged}: sha256 {digest}, expected OpenCC 1.4.2's {OPENCC_SHA256}")
                staged.replace(path)
            finally:
                staged.unlink(missing_ok=True)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != OPENCC_SHA256:
        raise ValueError(f"{path}: sha256 {digest}, expected OpenCC 1.4.2's {OPENCC_SHA256}")
    return path


def opencc_characters(path: pathlib.Path) -> dict[str, list[str]]:
    mappings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, values = line.split("\t", 1)
        if len(key) == 1:
            mappings[key] = values.split()
    return mappings


def cedict_pinyin(token: str) -> str | None:
    """Return the toneless spelling of one numbered CC-CEDICT syllable."""
    plain = token.lower().replace("u:", "u").replace("ü", "u").replace("v", "u")
    plain = re.sub(r"[0-5]$", "", plain)
    return plain if plain and plain.isascii() and plain.isalpha() else None


def cedict_words(archive: pathlib.Path) -> tuple[str, dict[str, set[tuple[str, ...]]]]:
    """Index simplified and traditional spellings and their numbered-pinyin readings.

    Traditional spellings count because a row may keep a character that has no
    simplified mapping of its own (著急 for traditional text, where 着急 is the
    simplified spelling)."""
    words: dict[str, set[tuple[str, ...]]] = defaultdict(set)
    date_line = "date line unavailable"
    with zipfile.ZipFile(archive) as z:
        member = next((name for name in z.namelist() if pathlib.PurePosixPath(name).name == "cedict_ts.u8"), None)
        if member is None:
            raise ValueError(f"{archive}: missing cedict_ts.u8")
        for line in z.read(member).decode("utf-8").splitlines():
            if line.startswith("#! date=") or line.startswith("#. Date:"):
                date_line = line
                continue
            if not line or line.startswith("#"):
                continue
            match = re.match(r"^(\S+)\s+(\S+)\s+\[([^]]*)\]", line)
            if not match:
                continue
            traditional, simplified, reading_text = match.groups()
            syllables = tuple(cedict_pinyin(token) for token in reading_text.split())
            if syllables and all(syllables):
                words[simplified].add(syllables)
                words[traditional].add(syllables)
    return date_line, words


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
        if path.name == "words.tsv":
            continue
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
        return True
    expected = expected_tone_position(base)
    if expected is None:
        return unicodedata.normalize("NFC", reading) in special_spellings
    return len(tone_positions) == 1 and tone_positions[0] == expected


def read_reviewed(path: pathlib.Path) -> set[tuple[str, str, str, str]]:
    """Load exact finding keys from a reviewed-disagreements TSV."""
    reviewed = set()
    for row_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw_line.strip() or raw_line.lstrip().startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 5:
            raise ValueError(f"{path}:{row_number}: expected five reviewed columns")
        reviewed.add((fields[0], fields[1], fields[2], fields[3]))
    return reviewed


def finding_key(finding: tuple[str, str, str, str, str, str]) -> tuple[str, str, str, str]:
    """Return the exact fields a reviewed row must match to suppress a finding."""
    return finding[2], finding[3], finding[4], finding[5]


def clean_tsv(value: str) -> str:
    return value.replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def word_character_data(authored_dir: pathlib.Path) -> tuple[dict[str, list[str]], dict[str, str]]:
    """Read authored readings and simplified mappings, then Unihan defaults."""
    readings: dict[str, list[str]] = {}
    simplified: dict[str, str] = {}
    pinyin_path = DEFAULT_UNIHAN_DIR / "han_pinyin.tsv"
    for line in pinyin_path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        pinyin, characters = line.split("\t", 1)
        for ch in characters:
            readings.setdefault(ch, [toneless(pinyin)])

    simple_path = DEFAULT_UNIHAN_DIR / "han_simplified.tsv"
    for line in simple_path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        simple, characters = line.split("\t", 1)
        for ch in characters:
            simplified[ch] = simple

    for path in sorted(authored_dir.glob("*.tsv")):
        if path.name == "words.tsv":
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            columns = line.split("\t")
            if len(columns) != 3 or len(columns[0]) != 1:
                continue
            ch, reading_text, simple = columns
            readings[ch] = [] if reading_text == "-" else [toneless(reading) for reading in reading_text.split()]
            if simple:
                simplified[ch] = simple
    return readings, simplified


def terminal_simplified(ch: str, mappings: dict[str, str]) -> str:
    seen = set()
    while ch in mappings and ch not in seen:
        seen.add(ch)
        ch = mappings[ch]
    return ch


def check_words(
    path: pathlib.Path,
    authored_dir: pathlib.Path,
    cedict: dict[str, set[tuple[str, ...]]],
    inventory: set[str],
    special_spellings: set[str],
    reviewed: set[tuple[str, str, str, str]],
) -> tuple[list[tuple[str, str, str, str, str, str]], Counter]:
    """Check simplified Han word readings against character data and CC-CEDICT."""
    readings, simplified = word_character_data(authored_dir)
    findings: list[tuple[str, str, str, str, str, str]] = []
    first_occurrence: dict[str, int] = {}

    def add(row: int, word: str, kind: str, ours: str, reference: str) -> None:
        findings.append((str(path), str(row), word or "-", kind, ours, reference))

    for row_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        columns = line.split("\t")
        word = columns[0] if columns else ""
        if len(columns) not in (2, 3):
            add(row_number, word, "format", f"columns={len(columns)}", "2 or 3 columns")
            continue

        valid_word = bool(word) and all(is_unihan_ideograph(ch) for ch in word)
        if not valid_word:
            add(row_number, word, "format", word or "-", "one or more CJK ideographs")
        else:
            previous = first_occurrence.get(word)
            if previous is not None:
                add(row_number, word, "format", word, f"first listed at {path}:{previous}")
            else:
                first_occurrence[word] = row_number
            expected_simple = "".join(terminal_simplified(ch, simplified) for ch in word)
            if word != expected_simple:
                add(row_number, word, "format", word, expected_simple)

        references = cedict.get(word)
        reading_fields = columns[1:]
        if valid_word:
            for position, index in (("first", 0), ("last", -1)):
                particle = PARTICLE_READINGS.get(word[index])
                if particle and not any(
                    len(syllables := reading.split()) == len(word)
                    and pinyin_base(syllables[index]) == particle
                    for reading in reading_fields
                ):
                    add(row_number, word, "edge-particle", " / ".join(reading_fields),
                        f"{position}:{word[index]} {particle}")
        normalized_readings = [" ".join(unicodedata.normalize("NFC", field).split())
                               for field in reading_fields]
        if len(normalized_readings) == 2 and normalized_readings[0] == normalized_readings[1]:
            add(row_number, word, "format", reading_fields[1], "a distinct second reading")

        has_valid_reading = False
        for reading_text in reading_fields:
            syllables = reading_text.split()
            if not syllables:
                add(row_number, word, "format", reading_text or "-", "tone-marked pinyin syllables")
            elif valid_word and len(syllables) != len(word):
                add(row_number, word, "format", f"{len(syllables)} syllables", f"{len(word)} syllables")

            valid_pinyin = []
            for syllable in syllables:
                valid = is_tone_marked_pinyin(syllable, inventory, special_spellings)
                if not valid:
                    add(row_number, word, "format", syllable, "tone-marked Mandarin syllable")
                valid_pinyin.append(pinyin_base(syllable) if valid else None)

            if not valid_word or len(syllables) != len(word) or any(base is None for base in valid_pinyin):
                continue
            has_valid_reading = True

            for ch, syllable, base in zip(word, syllables, valid_pinyin):
                available = readings.get(ch, [])
                if base not in available:
                    add(row_number, word, "reading", syllable, " ".join(available) or "-")

            if references and tuple(valid_pinyin) not in references:
                alternatives = sorted(" ".join(reading) for reading in references)
                add(row_number, word, "cedict", reading_text, "; ".join(alternatives))

        if has_valid_reading and references:
            authored = {tuple(pinyin_base(syllable) for syllable in field.split())
                        for field in reading_fields}
            for reference in sorted(references - authored):
                add(row_number, word, "cedict-extra", "; ".join(reading_fields), " ".join(reference))

        if has_valid_reading and not references:
            add(row_number, word, "uncommon", reading_fields[0], "-")

    unsuppressed = [finding for finding in findings if finding_key(finding) not in reviewed]
    counts = Counter(finding[3] for finding in unsuppressed)
    return unsuppressed, counts


def check(files: list[pathlib.Path], reviewed_file: pathlib.Path, words_file: pathlib.Path | None = None,
          authored_dir: pathlib.Path = DEFAULT_AUTHORED_DIR,
          opencc_path: pathlib.Path | None = None) -> int:
    opencc = (opencc_characters(opencc_archive(opencc_path))
              if any(path.name in TIER_FILES for path in files) else {})
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
            if path.name in TIER_FILES:
                opencc_values = opencc.get(raw_ch)
                our_simple = simplified_text if simplified_text not in {"", "-"} else raw_ch
                if opencc_values and our_simple != opencc_values[0]:
                    add(path, row_number, raw_ch, "opencc", our_simple, "/".join(opencc_values))
            readings = [] if reading_text in {"", "-"} else reading_text.split()
            if reading_text == "":
                add(path, row_number, raw_ch, "format", "", "tone-marked readings or -")
            if reading_text not in {"", "-"} and not readings:
                add(path, row_number, raw_ch, "format", reading_text, "tone-marked readings or -")

            family = {raw_ch} | {
                chr(int(code[2:], 16))
                for field in VARIANT_FIELDS
                for code in fields[field].get(raw_ch, "").split()
            }
            attested_by_base: dict[str, set[str]] = defaultdict(set)
            for member in family:
                for field in ATTESTING_FIELDS:
                    for reading in field_readings(field, fields[field].get(member, "")):
                        attested_by_base[toneless(reading)].add(unicodedata.normalize("NFC", reading))
            attested_spellings = {form for forms in attested_by_base.values() for form in forms}

            seen_readings = set()
            for reading in readings:
                normalized = unicodedata.normalize("NFC", reading)
                if not is_tone_marked_pinyin(reading, inventory, attested_spellings):
                    add(path, row_number, raw_ch, "format", reading, "tone-marked Mandarin syllable")
                if normalized in seen_readings:
                    add(path, row_number, raw_ch, "format", reading, "reading listed once")
                seen_readings.add(normalized)
                attested = sorted(attested_by_base.get(toneless(reading), set()))
                if normalized not in attested:
                    add(path, row_number, raw_ch, "tone", reading, " ".join(attested) if attested else "-")

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

            # A reading is often recorded only under the character's simplified
            # or traditional counterpart, so those count as attestation too.
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
                elif base not in current and reading != readings[0]:
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
            if finding_key(finding) not in reviewed
        ]
        counts = Counter(finding[3] for finding in unsuppressed)
        print(
            f"{path}: " + " ".join(f"{kind}={counts[kind]}" for kind in CHECK_KINDS),
            file=sys.stderr,
        )

    unsuppressed = [
        finding for finding in findings
        if finding_key(finding) not in reviewed
    ]
    if words_file is not None:
        date_line, cedict = cedict_words(cedict_archive())
        special_spellings = {
            unicodedata.normalize("NFC", reading)
            for field in READING_FIELDS
            for value in fields[field].values()
            for reading in field_readings(field, value)
        }
        word_findings, word_counts = check_words(
            words_file, authored_dir, cedict, inventory, special_spellings, reviewed,
        )
        unsuppressed.extend(word_findings)
        print(
            f"{words_file}: " + " ".join(f"{kind}={word_counts[kind]}" for kind in CHECK_KINDS)
            + f"; CC-CEDICT {date_line}",
            file=sys.stderr,
        )
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

    check_parser = commands.add_parser("check", help="check authored readings, variants, and words")
    check_parser.add_argument("--authored", type=pathlib.Path, default=DEFAULT_AUTHORED_DIR,
                              help="directory scanned when no FILE arguments are given")
    check_parser.add_argument("--reviewed", type=pathlib.Path, default=DEFAULT_REVIEWED_FILE,
                              help="reviewed-disagreements TSV")
    check_parser.add_argument("--words", type=pathlib.Path,
                              help="word readings TSV (default: words.tsv in the authored directory)")
    check_parser.add_argument("--opencc", type=pathlib.Path,
                              help="OpenCC TSCharacters.txt instead of the cached reference")
    check_parser.add_argument("files", nargs="*", type=pathlib.Path,
                              help="specific files to check; these replace the directory scan")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    if args.command == "generate":
        generate(args.unihan, args.authored, args.out_dir)
        return 0

    files = args.files
    if not files:
        required = [args.authored / name for name in TIER_FILES]
        missing = [path for path in required if not path.is_file()]
        if missing:
            for path in missing:
                print(f"{path}: required authored Han tier file is missing", file=sys.stderr)
            return 2
        files = required
    try:
        return check(files, args.reviewed, args.words or args.authored / "words.tsv", args.authored,
                     args.opencc)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
