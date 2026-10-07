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
SCALAR_KEYS = ["tempo_bpm", "loudness_lufs", "loudness_std_db", "spectral_centroid_mean_hz",
               "spectral_centroid_std_hz", "spectral_flatness_mean", "zero_crossing_rate",
               "onset_rate_hz", "energy"]
SCALARS = ", ".join(SCALAR_KEYS)


def missing_tools():
    missing = [tool for tool in ("ffmpeg", "ffprobe") if shutil.which(tool) is None]
    if not missing:
        encoders = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True)
        if "libmp3lame" not in encoders.stdout:
            missing.append("the ffmpeg libmp3lame encoder")
    return missing


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    result = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=90)
    if EVIDENCE:
        with (EVIDENCE / "commands.jsonl").open("a") as log:
            log.write(json.dumps({"command": list(map(str, command)), "state_root": str(root),
                                  "exit": result.returncode, "stdout": result.stdout,
                                  "stderr": result.stderr}) + "\n")
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
            "too-many-records": lambda lines: lines[0].update(records=100001),
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
            file.truncate(64 * 1024 * 1024 + 1)
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
        # Export needs only per-file analysis, which identity refresh writes.
        identity_only = json.loads(pack(self.b, "export", destination, "--replace").stdout)
        self.assertEqual((identity_only["exported"], identity_only["skipped"]), (3, 0))
        self.assertEqual(self.store.read_bytes(), self.baseline)

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
        pack(self.a, "export", self.work / "absent/out.ndjson", expected=4)
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
        self.assertIn("content hashes identify recordings", preview["contents"])
        if EVIDENCE:
            (EVIDENCE / "leak-scan.json").write_text(json.dumps({"pack_sha256": digest(self.export),
                "forbidden_tokens": forbidden, "matches": [], "record_keys": sorted(allowed)}, indent=2) + "\n")

    def test_store_schema_and_lock(self):
        with database(self.store) as db:
            db.execute("UPDATE meta SET value='6' WHERE key='schema_version'")
        self.assert_rejected(self.export)
        self.store.write_bytes(self.baseline)
        lock = Path(str(self.store) + ".lock")
        process_name = Path("/proc/self/exe").resolve().name
        lock.write_text(f"{os.getpid()}\n{process_name}\n{socket.gethostname()}\n")
        try:
            self.assert_rejected(self.export, expected=5)
        finally:
            lock.unlink()

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
