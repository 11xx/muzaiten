"""Local feature exchange through the real single-writer CLI, using synthetic audio."""

import argparse
from contextlib import contextmanager
import getpass
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import wave


PARSER = argparse.ArgumentParser()
PARSER.add_argument("binary", type=Path)
PARSER.add_argument("--scratch", type=Path, help="parent directory for scratch data (default: TMPDIR)")
PARSER.add_argument("--evidence", type=Path)
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
BINARY = ARGS.binary.resolve()
SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = ARGS.scratch.resolve() if ARGS.scratch else None
EVIDENCE = ARGS.evidence.resolve() if ARGS.evidence else None
if EVIDENCE:
    EVIDENCE.mkdir(parents=True, exist_ok=True)
# CTest reports this exit status as a skip.
SKIP = 77
MAXIMUM_BYTES = 64 * 1024 * 1024
SCALAR_KEYS = ["tempo_bpm", "loudness_lufs", "loudness_std_db", "spectral_centroid_mean_hz",
               "spectral_centroid_std_hz", "spectral_flatness_mean", "zero_crossing_rate",
               "onset_rate_hz", "energy"]
SCALARS = ", ".join(SCALAR_KEYS)
# Protocol stub: answers the capabilities handshake, then holds `neighbors` (and so the writer lock)
# until the gate file is removed.
PROVIDER_STUB = r"""#!/bin/sh
request=$(cat)
id=$(printf '%s' "$request" | sed -n 's/.*"request_id":"\([^"]*\)".*/\1/p')
op=$(printf '%s' "$request" | sed -n 's/.*"operation":"\([^"]*\)".*/\1/p')
case "$op" in
capabilities)
    printf '{"protocol_version":1,"request_id":"%s","event":"result","result":{"capability":"clap","protocol_versions":[1]}}\n' "$id" ;;
neighbors)
    : > "$PROBE_READY"
    while [ -e "$PROBE_GATE" ]; do sleep 0.05; done
    printf '{"protocol_version":1,"request_id":"%s","event":"result","result":{"state":"released"}}\n' "$id" ;;
*)
    printf '{"protocol_version":1,"request_id":"%s","event":"error","code":"unsupported","message":"stub"}\n' "$id"
    exit 2 ;;
esac
"""


def missing_tools():
    missing = [tool for tool in ("ffmpeg", "ffprobe") if shutil.which(tool) is None]
    if not missing:
        encoders = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True)
        if EVIDENCE:
            (EVIDENCE / "encoder-probe.json").write_text(json.dumps({
                "command": ["ffmpeg", "-hide_banner", "-encoders"], "exit": encoders.returncode,
                "stdout": encoders.stdout, "stderr": encoders.stderr}, indent=2) + "\n")
        if encoders.returncode != 0 or not any(
                len(fields := line.split()) > 1 and len(fields[0]) == 6
                and fields[0].startswith("A") and fields[1] == "libmp3lame"
                for line in encoders.stdout.splitlines()):
            missing.append("the ffmpeg libmp3lame encoder")
    return missing


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def retain(paths):
    if not EVIDENCE:
        return {}
    artifacts = EVIDENCE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    hashes = {}
    for path in paths:
        if path.is_file():
            value = digest(path)
            destination = artifacts / value
            if not destination.exists():
                shutil.copyfile(path, destination)
            hashes[str(path)] = value
    return hashes


@contextmanager
def database(path):
    db = sqlite3.connect(path)
    try:
        with db:
            yield db
    finally:
        db.close()


def run(command, root, expected=0):
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("MUZAITEN_")}
    env.update(MUZAITEN_STATE_ROOT=str(root), QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(runtime))
    inputs = [root / "data/library.sqlite", root / "data/features.sqlite"]
    inputs += [Path(str(arg)) for arg in command[1:]]
    before = retain(inputs)
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=90)
    if EVIDENCE:
        with (EVIDENCE / "commands.jsonl").open("a") as log:
            log.write(json.dumps({"command": list(map(str, command)), "state_root": str(root),
                                  "exit": result.returncode, "stdout": result.stdout,
                                  "stderr": result.stderr, "artifacts_before": before,
                                  "artifacts_after": retain(inputs)}) + "\n")
    if result.returncode != expected:
        raise AssertionError(f"{command}: exit {result.returncode}, expected {expected}\n{result.stderr}")
    return result


def synthetic_audio(path, seed):
    rng = random.Random(seed)
    rate = 22050
    samples = bytearray()
    notes = [rng.choice([110, 146, 196, 261, 349, 466, 622, 830, 1108, 1480]) for _ in range(100)]
    for i in range(rate * 24):
        t = i / rate
        note = notes[int(t * 4)]
        envelope = math.exp(-8 * (t * 4 % 1))
        value = (0.3 * math.sin(2 * math.pi * note * t)
                 + 0.18 * math.sin(2 * math.pi * note * 1.5 * t)
                 + 0.08 * rng.uniform(-1, 1)) * envelope
        samples += struct.pack("<h", int(value * 32767))
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        audio.writeframes(samples)


def create_library(root, files, genre):
    data = root / "data"
    data.mkdir(parents=True, exist_ok=True)
    with database(data / "library.sqlite") as db:
        db.execute("CREATE TABLE tracks(path TEXT PRIMARY KEY, file_mtime INTEGER, file_size INTEGER, "
                   "missing INTEGER, scan_error TEXT, title TEXT, artist TEXT, genre TEXT, "
                   "musicbrainz_recording_id TEXT)")
        for name, mbid in files:
            path = root / "audio" / name
            stat = path.stat()
            db.execute("INSERT INTO tracks VALUES(?, ?, ?, 0, '', ?, ?, ?, ?)",
                       (str(path), int(stat.st_mtime), stat.st_size, name, root.name, genre, mbid))


def refresh(root, stage="all"):
    return json.loads(run([BINARY, "refresh", "--stage", stage, "--jobs", "2", "--no-semantic", "--json"], root).stdout)


def pack(root, operation, path, *options, expected=0):
    return run([BINARY, "pack", operation, path, *options, "--json"], root, expected)


class FeaturesPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SCRATCH:
            SCRATCH.mkdir(parents=True, exist_ok=True)
        cls.tmp = None if EVIDENCE else tempfile.TemporaryDirectory(prefix="exchange-", dir=SCRATCH)
        cls.work = Path(tempfile.mkdtemp(prefix="synthetic-", dir=EVIDENCE)) if EVIDENCE else Path(cls.tmp.name)
        cls.a, cls.b, cls.c = [cls.work / name for name in ("A", "B", "C")]
        for root in (cls.a, cls.b, cls.c):
            (root / "audio").mkdir(parents=True)
        for name, seed in (("shared.wav", 100), ("original.wav", 200), ("source-only.wav", 300)):
            synthetic_audio(cls.a / "audio" / name, seed)
        shared_mbid = "00000000-0000-4000-8000-000000000001"
        reencoded_mbid = "00000000-0000-4000-8000-000000000002"
        run(["ffmpeg", "-v", "error", "-i", cls.a / "audio/original.wav", "-metadata",
             f"MUSICBRAINZ_TRACKID={reencoded_mbid}", "-metadata", "genre=Electronic",
             cls.a / "audio/original.flac"], cls.a)
        (cls.a / "audio/original.wav").unlink()
        run(["ffmpeg", "-v", "error", "-i", cls.a / "audio/shared.wav", "-metadata",
             f"MUSICBRAINZ_TRACKID={shared_mbid}", "-metadata", "genre=Jazz", cls.b / "audio/identical.flac"], cls.b)
        run(["ffmpeg", "-v", "error", "-i", cls.a / "audio/original.flac", "-c:a", "libmp3lame",
             "-b:a", "64k", "-metadata", f"MUSICBRAINZ_TRACKID={reencoded_mbid}",
             "-metadata", "genre=Folk", cls.b / "audio/reencoded.mp3"], cls.b)
        synthetic_audio(cls.b / "audio/disjoint.wav", 400)
        tags = []
        for path, root in ((cls.a / "audio/original.flac", cls.a), (cls.b / "audio/reencoded.mp3", cls.b)):
            probe = run(["ffprobe", "-v", "error", "-show_entries", "format_tags", "-of", "json", path], root)
            tags.append({key.lower(): value for key, value in json.loads(probe.stdout)["format"]["tags"].items()})
        if tags[0]["musicbrainz_trackid"] != reencoded_mbid or tags[1]["musicbrainz_trackid"] != reencoded_mbid:
            raise AssertionError("Re-encoding must retain the source recording ID")
        for path in (cls.b / "audio").iterdir():
            shutil.copyfile(path, cls.c / "audio" / path.name)
        create_library(cls.a, [("shared.wav", shared_mbid), ("original.flac", reencoded_mbid),
                               ("source-only.wav", "")], "Electronic;Rock")
        for root, genre in ((cls.b, "Jazz;Folk"), (cls.c, "Classical;Ambient")):
            create_library(root, [("identical.flac", shared_mbid), ("reencoded.mp3", reencoded_mbid),
                                  ("disjoint.wav", "")], genre)
        refresh(cls.a)
        refresh(cls.b, "identity")
        refresh(cls.c)
        cls.store = cls.b / "data/features.sqlite"
        cls.baseline = cls.store.read_bytes()
        with database(cls.store) as db:
            cls.baseline_file_features = db.execute("SELECT * FROM file_features ORDER BY path").fetchall()
        cls.export = cls.work / "scalars.ndjson"
        cls.export_report = json.loads(pack(cls.a, "export", cls.export).stdout)
        cls.document = cls.export.read_bytes()
        cls.lines = [json.loads(line) for line in cls.document.splitlines()]
        if EVIDENCE:
            shutil.copyfile(cls.export, EVIDENCE / "scalars.ndjson")
            (EVIDENCE / "synthetic-manifest.json").write_text(json.dumps({
                "roots": [str(cls.a), str(cls.b), str(cls.c)],
                "audio_sha256": {str(p): digest(p) for p in cls.work.glob("*/audio/*")},
                "B_before": digest(cls.store), "export_report": cls.export_report,
                "source_and_reencoded_tags": tags}, indent=2) + "\n")
            (EVIDENCE / "B-before-import.sqlite").write_bytes(cls.baseline)

    @classmethod
    def tearDownClass(cls):
        if cls.tmp:
            cls.tmp.cleanup()

    def setUp(self):
        self.store.write_bytes(self.baseline)

    def write_document(self, lines, name="input.ndjson"):
        path = self.work / name
        path.write_bytes(b"".join(json.dumps(line, separators=(",", ":")).encode() + b"\n" for line in lines))
        return path

    def assert_rejected(self, path, expected=2):
        before = digest(self.store)
        result = pack(self.b, "import", path, "--replace", expected=expected)
        after = digest(self.store)
        if EVIDENCE:
            shutil.copyfile(self.store, EVIDENCE / "B-after-import.sqlite")
        self.assertEqual(after, before, result.stderr)
        if EVIDENCE:
            shutil.copyfile(self.store, EVIDENCE / "B-after-analysis.sqlite")
            with (EVIDENCE / "rejected-store-hashes.jsonl").open("a") as log:
                log.write(json.dumps({"input": path.name, "before": before, "after": after,
                                      "stderr": result.stderr, "exit": result.returncode}) + "\n")

    def test_roundtrip_preview_exact_identity_and_freshness(self):
        self.assertEqual(self.export_report["records"], 3)
        preview = json.loads(pack(self.b, "import", self.export, "--dry-run").stdout)
        self.assertEqual(self.store.read_bytes(), self.baseline)
        self.assertEqual((preview["adopted"], preview["skipped"], preview["mismatched"]), (1, 0, 2))
        self.assertEqual((preview["destination_files"], preview["destination_unmatched_files"]), (3, 2))
        adopted = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual(adopted["adopted"], 1)
        after = digest(self.store)
        with database(self.store) as db, database(self.c / "data/features.sqlite") as oracle:
            rows = db.execute("SELECT f.decode_hash, feat.* FROM features feat JOIN files f "
                              "ON f.content_group_id=feat.content_group_id").fetchall()
            self.assertEqual(len(rows), 1)
            fresh = oracle.execute("SELECT feat.* FROM features feat JOIN files f ON "
                                   "f.content_group_id=feat.content_group_id WHERE f.decode_hash=?", (rows[0][0],)).fetchone()
            self.assertEqual(rows[0][2:], fresh[1:])
            self.assertEqual(db.execute("SELECT * FROM file_features ORDER BY path").fetchall(), self.baseline_file_features)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM features feat JOIN files f ON "
                                        "f.content_group_id=feat.content_group_id WHERE path LIKE '%reencoded.mp3'").fetchone()[0], 0)
        again = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual((again["adopted"], again["skipped"], again["existing"]), (0, 1, 1))
        self.assertEqual(digest(self.store), after)
        next_analysis = refresh(self.b)
        self.assertEqual(next_analysis["scanned"], 0)
        self.assertEqual(next_analysis["skipped"], 3)
        self.assertEqual(next_analysis["feature_groups_processed"], 2)
        self.assertEqual(next_analysis["features_written"], 2)
        if EVIDENCE:
            (EVIDENCE / "roundtrip.json").write_text(json.dumps({"preview": preview, "import": adopted,
                "before": hashlib.sha256(self.baseline).hexdigest(), "after_import": after,
                "adopted_scalars": rows[0][2:], "fresh_C_scalars": fresh[1:],
                "next_analysis": next_analysis, "after_analysis": digest(self.store)}, indent=2) + "\n")

    def test_malformed_files_preserve_store_bytes(self):
        cases = {
            "malformed": b"{\n", "truncated": self.document[:-2],
            "record-boundary-truncated": b"\n".join(self.document.splitlines()[:-1]) + b"\n",
            "invalid-utf8": self.document.replace(b"muzaiten-features-pack", b"\xff"),
            "duplicate-key": self.document.replace(b'"format":', b'"format":"x","format":', 1),
            "escaped-duplicate-key": self.document.replace(b'"format":', b'"\\u0066ormat":"x","format":', 1),
            "blank-line": self.document + b"\n", "trailing-garbage": self.document + b"garbage\n",
            "oversize-line": b'{"format":"' + b"x" * 16384 + b'"}\n',
        }
        for name, content in cases.items():
            with self.subTest(name=name):
                path = self.work / (name + ".ndjson")
                path.write_bytes(content)
                self.assert_rejected(path)
        for name, mutate in {
            "unknown-version": lambda lines: lines[0].update(version=2),
            "string-version": lambda lines: lines[0].update(version="1"),
            "unknown-header-key": lambda lines: lines[0].update(timestamp=1),
            "too-many-records": lambda lines: lines[0].update(records=MAXIMUM_BYTES),
            "wrong-count": lambda lines: lines[0].update(records=2),
            "unknown-record-key": lambda lines: lines[1].update(path="audio.wav"),
            "embedding-section": lambda lines: lines[1].update(embedding={}),
            "hint-as-key": lambda lines: lines[1].update(musicbrainz_recording_id="id"),
            "unknown-scalar-key": lambda lines: lines[1]["scalars"].update(rating=5),
            "missing-key": lambda lines: lines[1].pop("extractor"),
            "uppercase-hash": lambda lines: lines[1].update(decode_hash="A" * 64),
            "string-scalar": lambda lines: lines[1]["scalars"].update(energy="0.5"),
            "nonfinite": lambda lines: lines[1]["scalars"].update(energy=float("inf")),
            "out-of-range": lambda lines: lines[1]["scalars"].update(energy=2),
            "duplicate-hash": lambda lines: lines[2].update(decode_hash=lines[1]["decode_hash"]),
        }.items():
            with self.subTest(name=name):
                lines = json.loads(json.dumps(self.lines))
                mutate(lines)
                self.assert_rejected(self.write_document(lines, name + ".ndjson"))
        oversized = self.work / "oversize.ndjson"
        with oversized.open("wb") as file:
            file.truncate(MAXIMUM_BYTES + 1)
        self.assert_rejected(oversized)
        oversized.unlink()

    def test_versions_and_replacement(self):
        lines = json.loads(json.dumps(self.lines))
        for record in lines[1:]:
            record["version"] = "muzaiten-dsp-v1"
        report = json.loads(pack(self.b, "import", self.write_document(lines), "--replace").stdout)
        self.assertEqual((report["adopted"], report["incompatible"], report["mismatched"]), (0, 3, 3))
        self.assertEqual(self.store.read_bytes(), self.baseline)
        for record in lines[1:]:
            record.update(version="muzaiten-dsp-v2", extractor="foreign-dsp")
        report = json.loads(pack(self.b, "import", self.write_document(lines), "--replace").stdout)
        self.assertEqual(report["incompatible"], 3)
        self.assertEqual(self.store.read_bytes(), self.baseline)
        pack(self.b, "import", self.export)
        with database(self.store) as db:
            adopted = db.execute("SELECT energy FROM features").fetchone()[0]
            db.execute("UPDATE features SET version='muzaiten-dsp-v1', energy=0.123")
        stale = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual((stale["adopted"], stale["existing"]), (1, 0))
        with database(self.store) as db:
            self.assertEqual(db.execute("SELECT version, energy FROM features").fetchone(), ("muzaiten-dsp-v2", adopted))
            db.execute("UPDATE features SET energy=0.123")
        before = digest(self.store)
        skipped = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual((skipped["adopted"], skipped["existing"]), (0, 1))
        self.assertEqual(digest(self.store), before)
        replaced = json.loads(pack(self.b, "import", self.export, "--replace").stdout)
        self.assertEqual(replaced["adopted"], 1)
        with database(self.store) as db:
            self.assertEqual(db.execute("SELECT version, energy FROM features").fetchone(), ("muzaiten-dsp-v2", adopted))

    def test_import_fills_stale_rows_without_decoding(self):
        # After a DSP upgrade, identity refresh skips unchanged files, so both
        # their group rows and their per-file rows stay stale.
        refresh(self.b, "features")
        with database(self.store) as db:
            db.execute("UPDATE features SET version='muzaiten-dsp-v1'")
            db.execute("UPDATE file_features SET version='muzaiten-dsp-v1'")
        report = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual((report["adopted"], report["existing"]), (1, 0))
        following = refresh(self.b)
        self.assertEqual(following["scanned"], 0)
        self.assertEqual(following["feature_groups_processed"], 2)

    def test_export_uses_the_representatives_own_analysis(self):
        # A fingerprint-matched sibling at a lower path becomes the representative
        # of a group whose current row still holds the original file's analysis.
        root = self.work / "D"
        (root / "audio").mkdir(parents=True)
        shutil.copyfile(self.a / "audio/original.flac", root / "audio/original.flac")
        create_library(root, [("original.flac", "")], "Electronic")
        refresh(root)
        sibling = root / "audio/0-reencoded.mp3"
        shutil.copyfile(self.b / "audio/reencoded.mp3", sibling)
        stat = sibling.stat()
        with database(root / "data/library.sqlite") as db:
            db.execute("INSERT INTO tracks VALUES(?, ?, ?, 0, '', ?, ?, ?, '')",
                       (str(sibling), int(stat.st_mtime), stat.st_size, sibling.name, root.name, "Electronic"))
        refresh(root)
        with database(root / "data/features.sqlite") as db:
            self.assertEqual(db.execute("SELECT COUNT(DISTINCT content_group_id) FROM files").fetchone()[0], 1)
            group_row = db.execute(f"SELECT {SCALARS} FROM features").fetchone()
            sibling_hash, original_hash = (
                db.execute("SELECT decode_hash FROM files WHERE path=?", (str(path),)).fetchone()[0]
                for path in (sibling, root / "audio/original.flac"))
        with database(self.c / "data/features.sqlite") as oracle:
            fresh = oracle.execute(f"SELECT {SCALARS} FROM features feat JOIN files f ON "
                                   "f.content_group_id=feat.content_group_id WHERE f.decode_hash=?",
                                   (sibling_hash,)).fetchone()
        self.assertNotEqual(group_row, fresh, "the group row must still describe the original file")
        exported = self.work / "representative.ndjson"
        report = json.loads(pack(root, "export", exported).stdout)
        self.assertEqual((report["exported"], report["skipped"]), (1, 0))
        records = [json.loads(line) for line in exported.read_bytes().splitlines()[1:]]
        self.assertEqual([record["decode_hash"] for record in records], [sibling_hash])
        self.assertNotIn(original_hash.encode(), exported.read_bytes())
        self.assertEqual(tuple(records[0]["scalars"][key] for key in SCALAR_KEYS), fresh)
        adopted = json.loads(pack(self.b, "import", exported).stdout)
        self.assertEqual((adopted["adopted"], adopted["mismatched"]), (1, 0))
        with database(self.store) as db:
            imported = db.execute(f"SELECT {SCALARS} FROM features feat JOIN files f ON "
                                  "f.content_group_id=feat.content_group_id WHERE f.decode_hash=?",
                                  (sibling_hash,)).fetchone()
            self.assertEqual(imported, fresh)
            self.assertEqual(db.execute("SELECT * FROM file_features ORDER BY path").fetchall(), self.baseline_file_features)
        if EVIDENCE:
            (EVIDENCE / "lower-sorting-sibling.json").write_text(json.dumps({
                "original_hash": original_hash, "sibling_hash": sibling_hash,
                "group_scalars": group_row, "fresh_C_scalars": fresh,
                "export": report, "import": adopted, "imported_scalars": imported}, indent=2) + "\n")

    def test_export_after_in_place_reencoding(self):
        a, b, c = [self.work / "in-place" / name for name in ("A", "B", "C")]
        for root in (a, b, c):
            (root / "audio").mkdir(parents=True)
        representative = a / "audio/recording.flac"
        shutil.copyfile(self.a / "audio/original.flac", representative)
        shutil.copyfile(self.b / "audio/reencoded.mp3", a / "audio/zz-sibling.mp3")
        create_library(a, [(representative.name, ""), ("zz-sibling.mp3", "")], "Electronic")
        refresh(a)
        source_store = a / "data/features.sqlite"
        with database(source_store) as db:
            self.assertEqual(db.execute("SELECT COUNT(DISTINCT content_group_id) FROM files").fetchone()[0], 1)
            old_hash, group_id = db.execute("SELECT decode_hash, content_group_id FROM files WHERE path=?",
                                           (str(representative),)).fetchone()
            old_scalars = db.execute(f"SELECT {SCALARS} FROM features").fetchone()
        replacement = a / "audio/replacement.flac"
        run(["ffmpeg", "-v", "error", "-i", representative, "-af", "volume=0.5", replacement], a)
        replacement.replace(representative)
        stat = representative.stat()
        with database(a / "data/library.sqlite") as db:
            db.execute("UPDATE tracks SET file_mtime=?, file_size=? WHERE path=?",
                       (int(stat.st_mtime), stat.st_size, str(representative)))
        refresh(a)
        with database(source_store) as db:
            self.assertEqual(db.execute("SELECT COUNT(DISTINCT content_group_id) FROM files").fetchone()[0], 1)
            new_hash, new_group_id = db.execute("SELECT decode_hash, content_group_id FROM files WHERE path=?",
                                               (str(representative),)).fetchone()
            group_scalars = db.execute(f"SELECT {SCALARS} FROM features").fetchone()
            own_scalars = db.execute(f"SELECT {SCALARS} FROM file_features WHERE path=?",
                                     (str(representative),)).fetchone()
        self.assertEqual(new_group_id, group_id)
        self.assertNotEqual(new_hash, old_hash)
        self.assertEqual(group_scalars, old_scalars)
        self.assertNotEqual(own_scalars, old_scalars)
        for root in (b, c):
            shutil.copyfile(representative, root / "audio/recipient.flac")
            create_library(root, [("recipient.flac", "")], "Folk")
        refresh(b, "identity")
        refresh(c)
        exported = self.work / "in-place.ndjson"
        report = json.loads(pack(a, "export", exported).stdout)
        self.assertEqual((report["exported"], report["skipped"]), (1, 0))
        record = json.loads(exported.read_bytes().splitlines()[1])
        self.assertEqual(record["decode_hash"], new_hash)
        self.assertEqual(tuple(record["scalars"][key] for key in SCALAR_KEYS), own_scalars)
        adopted = json.loads(pack(b, "import", exported).stdout)
        self.assertEqual((adopted["adopted"], adopted["mismatched"]), (1, 0))
        with database(b / "data/features.sqlite") as db, database(c / "data/features.sqlite") as oracle:
            self.assertEqual(db.execute("SELECT decode_hash FROM files").fetchone()[0], new_hash)
            self.assertEqual(oracle.execute("SELECT decode_hash FROM files").fetchone()[0], new_hash)
            imported = db.execute(f"SELECT {SCALARS} FROM features").fetchone()
            fresh = oracle.execute(f"SELECT {SCALARS} FROM features").fetchone()
            self.assertEqual(imported, fresh)
            self.assertEqual(imported, own_scalars)
        rejected = self.work / "in-place-rejected.ndjson"
        record["scalars"]["energy"] = 2
        rejected.write_text(json.dumps({"format": "muzaiten-features-pack", "version": 1, "records": 1})
                            + "\n" + json.dumps(record) + "\n")
        before = digest(b / "data/features.sqlite")
        pack(b, "import", rejected, "--replace", expected=2)
        after = digest(b / "data/features.sqlite")
        self.assertEqual(after, before)
        if EVIDENCE:
            (EVIDENCE / "in-place-reencoding.json").write_text(json.dumps({
                "old_hash": old_hash, "new_hash": new_hash, "group_id": group_id,
                "old_scalars": old_scalars, "retained_group_scalars": group_scalars,
                "per_file_scalars": own_scalars, "fresh_C_scalars": fresh,
                "export": report, "import": adopted, "imported_scalars": imported,
                "rejected_before": before, "rejected_after": after}, indent=2) + "\n")

    def test_export_skips_and_counts_unusable_local_rows(self):
        store = self.work / "unusable.sqlite"
        shutil.copyfile(self.a / "data/features.sqlite", store)
        destination = self.work / "unusable.ndjson"

        def export():
            report = json.loads(pack(self.a, "export", destination, "--replace", "--features", store).stdout)
            return report["exported"], report["skipped"], report["invalid"]

        with database(store) as db:
            original_hash = db.execute("SELECT decode_hash FROM files WHERE path LIKE '%original.flac'").fetchone()[0]
            db.execute("UPDATE file_features SET energy=9e999 WHERE path LIKE '%shared.wav'")
            db.execute("UPDATE file_features SET spectral_flatness_mean=1.5 WHERE path LIKE '%source-only.wav'")
        self.assertEqual(export(), (1, 2, 2))
        self.assertEqual([json.loads(line) for line in destination.read_bytes().splitlines()[1:]],
                         [line for line in self.lines[1:] if line["decode_hash"] == original_hash])
        with database(store) as db:
            db.execute("UPDATE file_features SET version='muzaiten-dsp-v1' WHERE path LIKE '%original.flac'")
        self.assertEqual(export(), (0, 3, 2))
        with database(store) as db:
            db.execute("DELETE FROM file_features")
            self.assertEqual(db.execute("SELECT COUNT(*) FROM features").fetchone()[0], 3)
        self.assertEqual(export(), (0, 3, 0))
        shutil.copyfile(self.a / "data/features.sqlite", store)
        with database(store) as db:
            db.execute("UPDATE files SET decode_hash='invalid' WHERE path LIKE '%original.flac'")
            db.execute("UPDATE file_features SET energy='invalid' WHERE path LIKE '%shared.wav'")
        self.assertEqual(export(), (1, 2, 2))
        plain = run([BINARY, "pack", "export", destination, "--replace", "--features", store], self.a)
        self.assertIn("skipped=2", plain.stdout)
        self.assertIn("invalid=2", plain.stdout)
        # Export needs only per-file analysis, which identity refresh writes.
        identity_only = json.loads(pack(self.b, "export", destination, "--replace").stdout)
        self.assertEqual((identity_only["exported"], identity_only["skipped"]), (3, 0))
        self.assertEqual(self.store.read_bytes(), self.baseline)

    def test_byte_cap_is_the_binding_limit(self):
        shortest = {"decode_hash": "{hash}", "extractor": "a", "version": "a",
                    "scalars": dict.fromkeys(SCALAR_KEYS, 0)}
        prefix, suffix = json.dumps(shortest, separators=(",", ":")).encode().split(b"{hash}")
        line_bytes = len(prefix) + 64 + len(suffix) + 1

        def header(records):
            return json.dumps({"format": "muzaiten-features-pack", "version": 1, "records": records},
                              separators=(",", ":")).encode() + b"\n"

        count = (MAXIMUM_BYTES - len(header(MAXIMUM_BYTES))) // line_bytes
        path = self.work / "at-byte-cap.ndjson"
        try:
            with path.open("wb") as file:
                padding = MAXIMUM_BYTES - len(header(count)) - count * line_bytes
                file.write(header(count)[:-1] + b" " * padding + b"\n")
                for index in range(count):
                    file.write(prefix + b"%064x" % index + suffix + b"\n")
            self.assertEqual(path.stat().st_size, MAXIMUM_BYTES)
            self.assertGreater(count, 100000)
            report = json.loads(pack(self.b, "import", path, "--dry-run").stdout)
            self.assertEqual((report["records"], report["incompatible"]), (count, count))
            self.assertEqual(self.store.read_bytes(), self.baseline)
            with path.open("ab") as file:
                file.write(b"\n")
            self.assert_rejected(path)
        finally:
            path.unlink(missing_ok=True)
        # A library whose pack would exceed the byte cap exports nothing.
        store = self.work / "past-byte-cap.sqlite"
        shutil.copyfile(self.a / "data/features.sqlite", store)
        with database(store) as db:
            db.execute("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100001) "
                       "INSERT INTO files(path, mtime, size, decode_hash, content_group_id, analyzed_at, status) "
                       "SELECT '/synthetic/' || i, 0, 0, printf('%064x', i), 1000000 + i, 0, 'ok' FROM n")
            db.execute(f"INSERT INTO file_features(path, {SCALARS}, extractor, version) SELECT path, "
                       + ", ".join(["0"] * len(SCALAR_KEYS))
                       + ", 'muzaiten-dsp', 'muzaiten-dsp-v2' FROM files WHERE path LIKE '/synthetic/%'")
        destination = self.work / "past-byte-cap.ndjson"
        exported = json.loads(pack(self.a, "export", destination, "--features", store).stdout)
        self.assertEqual(exported["exported"], 100004)
        self.assertLess(destination.stat().st_size, MAXIMUM_BYTES)
        before = digest(destination)
        with database(store) as db:
            db.execute("WITH RECURSIVE n(i) AS (SELECT 100002 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) "
                       "INSERT INTO files(path, mtime, size, decode_hash, content_group_id, analyzed_at, status) "
                       "SELECT '/synthetic/' || i, 0, 0, printf('%064x', i), 1000000 + i, 0, 'ok' FROM n")
            db.execute(f"INSERT OR REPLACE INTO file_features(path, {SCALARS}, extractor, version) SELECT path, "
                       + ", ".join(["0.123456789012345"] * len(SCALAR_KEYS))
                       + ", 'muzaiten-dsp', 'muzaiten-dsp-v2' FROM files WHERE path LIKE '/synthetic/%'")
        failed = pack(self.a, "export", destination, "--replace", "--features", store, expected=2)
        self.assertIn("size limits", failed.stderr)
        self.assertEqual(digest(destination), before)
        absent = self.work / "oversize-export.ndjson"
        pack(self.a, "export", absent, "--dry-run", "--features", store, expected=2)
        self.assertFalse(absent.exists())
        store.unlink()

    def test_null_scalar_result_remains_fresh(self):
        lines = json.loads(json.dumps(self.lines))
        for record in lines[1:]:
            record["scalars"] = dict.fromkeys(record["scalars"])
        report = json.loads(pack(self.b, "import", self.write_document(lines)).stdout)
        self.assertEqual(report["adopted"], 1)
        with database(self.store) as db:
            scalar_keys = list(lines[1]["scalars"])
            row = db.execute("SELECT " + ", ".join(scalar_keys) + " FROM features").fetchone()
            self.assertEqual(row, (None,) * 9)
        self.assertEqual(refresh(self.b, "features")["feature_groups_processed"], 2)

    def test_export_preview_atomic_replacement_and_disclosure(self):
        destination = self.work / "destination.ndjson"
        if destination.exists():
            destination.unlink()
        preview = json.loads(pack(self.a, "export", destination, "--dry-run").stdout)
        self.assertFalse(destination.exists())
        self.assertEqual(preview["exported"], 3)
        destination.write_bytes(b"retain me")
        pack(self.a, "export", destination, expected=2)
        self.assertEqual(destination.read_bytes(), b"retain me")
        pack(self.a, "export", destination, "--replace", "--dry-run")
        self.assertEqual(destination.read_bytes(), b"retain me")
        pack(self.a, "export", destination, "--replace")
        self.assertEqual(destination.read_bytes(), self.document)
        failed = run([BINARY, "pack", "export", self.work / "absent/out.ndjson"], self.a, expected=4)
        self.assertIn("anyone with the same audio can tell you have it", failed.stdout)
        plain = run([BINARY, "pack", "export", destination, "--replace"], self.a)
        self.assertEqual(plain.stdout.splitlines()[0], preview["contents"])
        self.assertEqual(plain.stdout.count(preview["contents"]), 1)
        store = self.a / "data/features.sqlite"
        before = digest(store)
        for suffix in ("", ".lock", "-wal", "-shm", "-journal"):
            pack(self.a, "export", str(store) + suffix, "--replace", expected=2)
        self.assertEqual(digest(store), before)
        link = self.work / "linked.ndjson"
        link.symlink_to(destination)
        pack(self.a, "export", link, "--replace", expected=2)
        self.assertEqual(destination.read_bytes(), self.document)
        text = self.document.decode()
        forbidden = [str(self.work), str(SOURCE), getpass.getuser(), "shared.wav", "original.flac",
                     "source-only.wav", "identical.flac", "reencoded.mp3", "disjoint.wav"]
        for token in forbidden:
            self.assertNotIn(token, text)
        allowed = {"decode_hash", "extractor", "version", "scalars"}
        for record in self.lines[1:]:
            self.assertEqual(set(record), allowed)
        self.assertIn("every exported recording in this library", preview["contents"])
        self.assertIn("anyone with the same audio can tell you have it", preview["contents"])
        self.assertNotIn("—", plain.stdout + failed.stdout)
        if EVIDENCE:
            (EVIDENCE / "leak-scan.json").write_text(json.dumps({"pack_sha256": digest(self.export),
                "forbidden_tokens": forbidden, "matches": [], "record_keys": sorted(allowed)}, indent=2) + "\n")

    def test_store_schema_and_lock(self):
        with database(self.store) as db:
            db.execute("UPDATE meta SET value='6' WHERE key='schema_version'")
        self.assert_rejected(self.export)
        self.store.write_bytes(self.baseline)
        lock = Path(os.path.realpath(self.store) + ".lock")
        process_name = Path("/proc/self/exe").resolve().name
        lock.write_text(f"{os.getpid()}\n{process_name}\n{socket.gethostname()}\n")
        try:
            self.assert_rejected(self.export, expected=5)
        finally:
            lock.unlink()

    def test_live_writer_excludes_file_alias_spellings(self):
        alias = self.work / "alias" / "features.sqlite"
        alias.parent.mkdir(exist_ok=True)
        alias.symlink_to(self.store)
        stub = self.work / "provider-stub.sh"
        stub.write_text(PROVIDER_STUB)
        stub.chmod(0o755)
        ready, gate = self.work / "owner.ready", self.work / "owner.gate"
        ready.unlink(missing_ok=True)
        gate.write_text("hold\n")
        runtime = self.b / "runtime"
        runtime.mkdir(mode=0o700, exist_ok=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith("MUZAITEN_")}
        env.update(MUZAITEN_STATE_ROOT=str(self.b), QT_QPA_PLATFORM="offscreen",
                   XDG_RUNTIME_DIR=str(runtime), PROBE_READY=str(ready), PROBE_GATE=str(gate))
        owner = subprocess.Popen([str(BINARY), "neighbors", "--force", "--features", str(self.store),
                                  "--state", str(self.b / "state.sqlite"), "--provider", str(stub), "--json"],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 30
            while not ready.exists():
                if owner.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError("the live owner did not take the writer lock")
                time.sleep(0.02)
            before = digest(self.store)
            pack(self.b, "export", self.work / "canonical-contended.ndjson", expected=5)
            pack(self.b, "export", self.work / "alias-contended.ndjson", "--features", alias, expected=5)
            pack(self.b, "import", self.export, "--features", alias, expected=5)
            self.assertEqual(digest(self.store), before)
            self.assertFalse((self.work / "alias-contended.ndjson").exists())
        finally:
            gate.unlink(missing_ok=True)
            _, errors = owner.communicate(timeout=30)
        self.assertEqual(owner.returncode, 0, errors)
        pack(self.b, "export", self.work / "alias-released.ndjson", "--features", alias)

    def test_representative_hash_is_the_only_adoption_key(self):
        with database(self.store) as db:
            shared_hash = next(record["decode_hash"] for record in self.lines[1:]
                               if db.execute("SELECT 1 FROM files WHERE decode_hash=?", (record["decode_hash"],)).fetchone())
            group = db.execute("SELECT content_group_id FROM files WHERE decode_hash=?", (shared_hash,)).fetchone()[0]
            db.execute("UPDATE files SET content_group_id=? WHERE path LIKE '%disjoint.wav'", (group,))
        before = digest(self.store)
        report = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual(report["adopted"], 0)
        self.assertEqual(report["destination_unmatched_files"], 2)
        self.assertEqual(digest(self.store), before)

    def test_ambiguous_destination_hashes_are_mismatched(self):
        with database(self.store) as db:
            shared_hash = db.execute("SELECT decode_hash FROM files WHERE path LIKE '%identical.flac'").fetchone()[0]
            db.execute("UPDATE files SET decode_hash=? WHERE path LIKE '%disjoint.wav'", (shared_hash,))
        before = digest(self.store)
        report = json.loads(pack(self.b, "import", self.export).stdout)
        self.assertEqual((report["adopted"], report["mismatched"]), (0, 3))
        self.assertEqual(digest(self.store), before)

    def test_transaction_rolls_back_every_row(self):
        # Copy identity rows from A so multiple matching inserts hit one transaction.
        source = self.a / "data/features.sqlite"
        self.store.write_bytes(source.read_bytes())
        with database(self.store) as db:
            db.execute("DELETE FROM features")
            db.execute("CREATE TRIGGER reject_second BEFORE INSERT ON features WHEN "
                       "(SELECT COUNT(*) FROM features) >= 1 BEGIN SELECT RAISE(ABORT, 'fixture rejection'); END")
        self.assert_rejected(self.export, expected=4)
        with database(self.store) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM features").fetchone()[0], 0)


if __name__ == "__main__":
    if missing := missing_tools():
        print("Skipping: requires " + ", ".join(missing) + ".", file=sys.stderr)
        sys.exit(SKIP)
    unittest.main(argv=[__file__, *UNITTEST_ARGS])
