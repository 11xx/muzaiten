#!/usr/bin/env python3
"""Compile the search-fold tables into C++ initializers.

Reads the hand-authored tables in src/search/fold/tables/, the Han reading
data next to them, and Python's Unicode character database, and writes one
C++ include holding:

- a 32-bit entry per BMP code point and a sorted table of astral entries,
  each giving the code point's primary fold (keep it, replace it with one
  character or a pooled string, drop it, hand it to a script handler, or
  fold its compatibility expansion) and the script it belongs to;
- the handler forms: the code points a script handler reads in place of a
  precomposed, uppercase or compatibility character;
- every script handler's tables, with their strings in one UTF-16 pool.

The runtime never normalizes: NFKC, case mapping, NFD and mark stripping
are all applied here. The layout of an entry is documented in
src/search/fold/FoldEngine.h and must match ENTRY_* below.

Usage: tools/foldgen.py <tables-dir> <output.inc>
"""

import argparse
import hashlib
import json
import pathlib
import sys
import unicodedata
from typing import Optional

# ---- entry layout (FoldEngine.h) --------------------------------------------

KEEP, CHAR, POOL, DROP, SCRIPT, EXPAND = range(6)

SCRIPTS = [
    "None", "Inherited", "Latin", "LatinAlt", "Greek", "Cyrillic", "CyrillicUk",
    "CyrillicBe", "CyrillicSr", "Armenian", "Georgian", "Hebrew", "Arabic", "Syriac",
    "Thaana", "Brahmic", "Thai", "Lao", "Tibetan", "Kana", "Han", "Hangul",
]
S = {name: i for i, name in enumerate(SCRIPTS)}

PAYLOAD_BITS = 17
MAX_LEN = 63


def pack(kind: int, script: int, payload: int = 0, length: int = 0, decomp: bool = False) -> int:
    assert 0 <= payload < (1 << PAYLOAD_BITS), payload
    assert 0 <= length <= MAX_LEN, length
    return kind | (script << 3) | (int(decomp) << 8) | (length << 9) | (payload << 15)


def unpack(entry: int) -> tuple[int, int, int, int, int]:
    return entry & 7, (entry >> 3) & 31, (entry >> 8) & 1, (entry >> 9) & 63, entry >> 15


# ---- input -------------------------------------------------------------------


def load_json(path: pathlib.Path) -> dict:
    def reject_duplicates(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                sys.exit(f"{path}: duplicate key {key!r}")
            seen[key] = value
        return seen

    with open(path, encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=reject_duplicates)


def load_tsv(path: pathlib.Path) -> list[tuple[str, str]]:
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            key, value = line.split("\t")
            rows.append((key, value))
    return rows


def without_doc(table: dict) -> dict:
    return {k: v for k, v in table.items() if k != "_doc"}


# ---- string pool -------------------------------------------------------------


class Pool:
    """UTF-16 string pool; identical strings share one slice."""

    def __init__(self) -> None:
        self.units: list[int] = []
        self.index: dict[str, tuple[int, int]] = {}

    def add(self, text: str) -> tuple[int, int]:
        if text in self.index:
            return self.index[text]
        raw = text.encode("utf-16-le")
        units = [int.from_bytes(raw[i:i + 2], "little") for i in range(0, len(raw), 2)]
        ref = (len(self.units), len(units))
        self.units.extend(units)
        self.index[text] = ref
        return ref

    def ref(self, text: str) -> str:
        off, length = self.add(text)
        return f"{{{off}, {length}}}"


# ---- Unicode helpers ---------------------------------------------------------


def name(ch: str) -> str:
    return unicodedata.name(ch, "")


def in_ranges(cp: int, ranges) -> bool:
    return any(lo <= cp <= hi for lo, hi in ranges)


# Marks that decorate a letter without changing which letter it is; dropped
# after NFD. Script-specific marks (Indic vowel signs, Hebrew points, the kana
# voicing marks) are not here: their scripts read them.
GENERIC_MARK_RANGES = [
    (0x0300, 0x036F), (0x0483, 0x0489), (0x1AB0, 0x1AFF), (0x1DC0, 0x1DFF),
    (0x20D0, 0x20FF), (0xFE20, 0xFE2F), (0x302A, 0x302F), (0x180B, 0x180D), (0x180F, 0x180F),
]

HAN_RANGES = [
    (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0x20000, 0x2A6DF), (0x2A700, 0x2B73F),
    (0x2B740, 0x2B81F), (0x2B820, 0x2CEAF), (0x2CEB0, 0x2EBEF), (0x2EBF0, 0x2EE5F),
    (0x2F800, 0x2FA1F), (0x30000, 0x3134F), (0x31350, 0x323AF),
]
HAN_ITERATION = {0x3005, 0x303B}

KANA_RANGES = [(0x3041, 0x3096), (0x3099, 0x309F), (0x30A1, 0x30FA), (0x30FC, 0x30FF), (0x31F0, 0x31FF)]
HANGUL_SYLLABLES = (0xAC00, 0xD7A3)


def is_generic_mark(ch: str) -> bool:
    return in_ranges(ord(ch), GENERIC_MARK_RANGES)


def is_dropped_control(ch: str) -> bool:
    cat = unicodedata.category(ch)
    return (cat == "Cc" and ch not in "\t\n\x0b\x0c\r\x85") or cat == "Cf"


def is_han(cp: int) -> bool:
    return in_ranges(cp, HAN_RANGES) and unicodedata.category(chr(cp)) == "Lo"


# ---- the generator -----------------------------------------------------------


class Generator:
    def __init__(self, tables: pathlib.Path) -> None:
        self.tables = tables
        self.pool = Pool()
        self.latin = load_json(tables / "latin.json")
        self.punct = without_doc(load_json(tables / "punctuation.json")["map"])
        self.greek = load_json(tables / "greek.json")
        self.cyrillic = load_json(tables / "cyrillic.json")
        self.armenian = load_json(tables / "armenian.json")
        self.georgian = load_json(tables / "georgian.json")
        self.kana = load_json(tables / "kana.json")
        self.hangul = load_json(tables / "hangul.json")
        self.han = load_json(tables / "han.json")
        self.other = load_json(tables / "other.json")
        self.thai = load_json(tables / "thai.json")
        self.brahmic = load_json(tables / "brahmic.json")
        self.abjad = load_json(tables / "abjad.json")
        self.brahmic_blocks = [(name, int(b["first"], 16), int(b["last"], 16), b["reading"])
                               for name, b in self.brahmic["blocks"].items()]

        self.german = without_doc(self.latin["german"])
        self.u_as_i = without_doc(self.latin["u_as_i"])
        self.cyrillic_letters = self.cyrillic["letters"]
        self.language_of: dict[str, str] = {}
        for lang, letters in self.cyrillic["language_letters"].items():
            for letter in letters:
                self.language_of[letter] = lang
        self.cyrillic_by_name = {name(ch).replace("CYRILLIC SMALL LETTER ", ""): ch
                                 for ch in self.cyrillic_letters}
        self.georgian_by_name = {name(ch).replace("GEORGIAN LETTER ", ""): ch
                                 for ch in self.georgian["letters"]}

        # Plain letter tables consulted by transliterate(), after NFKC,
        # lowercasing and NFD have run.
        surfaces = [self.abjad[s].get("surface", {}) for s in ("arabic", "hebrew", "syriac")]
        drops = [{ch: "" for ch in self.abjad[s].get("drop", "")} for s in ("arabic", "hebrew", "syriac")]
        self.letters: dict[str, str] = {}
        for table in (without_doc(self.latin["letters"]), self.punct, self.greek["letters"],
                      self.cyrillic_letters, self.georgian["letters"], self.armenian["punctuation"],
                      self.other["ol_chiki"], *surfaces, *drops):
            for key, value in table.items():
                if key in self.letters and self.letters[key] != value:
                    sys.exit(f"conflicting readings for {key!r}")
                self.letters[key] = value

        self.simplified: dict[str, str] = {}
        for simple, traditional in load_tsv(tables.parent / "han_simplified.tsv"):
            for ch in traditional:
                self.simplified[ch] = simple
        self.kana_ids: dict[str, int] = {}

    # -- transliteration of one normalized, lowercase, mark-free character --

    def transliterate(self, ch: str) -> Optional[str]:
        if ch in self.letters:
            return self.letters[ch]
        digit = unicodedata.decimal(ch, None)
        if digit is not None:
            return str(digit)
        n = name(ch)
        for rule in (self.latin_by_name, self.cyrillic_by_base, self.georgian_by_base,
                     self.syllable_by_name):
            value = rule(ch, n)
            if value is not None:
                return value
        return None

    SHAPE_WORDS = {
        "SMALL", "CAPITAL", "TURNED", "REVERSED", "INVERTED", "SCRIPT", "CLOSED", "OPEN", "BARRED",
        "DOTLESS", "LONG", "SIDEWAYS", "INSULAR", "MIDDLE-WELSH", "BLACKLETTER", "STRETCHED",
        "BROKEN", "BOTTOM", "TOP", "HALF", "LEFT", "RIGHT", "ROUND", "VOLAPUK", "OLD", "POLISH",
        "LIGATURE", "SUBSCRIPT", "RETROFLEX", "MODIFIER", "LETTER", "DIGRAPH", "VISIGOTHIC",
    }

    def latin_by_name(self, ch: str, n: str) -> Optional[str]:
        if not n.startswith("LATIN "):
            return None
        stem = n.split(" WITH ")[0].split()[1:]
        named = " ".join(w for w in stem if w not in {"SMALL", "CAPITAL", "LETTER", "LIGATURE"})
        if named in self.latin["named"]:
            return self.latin["named"][named]
        words = [w for w in stem if w not in self.SHAPE_WORDS]
        if len(words) == 1 and words[0].isalpha() and words[0].isascii() and len(words[0]) <= 3:
            return words[0].lower()
        joined = " ".join(words)
        return self.latin["named"].get(joined)

    CYRILLIC_QUALIFIERS = {"ABKHASIAN", "KOMI", "BASHKIR", "ALEUT", "BYELORUSSIAN-UKRAINIAN",
                           "SEMISOFT", "OLD", "SLAVONIC", "CLOSED", "REVERSED", "BARRED", "ROUNDED",
                           "IOTIFIED", "DOUBLE", "BROAD", "MONOGRAPH", "MONOCULAR", "BINOCULAR",
                           "DOUBLE MONOCULAR", "MULTIOCULAR", "CROSSED", "TALL", "NARROW", "SOFT"}

    def cyrillic_by_base(self, ch: str, n: str) -> Optional[str]:
        if not n.startswith("CYRILLIC SMALL LETTER "):
            return None
        stem = n[len("CYRILLIC SMALL LETTER "):].split(" WITH ")[0]
        words = stem.split()
        for start in range(len(words)):
            key = " ".join(words[start:])
            if key in self.cyrillic_by_name:
                return self.cyrillic_letters[self.cyrillic_by_name[key]]
        words = [w for w in words if w not in self.CYRILLIC_QUALIFIERS]
        key = " ".join(words)
        base = self.cyrillic_by_name.get(key)
        return self.cyrillic_letters[base] if base else None

    def georgian_by_base(self, ch: str, n: str) -> Optional[str]:
        if not n.startswith("GEORGIAN SMALL LETTER "):
            return None
        base = self.georgian_by_name.get(n[len("GEORGIAN SMALL LETTER "):])
        return self.georgian["letters"][base] if base else None

    @staticmethod
    def syllable_by_name(ch: str, n: str) -> Optional[str]:
        """Scripts whose character names spell the sound: syllabaries and a few alphabets."""
        cp = ord(ch)
        words = n.split()
        if n.startswith(("NEGATIVE CIRCLED LATIN CAPITAL LETTER ", "NEGATIVE SQUARED LATIN CAPITAL LETTER ",
                         "REGIONAL INDICATOR SYMBOL LETTER ")):
            return words[-1].lower()
        if n.startswith("ETHIOPIC SYLLABLE "):
            tokens = [w for w in words[2:] if w not in {"SEBATBEIT", "GURAGE", "BENCH", "OROMO"}]
            vowel_only = tokens[0] in {"GLOTTAL", "PHARYNGEAL"}
            syl = tokens[-1]
            if syl.endswith("EE"):
                syl = syl[:-2] + "E"
            elif syl.endswith("AA"):
                syl = syl[:-2] + "A"
            elif syl.endswith("E") and len(syl) > 1 and syl[-2] not in "AEIOU" and not vowel_only:
                syl = syl[:-1]
            return syl.lower()
        if n.startswith(("CHEROKEE LETTER ", "CHEROKEE SMALL LETTER ", "YI SYLLABLE ", "VAI SYLLABLE ")):
            return words[-1].lower() if words[-1].isalpha() else None
        if n.startswith("CANADIAN SYLLABICS "):
            last = words[-1]
            stop = {"DOT", "RING", "ACUTE", "GRAVE", "BOTTOM", "TOP", "HALF", "SMALL", "RAISED",
                    "DOUBLE", "HYPHEN", "FULL", "STOP", "SYLLABICS", "MIDDLE", "HOOK", "CROSS",
                    "TRIANGLE", "BAR", "DIAMOND", "SEPARATOR"}
            return last.lower() if last.isalpha() and len(last) <= 4 and last not in stop else None
        if n.startswith(("BOPOMOFO LETTER ", "BOPOMOFO FINAL LETTER ")):
            sound = words[-1]
            special = {"IU": "u", "EH": "e", "AU": "ao", "IH": "i"}
            return special.get(sound, sound.lower())
        if n.startswith("MONGOLIAN LETTER "):
            tokens = [w for w in words[2:] if w not in {"TODO", "SIBE", "MANCHU", "ALI", "GALI"}]
            if not tokens:
                return None
            sound = tokens[-1]
            vowels = {"A": "a", "E": "e", "I": "i", "O": "o", "U": "u", "OE": "o", "UE": "u", "EE": "e"}
            if sound in vowels:
                return vowels[sound]
            special = {"ANG": "ng", "HAA": "h", "ZHI": "zh", "CHI": "ch"}
            if sound in special:
                return special[sound]
            return sound[:-1].lower() if sound.endswith("A") and len(sound) > 1 else sound.lower()
        if n.startswith(("NKO LETTER ", "LISU LETTER ")):
            tokens = [w for w in words[2:] if w not in {"WOLOSO", "JONA", "TONE"}]
            if not tokens or not tokens[-1].isalpha():
                return None
            sound = tokens[0] if n.startswith("NKO") else tokens[-1]
            vowels = {"A", "EE", "I", "E", "U", "OO", "O", "AE", "EU", "UE", "UH", "OE"}
            if sound == "DAGBASINNA":
                return "e"
            if sound in vowels:
                return sound[0].lower()
            return sound[:-1].lower() if sound.endswith("A") and len(sound) > 1 else sound.lower()
        if n.startswith("TIFINAGH LETTER "):
            sound = words[-1]
            base = {"YA": "a", "YI": "i", "YU": "u", "YEY": "e", "YAA": "", "YE": "e", "YO": "o"}
            if sound in base:
                return base[sound]
            if sound.startswith("YA") and len(sound) > 2:
                cons = sound[2:]
                if len(cons) == 2 and cons[0] == cons[1]:
                    cons = cons[0]
                return cons.lower()
            return None
        if n.startswith("HENTAIGANA LETTER "):
            return words[-1].split("-")[0].lower()
        if cp > 0xFFFF and n.startswith(("HIRAGANA LETTER ", "KATAKANA LETTER ")):
            sound = words[-1]
            return sound.lower() if sound.isalpha() else None
        if n.startswith("RUNIC LETTER "):
            last = words[-1]
            return last.lower() if last.isalpha() and len(last) <= 2 else None
        return None

    # -- primary fold of a code point ----------------------------------------

    def handler_script(self, ch: str) -> Optional[int]:
        """The script whose handler reads this code point in every pass, or None."""
        cp = ord(ch)
        if cp in (0x309B, 0x309C):
            return S["Kana"]
        if in_ranges(cp, KANA_RANGES) or ch in self.kana["katakana"]:
            # Digraphs such as ゟ and ヿ fold through their NFKC expansion.
            return S["Kana"] if len(unicodedata.normalize("NFKC", ch)) == 1 else None
        if HANGUL_SYLLABLES[0] <= cp <= HANGUL_SYLLABLES[1] or 0x1100 <= cp <= 0x11FF:
            return S["Hangul"]
        if cp in HAN_ITERATION:
            return S["Han"]
        low = ch.lower()
        if len(low) == 1 and low in self.armenian["letters"]:
            return S["Armenian"]
        return None

    def primary_text(self, ch: str, depth: int = 0) -> Optional[str]:
        """Primary fold of one character, or None when a handler has to read it."""
        assert depth < 8, ch
        if self.handler_script(ch) is not None:
            return None
        if is_dropped_control(ch):
            return ""
        if ch in self.punct:
            return self.punct[ch]
        nfkc = unicodedata.normalize("NFKC", ch)
        if nfkc != ch:
            return self.join(self.primary_text(c, depth + 1) for c in nfkc)
        if is_han(ord(ch)):
            return self.simplified.get(ch, ch)
        low = ch.lower()
        if low != ch:
            return self.join(self.primary_text(c, depth + 1) for c in low)
        if ch in self.letters:
            return self.letters[ch]
        nfd = unicodedata.normalize("NFD", ch)
        if nfd != ch:
            return self.join(self.primary_text(c, depth + 1) for c in nfd if not is_generic_mark(c))
        if is_generic_mark(ch):
            return ""
        value = self.transliterate(ch)
        return ch if value is None else value

    @staticmethod
    def join(parts) -> Optional[str]:
        parts = list(parts)
        return None if any(p is None for p in parts) else "".join(parts)

    def script_of(self, ch: str) -> int:
        """Script id recorded in an entry: which alternate reading applies to it."""
        cp = ord(ch)
        if in_ranges(cp, GENERIC_MARK_RANGES) or cp in (0x200C, 0x200D):
            return S["Inherited"]
        nfkc = unicodedata.normalize("NFKC", ch)
        first = nfkc[0] if nfkc else ch
        if is_han(ord(first)) or ord(first) in HAN_ITERATION:
            return S["Han"]
        if unicodedata.category(first)[0] in "LM":
            for script, (lo, hi) in (("Thai", (0x0E00, 0x0E7F)), ("Lao", (0x0E80, 0x0EFF))):
                if lo <= ord(first) <= hi:
                    return S[script]
            if self.brahmic_block(ord(ch)) is not None:
                return S["Brahmic"]
            abjad = self.abjad_script(ord(first))
            if abjad is not None:
                return S[abjad]
            if 0x0F00 <= ord(first) <= 0x0FFF:
                return S["Tibetan"]
        if ord(first) in (0x0F0B, 0x0F0C):
            return S["Tibetan"]
        if ord(first) == 0x05F3:
            return S["Hebrew"]
        low = first.lower()
        composed = unicodedata.normalize("NFC", low)
        if composed in self.german:
            return S["LatinAlt"]
        n = name(first)
        if n.startswith("LATIN "):
            return S["Latin"]
        if n.startswith("GREEK ") and "LETTER" in n:
            return S["Greek"]
        if n.startswith("CYRILLIC ") and "LETTER" in n:
            lang = self.language_of.get(composed)
            return S[{"uk": "CyrillicUk", "be": "CyrillicBe", "sr": "CyrillicSr"}[lang]] if lang else S["Cyrillic"]
        return S["None"]

    def handler_form(self, ch: str, script: int) -> str:
        """The code points a script handler reads for `ch`."""
        if script == S["Greek"]:
            return unicodedata.normalize("NFD", unicodedata.normalize("NFKC", ch).lower())
        if script in (S["Cyrillic"], S["CyrillicUk"], S["CyrillicBe"], S["CyrillicSr"],
                      S["Armenian"], S["LatinAlt"]):
            return unicodedata.normalize("NFC", unicodedata.normalize("NFKC", ch).lower())
        if script == S["Han"]:
            return unicodedata.normalize("NFKC", ch)
        if script == S["Lao"] and ord(ch) in (0x0EDC, 0x0EDD):
            return unicodedata.normalize("NFKC", ch)
        if script in (S["Arabic"], S["Hebrew"]) and ord(ch) >= 0xFB1D:
            return unicodedata.normalize("NFKC", ch)
        if script == S["Tibetan"]:
            return unicodedata.normalize("NFD", ch)
        return ch

    def entry_for(self, cp: int) -> tuple[int, Optional[str]]:
        """(entry, handler form or None) for one code point."""
        ch = chr(cp)
        if 0xD800 <= cp <= 0xDFFF:
            return pack(KEEP, S["None"]), None
        if cp < 0x80:
            if is_dropped_control(ch):
                return pack(DROP, S["None"]), None
            if "A" <= ch <= "Z":
                return pack(CHAR, S["Latin"], ord(ch.lower())), None
            return pack(KEEP, S["Latin"] if ch.isalpha() else S["None"]), None

        handler = self.handler_script(ch)
        if handler is not None:
            return self.handler_entry(ch, handler)
        nfkc = unicodedata.normalize("NFKC", ch)
        if len(nfkc) == 1 and nfkc != ch and self.handler_script(nfkc) is not None:
            return self.handler_entry(nfkc, self.handler_script(nfkc))

        text = self.primary_text(ch)
        script = self.script_of(ch)
        if text is None:
            off, length = self.pool.add(nfkc)
            return pack(EXPAND, S["None"], off, length), None
        form = None
        if script in (S["Greek"], S["Cyrillic"], S["CyrillicUk"], S["CyrillicBe"], S["CyrillicSr"],
                      S["LatinAlt"], S["Han"], S["Lao"], S["Arabic"], S["Hebrew"], S["Tibetan"]):
            candidate = self.handler_form(ch, script)
            if candidate != ch:
                form = candidate
        if text == ch:
            entry = pack(KEEP, script, decomp=form is not None)
        elif text == "":
            entry = pack(DROP, script, decomp=form is not None)
        elif len(text) == 1 and ord(text) < 0x10000:
            entry = pack(CHAR, script, ord(text), decomp=form is not None)
        else:
            off, length = self.pool.add(text)
            entry = pack(POOL, script, off, length, decomp=form is not None)
        return entry, form

    def handler_entry(self, ch: str, script: int) -> tuple[int, Optional[str]]:
        cp = ord(ch)
        if script == S["Kana"]:
            return pack(SCRIPT, script, self.kana_id(ch)), None
        if script == S["Hangul"]:
            return pack(SCRIPT, script, self.hangul_payload(cp)), None
        if script == S["Han"]:
            return pack(SCRIPT, script), None
        if script == S["Armenian"]:
            low = unicodedata.normalize("NFC", ch.lower())
            return pack(SCRIPT, script, decomp=low != ch), (low if low != ch else None)
        raise AssertionError(ch)

    # -- abjads ------------------------------------------------------------------

    ABJAD_BLOCKS = [
        ("Arabic", 0x0600, 0x06FF), ("Arabic", 0x0750, 0x077F), ("Arabic", 0x08A0, 0x08FF),
        ("Arabic", 0xFB50, 0xFDFF), ("Arabic", 0xFE70, 0xFEFF), ("Hebrew", 0x0590, 0x05FF),
        ("Hebrew", 0xFB1D, 0xFB4F), ("Syriac", 0x0700, 0x074F), ("Syriac", 0x0860, 0x086F),
        ("Thaana", 0x0780, 0x07BF),
    ]
    ABJAD_READER_BLOCKS = [("Arabic", 0x0600, 0x06FF), ("Arabic", 0x0750, 0x077F), ("Arabic", 0x08A0, 0x08FF),
                           ("Hebrew", 0x0590, 0x05FF), ("Syriac", 0x0700, 0x074F), ("Syriac", 0x0860, 0x086F),
                           ("Thaana", 0x0780, 0x07BF)]

    def abjad_script(self, cp: int) -> Optional[str]:
        for script, lo, hi in self.ABJAD_BLOCKS:
            if lo <= cp <= hi:
                return script
        return None

    ABJAD_ROLES = {"alef": "Alef", "ain": "Ain", "waw": "Waw", "yeh": "Yeh", "maksura": "Maksura",
                   "marbuta": "Marbuta", "hamza": "Hamza", "vowel": "VowelLetter", "vav": "Waw", "yod": "Yeh",
                   "he": "He", "shin": "Shin"}
    ABJAD_MARKS = {"shadda": "Shadda", "sukun": "Sukun", "dagesh": "Dagesh", "shin": "ShinDot", "sin": "SinDot",
                   "rafe": "Rafe", "geresh": "Geresh", "sheva": "Sheva"}

    def abjad_info(self, script: str, cp: int) -> tuple[str, list[str], str, str]:
        """(role, [style 1-3 spellings], soft spelling, geresh spelling) of an abjad code point."""
        table = self.abjad[script.lower()]
        ch = chr(cp)

        def styles(value) -> list[str]:
            values = [value] if isinstance(value, str) else list(value)
            while len(values) < 3:
                values.append(values[-1])
            return values

        soft = self.abjad["hebrew"]["soft"].get(ch, "") if script == "Hebrew" else ""
        geresh = self.abjad["hebrew"]["geresh"].get(ch, "") if script == "Hebrew" else ""
        roles = table.get("roles", {})
        if ch in roles:
            role = roles[ch]
            kind, value = (role, "") if isinstance(role, str) else (role[0], role[1])
            spelled = styles(value)
            if kind == "alef" and isinstance(role, str):
                spelled = styles("a")
            if script == "Hebrew" and kind == "shin":
                spelled = styles("sh")
            return self.ABJAD_ROLES[kind], spelled, soft, geresh
        if ch in table.get("letters", {}):
            return "Consonant", styles(table["letters"][ch]), soft, geresh
        marks = table.get("marks", {})
        if ch in marks:
            mark = marks[ch]
            if mark in self.ABJAD_MARKS:
                return self.ABJAD_MARKS[mark], styles(""), "", ""
            return "Vowel", styles(mark), "", ""
        n = name(ch)
        if script == "Arabic" and n.startswith("ARABIC LETTER "):
            stem = n[len("ARABIC LETTER "):].split(" WITH ")[0]
            for words in (stem, stem.split()[-1]):
                if words in table["names"]:
                    return "Consonant", styles(table["names"][words]), "", ""
        return "None", styles(""), "", ""

    # -- Tibetan -----------------------------------------------------------------

    TIBETAN_SOUNDS = {"K": "k", "KH": "kh", "G": "g", "NG": "ng", "C": "c", "CH": "ch", "J": "j", "NY": "ny",
                      "TT": "t", "TTH": "th", "DD": "d", "NN": "n", "T": "t", "TH": "th", "D": "d", "N": "n",
                      "P": "p", "PH": "ph", "B": "b", "M": "m", "TS": "ts", "TSH": "tsh", "DZ": "dz", "W": "w",
                      "ZH": "zh", "Z": "z", "-": "'", "": "", "Y": "y", "R": "r", "L": "l", "SH": "sh", "SS": "sh",
                      "S": "s", "H": "h", "FIXED-FORM R": "r", "KK": "k", "RR": "r"}
    TIBETAN_VOWELS = {"AA": "", "I": "i", "U": "u", "E": "e", "EE": "ai", "O": "o", "OO": "au",
                      "REVERSED I": "i", "VOCALIC RR": "ri", "VOCALIC LL": "li"}

    def tibetan_info(self, cp: int) -> tuple[str, str]:
        """(role, spelling) of a Tibetan code point."""
        n = name(chr(cp))
        if cp in (0x0F0B, 0x0F0C):
            return "Tsheg", ""
        for prefix, role in (("TIBETAN LETTER ", "Base"), ("TIBETAN SUBJOINED LETTER ", "Subjoined")):
            if n.startswith(prefix):
                stem = n[len(prefix):]
                stem = stem[:-1] if stem.endswith("A") else stem
                sound = self.TIBETAN_SOUNDS.get(stem.strip())
                return (role, sound) if sound is not None else ("None", "")
        if n.startswith("TIBETAN VOWEL SIGN "):
            vowel = self.TIBETAN_VOWELS.get(n[len("TIBETAN VOWEL SIGN "):])
            return ("Vowel", vowel) if vowel is not None else ("None", "")
        if cp in (0x0F7E, 0x0F82, 0x0F83):
            return "Coda", "m"
        if cp == 0x0F7F:
            return "Coda", "h"
        return "None", ""

    # -- Brahmic -----------------------------------------------------------------

    def brahmic_block(self, cp: int):
        for block in self.brahmic_blocks:
            if block[1] <= cp <= block[2]:
                return block
        return None

    def strip_qualifiers(self, words: list[str]) -> list[str]:
        qualifiers = set(self.brahmic["qualifiers"])
        return [w for w in words if w not in qualifiers]

    def brahmic_vowel(self, stem: str) -> Optional[str]:
        stem = " ".join(self.strip_qualifiers(stem.split()))
        return self.brahmic["vowels"].get(stem)

    def brahmic_info(self, cp: int) -> tuple[str, str, str, str]:
        """(role, inherent vowel, spelling, alternate spelling) of a Brahmic code point."""
        ch = chr(cp)
        block = self.brahmic_block(cp)
        script = block[0]
        if unicodedata.category(ch)[0] not in "LM":
            return "None", "", "", ""
        if script == "SINHALA":
            table = self.brahmic["sinhala"]
            if ch in table["consonants"]:
                return "Consonant", "a", table["consonants"][ch], table["consonants"][ch]
            if ch in table["vowels"]:
                return "Vowel", "", table["vowels"][ch], ""
            if ch in table["signs"]:
                return "Sign", "", table["signs"][ch], ""
        n = name(ch)
        rest = n[len(script) + 1:]
        if rest.startswith("LETTER "):
            stem = rest[len("LETTER "):]
            if stem.startswith("CHILLU "):
                sound = self.brahmic["consonants"].get(stem[len("CHILLU "):], stem[len("CHILLU "):].lower())
                return "Final", "", sound, sound
            if stem in ("KHANDA TA", "DOT REPH"):
                sound = "t" if stem == "KHANDA TA" else "r"
                return "Final", "", sound, sound
            if script == "MYANMAR" and stem == "A":
                return "Consonant", "a", "", ""
            if script == "KHMER" and stem == "QA":
                return "Consonant", "a", self.brahmic["consonants"]["Q_KHMER"], ""
            if " ".join(self.strip_qualifiers(stem.split())) in self.brahmic["vowel_letters"]:
                return "Vowel", "", self.brahmic_vowel(stem), ""
            words = self.strip_qualifiers(stem.split())
            stem = " ".join(words)
            inherent = "a"
            if script == "KHMER" and stem.endswith("O"):
                base, inherent = stem[:-1], "o"
            elif stem.endswith("A"):
                base = stem[:-1]
            else:
                base = stem
            if script == "KHMER" and base == "NY":
                base = "NY_KHMER"
            sound = self.brahmic["consonants"].get(base.strip(), base.strip().lower().replace(" ", ""))
            alt = "w" if sound == "v" and block[3] in ("north", "bengali") else sound
            return "Consonant", inherent, sound, alt
        if rest.startswith("VOWEL SIGN "):
            vowel = self.brahmic_vowel(rest[len("VOWEL SIGN "):])
            return ("Sign", "", vowel, "") if vowel is not None else ("None", "", "", "")
        if rest.startswith("INDEPENDENT VOWEL "):
            vowel = self.brahmic_vowel(rest[len("INDEPENDENT VOWEL "):])
            return ("Vowel", "", vowel, "") if vowel is not None else ("None", "", "", "")
        if rest.startswith("CONSONANT SIGN MEDIAL "):
            spelled = self.brahmic["medials"].get(rest.split()[-1])
            return ("Medial", "", spelled[0], spelled[1]) if spelled else ("None", "", "", "")
        roles = {
            "SIGN VIRAMA": "Virama", "SIGN ASAT": "Virama", "SIGN COENG": "Virama", "SIGN AL-LAKUNA": "Virama",
            "SIGN VIRIAM": "Virama", "SIGN ANUSVARA": "Anusvara", "SIGN NIKAHIT": "Anusvara",
            "SIGN BINDI": "Anusvara", "TIPPI": "Anusvara", "SIGN ANUSVARAYA": "Anusvara",
            "SIGN CANDRABINDU": "Candrabindu", "SIGN VISARGA": "Visarga", "SIGN REAHMUK": "Visarga",
            "SIGN VISARGAYA": "Visarga", "SIGN NUKTA": "Nukta", "ADDAK": "Geminate",
        }
        role = roles.get(rest, "None")
        if script == "MYANMAR" and role == "Visarga":
            role = "None"  # a tone mark in Burmese
        if rest == "OM":
            return "Vowel", "", "om", ""
        return role, "", "", ""

    # -- kana ------------------------------------------------------------------

    def build_kana(self) -> None:
        """Assign kana ids and build the kana table."""
        rows: list[dict] = [dict(kind="None", reading=("", ""), consonant="", combine="", vowel="",
                                 voiced=0, semivoiced=0, hira=0)]
        by_char: dict[str, int] = {}

        def reading(value) -> tuple[str, str]:
            return (value, value) if isinstance(value, str) else (value[0], value[1])

        kana = self.kana["kana"]
        small = self.kana["small"]
        combine = self.kana["combine"]
        for ch in sorted(set(kana) | set(small)):
            if ch in kana:
                wapuro, hepburn = reading(kana[ch])
                vowel = wapuro[-1] if wapuro and wapuro[-1] in "aiueo" else ""
                consonant = wapuro[:-1] if vowel else wapuro
                row = dict(kind="Syllable", reading=(wapuro, hepburn), consonant=consonant,
                           combine=combine.get(ch, consonant), vowel=vowel, voiced=0, semivoiced=0,
                           hira=ord(ch))
                if ch == "ん":
                    row["kind"] = "Moraic"
            else:
                value = small[ch]
                row = dict(kind="Small" if value not in ("ka", "ke") else "Syllable",
                           reading=(value, value), consonant="", combine="", vowel=value[-1],
                           voiced=0, semivoiced=0, hira=ord(ch))
            by_char[ch] = len(rows)
            rows.append(row)
        for ch, value in self.kana["katakana"].items():
            by_char[ch] = len(rows)
            vowel = value[-1] if value[-1] in "aiueo" else ""
            rows.append(dict(kind="Syllable", reading=(value, value),
                             consonant=value[:-1] if vowel else value, combine=value[:-1] if vowel else value,
                             vowel=vowel, voiced=0, semivoiced=0, hira=0))
        for kind, chars in (("Sokuon", "っ"), ("Long", "ー"), ("Iteration", "ゝ"),
                            ("VoicedIteration", "ゞ"), ("Dakuten", "゙"), ("Handakuten", "゚")):
            by_char[chars] = len(rows)
            rows.append(dict(kind=kind, reading=("", ""), consonant="", combine="", vowel="",
                             voiced=0, semivoiced=0, hira=ord(chars)))
        for ch, row_id in list(by_char.items()):
            row = rows[row_id]
            if row["kind"] not in ("Syllable", "Moraic"):
                continue
            for mark, field in (("゙", "voiced"), ("゚", "semivoiced")):
                composed = unicodedata.normalize("NFC", ch + mark)
                if len(composed) == 1 and composed in by_char:
                    row[field] = by_char[composed]
        # Katakana and the remaining marks share their hiragana rows.
        aliases = {"ゕ": "ゕ", "ゖ": "ゖ", "゛": "゙", "゜": "゚",
                   "ヽ": "ゝ", "ヾ": "ゞ", "ゟ": None, "ヿ": None, "ゐ": "ゐ"}
        for cp in range(0x30A1, 0x30F7):
            kata = chr(cp)
            hira = chr(cp - 0x60)
            if hira in by_char:
                by_char[kata] = by_char[hira]
        for kata, hira in aliases.items():
            if hira is not None and hira in by_char:
                by_char[kata] = by_char[hira]
        self.kana_rows = rows
        self.kana_ids = by_char

    def kana_id(self, ch: str) -> int:
        if ch in self.kana_ids:
            return self.kana_ids[ch]
        nfkc = unicodedata.normalize("NFKC", ch)
        if nfkc in self.kana_ids:
            return self.kana_ids[nfkc]
        if len(nfkc) == 2 and nfkc[0] == " " and nfkc[1] in self.kana_ids:
            return self.kana_ids[nfkc[1]]
        return 0

    # -- Hangul ------------------------------------------------------------------

    @staticmethod
    def hangul_payload(cp: int) -> int:
        """0 for a syllable; 1 + kind * 32 + index for a modern jamo; 127 for others."""
        if HANGUL_SYLLABLES[0] <= cp <= HANGUL_SYLLABLES[1]:
            return 0
        jamo = unicodedata.normalize("NFKC", chr(cp))
        j = ord(jamo[0]) if len(jamo) == 1 else 0
        if 0x1100 <= j <= 0x1112:
            return 1 + 0 * 32 + (j - 0x1100)
        if 0x1161 <= j <= 0x1175:
            return 1 + 1 * 32 + (j - 0x1161)
        if 0x11A8 <= j <= 0x11C2:
            return 1 + 2 * 32 + (j - 0x11A7)
        return 127

    # -- whole tables ------------------------------------------------------------

    def build(self) -> str:
        self.build_kana()
        bmp: list[int] = []
        forms: dict[int, str] = {}
        for cp in range(0x10000):
            entry, form = self.entry_for(cp)
            bmp.append(entry)
            if form is not None:
                forms[cp] = form
        # Compatibility jamo and half-width Hangul fold through NFKC onto jamo;
        # make sure they are handler-owned too.
        for cp in list(range(0x3131, 0x318F)) + list(range(0xFFA0, 0xFFDD)):
            if self.hangul_payload(cp) != 127:
                bmp[cp] = pack(SCRIPT, S["Hangul"], self.hangul_payload(cp))
        astral: list[tuple[int, int]] = []
        for cp in range(0x10000, 0x110000):
            ch = chr(cp)
            if unicodedata.category(ch) == "Cn":
                continue
            # Most astral ideographs fold to themselves and are answered by the
            # range fallback; those with a simplified form need an entry.
            if is_han(cp) and not 0x2F800 <= cp <= 0x2FA1F and ch not in self.simplified:
                continue
            entry, form = self.entry_for(cp)
            kind, script, _, _, _ = unpack(entry)
            if kind == KEEP and script == S["None"] and form is None:
                continue
            astral.append((cp, entry))
            if form is not None:
                forms[cp] = form
        return self.emit(bmp, astral, forms)

    # -- C++ emission --------------------------------------------------------------

    def emit(self, bmp: list[int], astral: list[tuple[int, int]], forms: dict[int, str]) -> str:
        pool = self.pool
        out: list[str] = []
        w = out.append

        decomp_rows = [f"    {{0x{cp:X}, {pool.ref(form)}}}," for cp, form in sorted(forms.items())]

        # Kana
        kana_rows = []
        for row in self.kana_rows:
            voiced = row["voiced"]
            vowel = f"u'{row['vowel']}'" if row["vowel"] else "0"
            kana_rows.append(
                f"    {{KanaKind::{row['kind']}, {pool.ref(row['reading'][0])}, {pool.ref(row['reading'][1])}, "
                f"{pool.ref(row['consonant'])}, {pool.ref(row['combine'])}, "
                f"{vowel}, {voiced}, {row['semivoiced']}, 0x{row['hira']:X}}},")

        # Hangul
        def str_array(values: list[str]) -> str:
            return "{" + ", ".join(pool.ref(v) for v in values) + "}"

        coda_order = ["k", "n", "t", "l", "m", "p", "ng"]
        rr, mr = self.hangul["rr"], self.hangul["mr"]
        hangul_lines = [
            f"const HangulStyle kHangulRr = {{{str_array(rr['initials'])}, {str_array(rr['initials'])}, "
            f"{str_array(rr['vowels'])}, {str_array([rr['codas'][c] for c in coda_order])}, {pool.ref(rr['lateral'])}}};",
            f"const HangulStyle kHangulMr = {{{str_array(mr['initials'])}, {str_array(mr['voiced_initials'])}, "
            f"{str_array(mr['vowels'])}, {str_array([mr['codas'][c] for c in coda_order])}, {pool.ref(mr['lateral'])}}};",
        ]

        # Han
        pinyin_syllables: list[str] = []
        syllable_id: dict[str, int] = {}
        reading_of: dict[str, str] = {}
        for reading, chars in load_tsv(self.tables.parent / "han_pinyin.tsv"):
            for ch in chars:
                reading_of[ch] = reading
        reading_of.update(self.han["chinese"]["chars"])

        def pinyin_id(reading: str) -> int:
            if reading not in syllable_id:
                syllable_id[reading] = len(pinyin_syllables) + 1
                pinyin_syllables.append(reading)
            return syllable_id[reading]

        bmp_pinyin = [0] * (0xA000 - 0x3400)
        astral_pinyin: list[tuple[int, int]] = []
        for ch, reading in sorted(reading_of.items()):
            cp = ord(ch)
            if 0x3400 <= cp < 0xA000:
                bmp_pinyin[cp - 0x3400] = pinyin_id(reading)
            elif cp >= 0x10000:
                astral_pinyin.append((cp, pinyin_id(reading)))

        def words(table: dict, split: bool) -> tuple[list[str], int]:
            rows = []
            longest = 0
            for key in sorted(table, key=lambda k: k.encode("utf-16-be")):
                value = table[key]
                if split:
                    syllables = value.split()
                    if len(syllables) != len(key):
                        sys.exit(f"han.json: {key} has {len(key)} characters but {len(syllables)} syllables")
                    if all(reading_of.get(c) == s for c, s in zip(key, syllables)):
                        continue
                rows.append(f"    {{{pool.ref(key)}, {pool.ref(value)}}},")
                longest = max(longest, len(key.encode("utf-16-le")) // 2)
            return rows, longest

        japanese_rows, japanese_longest = words(self.han["japanese"], False)
        chinese_rows, chinese_longest = words(self.han["chinese"]["words"], True)

        # Greek
        greek_styles = []
        for style in self.greek["styles"]:
            letters = [style["letters"].get(chr(cp), self.greek["letters"].get(chr(cp), ""))
                       for cp in range(0x03B1, 0x03CA)]
            pairs = []
            for key, value in style["pairs"].items():
                initial, medial = (value, value) if isinstance(value, str) else value
                kind = "GreekPairKind::Plain" if isinstance(value, str) else "GreekPairKind::Initial"
                pairs.append((key, initial, medial, kind))
            for key, (voiced, voiceless) in style["u_diphthongs"].items():
                pairs.append((key, voiced, voiceless, "GreekPairKind::Voicing"))
            pair_text = ", ".join(f"{{u'{k[0]}', u'{k[1]}', {pool.ref(a)}, {pool.ref(b)}, {kind}}}"
                                  for k, a, b, kind in pairs)
            greek_styles.append(
                f"    {{{str_array(letters)}, {{{pair_text}}}, {len(pairs)}, {pool.ref(style.get('front_gamma', ''))}}},")

        # Alphabet rules: Cyrillic styles and languages, Armenian, German.
        alpha_rows: list[str] = []

        def alpha_range(table: dict) -> str:
            begin = len(alpha_rows)
            items = []
            for key, value in table.items():
                if key == "_doc":
                    continue
                initial, medial = (value, value) if isinstance(value, str) else value
                first = key[0]
                second = key[1] if len(key) > 1 else "\0"
                items.append((ord(first), ord(second), initial, medial))
            for first, second, initial, medial in sorted(items):
                alpha_rows.append(f"    {{0x{first:X}, 0x{second:X}, {pool.ref(initial)}, {pool.ref(medial)}}},")
            return f"{{{begin}, {len(alpha_rows) - begin}}}"

        cyr_styles = []
        for style in self.cyrillic["styles"]:
            langs = style.get("languages", {})
            cyr_styles.append("    {" + ", ".join([alpha_range(style["map"]),
                                                  alpha_range(langs.get("uk", {})),
                                                  alpha_range(langs.get("be", {})),
                                                  alpha_range(langs.get("sr", {}))]) + "},")
        armenian_eastern = alpha_range({**self.armenian["letters"], **self.armenian["pairs"]})
        armenian_western = alpha_range(self.armenian["western"])
        german = alpha_range(self.german)
        u_as_i = alpha_range(self.u_as_i)

        # Thai and Lao
        vowel_names = ["A", "I", "II", "UE", "UEE", "U", "UU", "E", "AE", "O", "OO", "IA", "UEA", "UA", "OE",
                       "OEI", "AI", "AO", "AM", "IMPLICIT_A", "IMPLICIT_O", "RUE", "LUE", "RI"]
        enum_names = {"IMPLICIT_A": "ImplicitA", "IMPLICIT_O": "ImplicitO"}
        thai_vowels = self.thai["vowels"]
        vowel_rows = [f"    {{{pool.ref(thai_vowels[v][0])}, {pool.ref(thai_vowels[v][1])}}}," for v in vowel_names]
        role_names = {"pre": "Pre", "above": "Above", "post": "Post", "tone": "Tone", "silencer": "Silencer",
                      "repeat": "Repeat", "letter": "Letter"}

        def thai_block(table: dict, base: int, lao: bool) -> list[str]:
            rows = []
            for cp in range(base, base + 128):
                ch = chr(cp)
                role, vowel, thai_eq, values = "None", "A", cp, ["", "", "", ""]
                if ch in table["consonants"]:
                    row = table["consonants"][ch]
                    if lao:
                        thai_eq, values = ord(row[0]), row[1:]
                    else:
                        values = row
                    role = "Cluster" if ch == "ຼ" else "Consonant"
                elif ch in table["marks"]:
                    mark = table["marks"][ch]
                    kind, detail = (mark, None) if isinstance(mark, str) else (mark[0], mark[1])
                    role = role_names[kind]
                    if detail in thai_vowels:
                        vowel = detail
                    elif detail is not None:
                        thai_eq = ord(detail)
                rows.append(f"    {{ThaiRole::{role}, ThaiVowel::{enum_names.get(vowel, vowel)}, 0x{thai_eq:X}, "
                            f"{{{pool.ref(values[0])}, {pool.ref(values[2])}}}, "
                            f"{{{pool.ref(values[1])}, {pool.ref(values[3])}}}}},")
            return rows

        thai_rows = thai_block(self.thai["thai"], 0x0E00, False)
        thai_word_table = {}
        for key, value in self.thai["words"].items():
            readings = [value] if isinstance(value, str) else list(value)
            thai_word_table[key] = " ".join(readings)
        thai_word_rows, thai_longest = words(thai_word_table, False)
        lao_rows = thai_block(self.thai["lao"], 0x0E80, True)

        # Brahmic
        brahmic_rows: list[str] = []
        brahmic_block_rows: list[str] = []
        merges: list[str] = []
        nukta_of: dict[int, str] = {}
        for _, first, last, _ in self.brahmic_blocks:
            for cp in range(first, last + 1):
                decomposed = unicodedata.normalize("NFD", chr(cp))
                if len(decomposed) == 2 and name(decomposed[1]).endswith("SIGN NUKTA"):
                    role, _, sound, _ = self.brahmic_info(cp)
                    if role == "Consonant":
                        nukta_of[ord(decomposed[0])] = sound
        readings = {"plain": "Plain", "north": "North", "bengali": "Bengali", "south": "South", "khmer": "Khmer"}
        for block_name, first, last, reading in self.brahmic_blocks:
            brahmic_block_rows.append(
                f"    {{0x{first:X}, 0x{last:X}, {len(brahmic_rows)}, BrahmicReading::{readings[reading]}}},")
            for cp in range(first, last + 1):
                role, inherent, sound, alt = self.brahmic_info(cp)
                inherent_literal = f"u'{inherent}'" if inherent else "0"
                brahmic_rows.append(
                    f"    {{BrahmicRole::{role}, {inherent_literal}, {pool.ref(sound)}, {pool.ref(alt)}, "
                    f"{pool.ref(nukta_of.get(cp, sound))}}},")
                decomposed = unicodedata.normalize("NFD", chr(cp))
                if role == "Sign" and len(decomposed) == 2:
                    merges.append((ord(decomposed[0]), ord(decomposed[1]), sound))
        merge_rows = [f"    {{0x{a:X}, 0x{b:X}, {pool.ref(s)}}}," for a, b, s in sorted(merges)]
        cluster_rows = []
        for key, sound in sorted(self.brahmic["clusters"].items()):
            cluster_rows.append(f"    {{0x{ord(key[0]):X}, 0x{ord(key[2]):X}, {pool.ref(sound)}}},")
        cluster_rows.sort()

        # Abjads
        abjad_rows: list[str] = []
        abjad_block_rows: list[str] = []
        for script, first, last in self.ABJAD_READER_BLOCKS:
            abjad_block_rows.append(f"    {{0x{first:X}, 0x{last:X}, {len(abjad_rows)}}},")
            for cp in range(first, last + 1):
                role, spelled, soft, geresh = self.abjad_info(script, cp)
                abjad_rows.append(
                    f"    {{AbjadRole::{role}, {{{', '.join(pool.ref(v) for v in spelled[:3])}}}, "
                    f"{pool.ref(soft)}, {pool.ref(geresh)}}},")
        arabic = self.abjad["arabic"]
        abjad_constants = [
            f"const Str kArabicArticle[3] = {str_array(arabic['article'])};",
            f"const Str kArabicWaw[3] = {str_array(arabic['long']['waw'])};",
            f"const Str kArabicYeh[3] = {str_array(arabic['long']['yeh'])};",
            f"const Str kArabicMarbuta[3] = {str_array(arabic['long']['marbuta'])};",
            f"const Str kHebrewFinalHe[2] = {str_array(self.abjad['hebrew']['final_he'])};",
        ]

        # Tibetan
        tibetan_rows = []
        for cp in range(0x0F00, 0x1000):
            role, sound = self.tibetan_info(cp)
            tibetan_rows.append(f"    {{TibetanRole::{role}, {pool.ref(sound)}}},")

        # Code points that compose canonically with the one before them (the
        # second half of an NFC composition), so the runtime composes only
        # text that needs it.
        composable_bmp = [0] * (0x10000 // 32)
        composable_astral: list[int] = []
        for cp in range(0x110000):
            decomposition = unicodedata.decomposition(chr(cp))
            if not decomposition or decomposition.startswith("<"):
                continue
            parts = [int(x, 16) for x in decomposition.split()]
            if len(parts) != 2 or unicodedata.normalize("NFC", unicodedata.normalize("NFD", chr(cp))) != chr(cp):
                continue
            if parts[1] < 0x10000:
                composable_bmp[parts[1] // 32] |= 1 << (parts[1] % 32)
            elif parts[1] not in composable_astral:
                composable_astral.append(parts[1])
        composable_astral.sort()
        # Hangul composes algorithmically: an LV syllable takes a final jamo.
        for cp in range(0x11A8, 0x11C3):
            composable_bmp[cp // 32] |= 1 << (cp % 32)

        # Pinyin syllable strings go into the pool before it is frozen.
        pinyin_refs = [pool.ref(s) for s in pinyin_syllables]

        digest = hashlib.sha256()
        digest.update(repr((bmp, astral, sorted(forms.items()), pool.units, kana_rows, hangul_lines,
                            bmp_pinyin, astral_pinyin, pinyin_syllables, japanese_rows, chinese_rows,
                            greek_styles, alpha_rows, cyr_styles, vowel_rows, thai_rows, lao_rows,
                            thai_word_rows, brahmic_rows, brahmic_block_rows, merge_rows, cluster_rows, abjad_rows,
                            abjad_block_rows, abjad_constants, tibetan_rows, composable_bmp,
                            composable_astral)).encode())
        if len(pool.units) >= (1 << PAYLOAD_BITS):
            sys.exit(f"string pool has {len(pool.units)} units; entries address {1 << PAYLOAD_BITS}")

        w("// Generated by tools/foldgen.py from src/search/fold/tables. Do not edit.")
        w("// clang-format off")
        w("")
        w(f"const std::uint32_t kDataDigest = 0x{digest.hexdigest()[:8]}u;")
        w("")
        w("const std::uint32_t kBmp[0x10000] = {")
        for i in range(0, 0x10000, 8):
            w("    " + " ".join(f"0x{e:08X}u," for e in bmp[i:i + 8]))
        w("};")
        w("")
        w(f"const AstralEntry kAstral[{len(astral)}] = {{")
        for cp, entry in astral:
            w(f"    {{0x{cp:X}, 0x{entry:08X}u}},")
        w("};")
        w(f"const std::size_t kAstralCount = {len(astral)};")
        w("")
        han_astral = [(lo, hi) for lo, hi in HAN_RANGES if lo >= 0x10000 and lo != 0x2F800]
        w(f"const CodeRange kAstralHan[{len(han_astral)}] = {{")
        for lo, hi in han_astral:
            w(f"    {{0x{lo:X}, 0x{hi:X}}},")
        w("};")
        w(f"const std::size_t kAstralHanCount = {len(han_astral)};")
        w("")
        w(f"const Decomp kDecomp[{len(decomp_rows)}] = {{")
        out.extend(decomp_rows)
        w("};")
        w(f"const std::size_t kDecompCount = {len(decomp_rows)};")
        w("")
        w(f"const KanaInfo kKana[{len(kana_rows)}] = {{")
        out.extend(kana_rows)
        w("};")
        w("")
        out.extend(hangul_lines)
        w("")
        w("const std::uint16_t kHanPinyinBmp[0xA000 - 0x3400] = {")
        for i in range(0, len(bmp_pinyin), 16):
            w("    " + " ".join(f"{v}," for v in bmp_pinyin[i:i + 16]))
        w("};")
        w(f"const HanAstralPinyin kHanPinyinAstral[{len(astral_pinyin)}] = {{")
        for cp, pid in astral_pinyin:
            w(f"    {{0x{cp:X}, {pid}}},")
        w("};")
        w(f"const std::size_t kHanPinyinAstralCount = {len(astral_pinyin)};")
        w(f"const Str kPinyin[{len(pinyin_refs) + 1}] = {{{{0, 0}}, {', '.join(pinyin_refs)}}};")
        w(f"const Word kJapaneseWords[{len(japanese_rows)}] = {{")
        out.extend(japanese_rows)
        w("};")
        w(f"const std::size_t kJapaneseWordCount = {len(japanese_rows)};")
        w(f"const int kJapaneseLongest = {japanese_longest};")
        w(f"const Word kChineseWords[{len(chinese_rows)}] = {{")
        out.extend(chinese_rows)
        w("};")
        w(f"const std::size_t kChineseWordCount = {len(chinese_rows)};")
        w(f"const int kChineseLongest = {chinese_longest};")
        w("")
        w(f"const GreekStyle kGreekStyles[{len(greek_styles)}] = {{")
        out.extend(greek_styles)
        w("};")
        w("")
        w("const std::uint32_t kComposableBmp[0x10000 / 32] = {")
        for i in range(0, len(composable_bmp), 8):
            w("    " + " ".join(f"0x{v:08X}u," for v in composable_bmp[i:i + 8]))
        w("};")
        w(f"const char32_t kComposableAstral[{len(composable_astral)}] = {{"
          + ", ".join(f"0x{cp:X}" for cp in composable_astral) + "};")
        w(f"const std::size_t kComposableAstralCount = {len(composable_astral)};")
        w("")
        w("const TibetanChar kTibetanChars[0x100] = {")
        out.extend(tibetan_rows)
        w("};")
        w("")
        w(f"const AbjadBlock kAbjadBlocks[{len(abjad_block_rows)}] = {{")
        out.extend(abjad_block_rows)
        w("};")
        w(f"const std::size_t kAbjadBlockCount = {len(abjad_block_rows)};")
        w(f"const AbjadChar kAbjadChars[{len(abjad_rows)}] = {{")
        out.extend(abjad_rows)
        w("};")
        out.extend(abjad_constants)
        w("")
        w(f"const BrahmicBlock kBrahmicBlocks[{len(brahmic_block_rows)}] = {{")
        out.extend(brahmic_block_rows)
        w("};")
        w(f"const std::size_t kBrahmicBlockCount = {len(brahmic_block_rows)};")
        w(f"const BrahmicChar kBrahmicChars[{len(brahmic_rows)}] = {{")
        out.extend(brahmic_rows)
        w("};")
        w(f"const BrahmicPair kBrahmicMerges[{max(1, len(merge_rows))}] = {{")
        out.extend(merge_rows)
        w("};")
        w(f"const std::size_t kBrahmicMergeCount = {len(merge_rows)};")
        w(f"const BrahmicPair kBrahmicClusters[{max(1, len(cluster_rows))}] = {{")
        out.extend(cluster_rows)
        w("};")
        w(f"const std::size_t kBrahmicClusterCount = {len(cluster_rows)};")
        w("")
        w("const ThaiChar kThaiChars[128] = {")
        out.extend(thai_rows)
        w("};")
        w("const ThaiChar kLaoChars[128] = {")
        out.extend(lao_rows)
        w("};")
        w(f"const Word kThaiWords[{len(thai_word_rows)}] = {{")
        out.extend(thai_word_rows)
        w("};")
        w(f"const std::size_t kThaiWordCount = {len(thai_word_rows)};")
        w(f"const int kThaiLongest = {thai_longest};")
        w(f"const Str kThaiVowels[{len(vowel_rows)}][2] = {{")
        out.extend(vowel_rows)
        w("};")
        w("")
        w(f"const AlphaRule kAlphaRules[{len(alpha_rows)}] = {{")
        out.extend(alpha_rows)
        w("};")
        w(f"const CyrillicStyle kCyrillicStyles[{len(cyr_styles)}] = {{")
        out.extend(cyr_styles)
        w("};")
        w(f"const AlphaRange kArmenianEastern = {armenian_eastern};")
        w(f"const AlphaRange kArmenianWestern = {armenian_western};")
        w(f"const AlphaRange kGerman = {german};")
        w(f"const AlphaRange kUmlautAsI = {u_as_i};")
        w("")
        w(f"const char16_t kPool[{max(1, len(pool.units))}] = {{")
        for i in range(0, len(pool.units), 16):
            w("    " + " ".join(f"0x{u:04X}," for u in pool.units[i:i + 16]))
        w("};")
        w(f"const std::size_t kPoolSize = {len(pool.units)};")
        w("")
        return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("tables", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    text = Generator(args.tables).build()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
