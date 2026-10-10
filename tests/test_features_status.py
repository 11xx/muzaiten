"""Diagnostics read a current-schema feature store without writing or waiting on a writer."""
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()
# Holds a committed row plus an open write transaction, as a refresh does
# between batch commits, and reports over the pipe.
WRITER = r"""
import sqlite3, sys
db = sqlite3.connect(sys.argv[1], isolation_level=None)
db.execute("BEGIN IMMEDIATE")
db.execute("INSERT INTO files(path, mtime, size, analyzed_at) VALUES ('/synthetic/committed.flac', 1, 2, 3)")
db.execute("COMMIT")
db.execute("BEGIN IMMEDIATE")
db.execute("INSERT INTO files(path, mtime, size, analyzed_at) VALUES ('/synthetic/uncommitted.flac', 1, 2, 3)")
print("holding", flush=True)
sys.stdin.readline()
db.execute("ROLLBACK")
"""
class FeaturesStatusTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="muzaiten-features-status-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("MUZAITEN_", "XDG_")) and k != "HOME"}
        self.env.update(HOME=str(self.root / "home"), MUZAITEN_STATE_ROOT=str(self.root / "state-root"),
                        PATH="/usr/bin:/bin")
        self.store = self.root / "data" / "features.sqlite"
        library = self.root / "library.sqlite"
        with closing(sqlite3.connect(library)) as db, db:
            db.execute("CREATE TABLE tracks(path TEXT, file_mtime INTEGER, file_size INTEGER,"
                       " missing INTEGER, scan_error TEXT)")
        self.store.parent.mkdir()
        # The native writer creates the current schema from an empty library.
        self.features("refresh", "--library", str(library), "--no-semantic", check=True)
        with closing(sqlite3.connect(self.store)) as db, db:
            db.execute("INSERT INTO content_groups(id) VALUES (1)")
            db.execute("INSERT INTO files(path, mtime, size, analyzed_at, content_group_id)"
                       " VALUES ('/synthetic/a.flac', 1, 2, 3, 1)")
            db.execute("INSERT INTO features(content_group_id, tempo_bpm, extractor, version)"
                       " VALUES (1, 120, 'cpp', 'older-dsp')")
            db.execute("UPDATE meta SET value = 'older-dsp' WHERE key = 'dsp_version'")
        self.addCleanup(self.store.parent.chmod, 0o755)

    def features(self, command, *args, check=False):
        result = subprocess.run([str(BINARY), command, "--features", str(self.store), "--state",
                                 str(self.root / "absent-state.sqlite"), *args],
                                env=self.env, capture_output=True, text=True, timeout=60)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def diagnose(self, command):
        result = self.features(command, "--provider", str(self.root / "absent-provider"), "--json")
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        report = json.loads(result.stdout)
        report.pop("semantic")
        return report

    def meta(self):
        db = sqlite3.connect(f"file:{self.store}?mode=ro", uri=True)
        try:
            return dict(db.execute("SELECT key, value FROM meta"))
        finally:
            db.close()

    def test_current_store_is_read_without_writes(self):
        for command in ("status", "doctor"):
            with self.subTest(command=command):
                with closing(sqlite3.connect(self.store)) as db, db:
                    db.execute("UPDATE meta SET value = 'older-dsp' WHERE key = 'dsp_version'")
                meta = self.meta()
                observer = sqlite3.connect(f"file:{self.store}?mode=ro", uri=True)
                version = observer.execute("PRAGMA data_version").fetchone()[0]
                report = self.diagnose(command)
                committed = observer.execute("PRAGMA data_version").fetchone()[0] != version
                observer.close()
                self.assertFalse(committed, "a diagnostic committed a write")
                self.assertEqual(self.meta(), meta)
                self.assertEqual(report["files"], 1)
                self.assertEqual(report["featured_groups"], 1)
                self.assertEqual(report["featured_stale"], 1)
                self.assertEqual(report["schema_version"], 5)

    def test_read_only_store(self):
        for connection_open in (False, True):
            # setUp closed the store, which removed its WAL and shared-memory
            # files; another open connection (the app reading it) keeps them.
            other = sqlite3.connect(self.store) if connection_open else None
            if other is not None:
                other.execute("SELECT COUNT(*) FROM files").fetchone()
            before = sorted(p.name for p in self.store.parent.iterdir())
            self.assertEqual(len(before), 3 if connection_open else 1, before)
            self.store.chmod(0o444)
            self.store.parent.chmod(0o555)
            try:
                for command in ("status", "doctor"):
                    with self.subTest(command=command, connection_open=connection_open):
                        self.assertEqual(self.diagnose(command)["files"], 1)
                self.assertEqual(sorted(p.name for p in self.store.parent.iterdir()), before)
            finally:
                self.store.parent.chmod(0o755)
                self.store.chmod(0o644)
                if other is not None:
                    other.close()

    def test_reads_last_commit_while_writer_holds_transaction(self):
        writer = subprocess.Popen([sys.executable, "-c", WRITER, str(self.store)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(writer.wait, 30)
        self.addCleanup(writer.stdin.close)
        self.addCleanup(writer.stdout.close)
        self.assertEqual(writer.stdout.readline().strip(), "holding")
        try:
            for command in ("status", "doctor"):
                with self.subTest(command=command):
                    # The writer releases only after the diagnostic returns, so a
                    # diagnostic that waited on it would exhaust its busy timeout.
                    self.assertEqual(self.diagnose(command)["files"], 2)
        finally:
            writer.stdin.write("release\n")
            writer.stdin.flush()


if __name__ == "__main__":
    unittest.main()
