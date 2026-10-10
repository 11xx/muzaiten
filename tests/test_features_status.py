"""Diagnostics read only a current-schema feature store.

A current store is read without writes or waiting on a writer. Any other
store (absent, another schema or shape, unreadable) is a data error, exit 4,
that leaves the store, its directories and the provider untouched.
"""
from contextlib import closing
import hashlib
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
# A provider candidate that records each run; its failed handshake leaves the
# semantic state "provider-missing".
PROVIDER = f"""#!{sys.executable}
import pathlib, sys
pathlib.Path(sys.argv[0] + ".ran").touch()
sys.exit(1)
"""


EMPTY = hashlib.sha256(b"").hexdigest()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entry(path):
    if not path.is_file():
        return "dir"
    if path.name.endswith("-shm"):
        # Every reader updates its read marks in the shared-memory index.
        return "shm"
    try:
        return digest(path)
    except PermissionError:
        status = path.stat()
        return f"unreadable mode {status.st_mode:o} size {status.st_size}"


def tree(root):
    """Every entry under root, with each readable file's digest."""
    return {str(p.relative_to(root)): entry(p) for p in sorted(root.rglob("*"))}


def without_new_sidecars(before, after, store, root):
    """after, less the sidecars a read-only WAL reader may add for store.

    Opening a WAL store read-only in a writable directory creates an empty
    -wal and a -shm index when they are absent. Neither is a write, but the
    -wal must stay empty.
    """
    after = dict(after)
    for suffix in ("-wal", "-shm"):
        name = str(Path(f"{store}{suffix}").relative_to(root))
        if name not in before and name in after:
            if suffix == "-wal" and after[name] != EMPTY:
                continue
            del after[name]
    return after


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
        self.library = self.root / "library.sqlite"
        with closing(sqlite3.connect(self.library)) as db, db:
            db.execute("CREATE TABLE tracks(path TEXT, file_mtime INTEGER, file_size INTEGER,"
                       " missing INTEGER, scan_error TEXT)")
        self.store.parent.mkdir()
        # The native writer creates the current schema from an empty library.
        self.features("refresh", "--library", str(self.library), "--no-semantic", check=True)
        with closing(sqlite3.connect(self.store)) as db, db:
            db.execute("INSERT INTO content_groups(id) VALUES (1)")
            db.execute("INSERT INTO files(path, mtime, size, analyzed_at, content_group_id)"
                       " VALUES ('/synthetic/a.flac', 1, 2, 3, 1)")
            db.execute("INSERT INTO features(content_group_id, tempo_bpm, extractor, version)"
                       " VALUES (1, 120, 'cpp', 'older-dsp')")
            db.execute("UPDATE meta SET value = 'older-dsp' WHERE key = 'dsp_version'")
        self.addCleanup(self.store.parent.chmod, 0o755)
        self.provider = self.root / "provider"
        self.provider.write_text(PROVIDER)
        self.provider.chmod(0o755)
        self.provider_ran = Path(f"{self.provider}.ran")

    def features(self, command, *args, check=False, store=None, env=None):
        result = subprocess.run([str(BINARY), command, "--features", str(store or self.store), "--state",
                                 str(self.root / "absent-state.sqlite"), *args],
                                env=env or self.env, capture_output=True, text=True, timeout=60)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def diagnose(self, command):
        result = self.features(command, "--provider", str(self.root / "absent-provider"), "--json")
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        report = json.loads(result.stdout)
        report.pop("semantic")
        return report

    def refuse(self, command, store, state, *, env=None, default_paths=False):
        """Runs a diagnostic that must fail as a data error, and returns its stderr."""
        args = [str(BINARY), command, "--provider", str(self.provider), "--json"]
        if not default_paths:
            args[2:2] = ["--features", str(store), "--state", str(self.root / "absent-state.sqlite")]
        result = subprocess.run(args, env=env or self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn(f"muzaiten-features: feature store {state}: {store}", result.stderr)
        self.assertEqual(result.stderr.count("\n"), 1, result.stderr)
        self.assertFalse(self.provider_ran.exists(), "a refused diagnostic invoked the provider")
        return result.stderr

    def meta(self):
        db = sqlite3.connect(f"file:{self.store}?mode=ro", uri=True)
        try:
            return dict(db.execute("SELECT key, value FROM meta"))
        finally:
            db.close()

    def snapshot(self, store):
        """Logical contents, the commit counter and every file's bytes.

        The observer stays open so that data_version reveals a commit by any
        other connection; it also creates the store's WAL sidecars first.
        """
        observer = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
        try:
            return (list(observer.iterdump()), observer.execute("PRAGMA data_version").fetchone()[0],
                    tree(self.root), observer)
        except BaseException:
            observer.close()
            raise

    def assert_unchanged(self, store, before):
        dump, version, files, observer = before
        try:
            self.assertEqual(observer.execute("PRAGMA data_version").fetchone()[0], version,
                             "a diagnostic committed a write")
            self.assertEqual(list(observer.iterdump()), dump)
        finally:
            observer.close()
        self.assertEqual(without_new_sidecars(files, tree(self.root), store, self.root), files)

    def test_current_store_is_read_without_writes(self):
        for command in ("status", "doctor"):
            with self.subTest(command=command):
                with closing(sqlite3.connect(self.store)) as db, db:
                    db.execute("UPDATE meta SET value = 'older-dsp' WHERE key = 'dsp_version'")
                meta = self.meta()
                before = self.snapshot(self.store)
                report = self.diagnose(command)
                self.assert_unchanged(self.store, before)
                self.assertEqual(self.meta(), meta)
                self.assertEqual(report["files"], 1)
                self.assertEqual(report["featured_groups"], 1)
                self.assertEqual(report["featured_stale"], 1)
                self.assertEqual(report["schema_version"], 5)

    def test_current_store_consults_provider(self):
        # Positive control for the provider marker the refusals check.
        for command in ("status", "doctor"):
            with self.subTest(command=command):
                self.provider_ran.unlink(missing_ok=True)
                result = self.features(command, "--provider", str(self.provider), "--json", check=True)
                self.assertEqual(json.loads(result.stdout)["semantic"]["state"], "provider-missing")
                self.assertTrue(self.provider_ran.exists())
        self.provider_ran.unlink()

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

    def test_absent_store_is_not_created(self):
        missing_file = self.store.parent / "absent.sqlite"
        missing_parent = self.root / "absent-dir" / "nested" / "features.sqlite"
        for store in (missing_file, missing_parent):
            for command in ("status", "doctor"):
                with self.subTest(command=command, store=store.relative_to(self.root)):
                    before = tree(self.root)
                    stderr = self.refuse(command, store, "absent")
                    self.assertIn("does not exist", stderr)
                    self.assertEqual(tree(self.root), before)

    def test_absent_store_at_default_paths_creates_no_directories(self):
        state_root = self.root / "fresh-root"
        home = self.root / "fresh-home"
        home.mkdir()
        xdg_env = {k: v for k, v in self.env.items() if k != "MUZAITEN_STATE_ROOT"}
        xdg_env.update(HOME=str(home))
        cases = (
            ("state root", dict(self.env, MUZAITEN_STATE_ROOT=str(state_root)),
             state_root / "data" / "features.sqlite"),
            ("XDG", xdg_env, home / ".local" / "share" / "muzaiten" / "features.sqlite"),
        )
        for name, env, store in cases:
            for command in ("status", "doctor"):
                with self.subTest(command=command, paths=name):
                    before = tree(self.root)
                    self.refuse(command, store, "absent", env=env, default_paths=True)
                    self.assertEqual(tree(self.root), before)
        self.assertFalse(state_root.exists())
        self.assertEqual(list(home.iterdir()), [])

    def unsupported_stores(self):
        """Stores SQLite reads but this build does not, with their expected causes."""
        pre_v3 = self.root / "pre-v3" / "features.sqlite"
        pre_v3.parent.mkdir()
        with closing(sqlite3.connect(pre_v3)) as db, db:
            db.executescript(
                "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
                "INSERT INTO meta VALUES ('schema_version', '2'), ('bliss_version', '1');"
                "CREATE TABLE files(path TEXT PRIMARY KEY, mtime INTEGER NOT NULL, size INTEGER NOT NULL,"
                " content_group_id INTEGER, analyzed_at INTEGER NOT NULL);"
                "CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);"
                "CREATE TABLE features(content_group_id INTEGER PRIMARY KEY, analysis BLOB NOT NULL);"
                "INSERT INTO files VALUES ('/synthetic/a.flac', 1, 2, 1, 3);"
                "INSERT INTO content_groups(id) VALUES (1);"
                "INSERT INTO features VALUES (1, x'0102');")
        yield pre_v3, "has schema version 2, older than this build's 5"

        def variant(name, cause, *statements):
            store = self.root / name / "features.sqlite"
            store.parent.mkdir()
            with closing(sqlite3.connect(self.store)) as source, closing(sqlite3.connect(store)) as target:
                source.backup(target)
                with target:
                    for statement in statements:
                        target.execute(statement)
            return store, cause

        yield variant("newer", "has schema version 99, newer than this build's 5",
                      "UPDATE meta SET value = '99' WHERE key = 'schema_version'")
        yield variant("unversioned", "records no schema version",
                      "DELETE FROM meta WHERE key = 'schema_version'")
        yield variant("missing-table", "lacks the track_neighbors table", "DROP TABLE track_neighbors")
        yield variant("feature-columns", "has an incompatible features table",
                      "ALTER TABLE features DROP COLUMN energy")
        yield variant("embedding-columns", "lacks embeddings.generation_id",
                      "DROP TABLE embeddings",
                      "CREATE TABLE embeddings(content_group_id INTEGER PRIMARY KEY, model TEXT,"
                      " version TEXT, dim INTEGER, vector BLOB)")
        yield variant("unfinished-migration", "has an unfinished embeddings_v4 migration",
                      "CREATE TABLE embeddings_v4(content_group_id INTEGER PRIMARY KEY)")

    def test_unsupported_store_is_reported_unchanged(self):
        for store, cause in self.unsupported_stores():
            for command in ("status", "doctor"):
                with self.subTest(command=command, store=store.parent.name):
                    before = self.snapshot(store)
                    stderr = self.refuse(command, store, "unsupported")
                    self.assertIn(cause, stderr)
                    self.assert_unchanged(store, before)

    def test_unreadable_store_is_reported_unchanged(self):
        garbage = self.root / "garbage" / "features.sqlite"
        garbage.parent.mkdir()
        garbage.write_bytes(b"not an SQLite database\n" * 256)
        # A current store cut to its first page: the schema is readable, the
        # tables it names are not.
        truncated = self.root / "truncated" / "features.sqlite"
        truncated.parent.mkdir()
        truncated.write_bytes(self.store.read_bytes()[:4096])
        # Cut after meta and its index: the schema check passes, the status
        # counts fail.
        with closing(sqlite3.connect(f"file:{self.store}?mode=ro", uri=True)) as db:
            page_size = db.execute("PRAGMA page_size").fetchone()[0]
            pages = dict(db.execute("SELECT name, rootpage FROM sqlite_master WHERE rootpage > 0"))
        meta_end = max(pages["meta"], pages["sqlite_autoindex_meta_1"])
        self.assertLess(meta_end, pages["files"])
        counts_cut = self.root / "counts-cut" / "features.sqlite"
        counts_cut.parent.mkdir()
        counts_cut.write_bytes(self.store.read_bytes()[:meta_end * page_size])
        directory = self.root / "directory" / "features.sqlite"
        directory.mkdir(parents=True)
        cases = [(garbage, "file is not a database"), (truncated, "malformed"), (counts_cut, "malformed"),
                 (directory, "not a file")]
        if os.geteuid() != 0:
            # Root bypasses the permission bits this case relies on.
            unreadable = self.root / "unreadable" / "features.sqlite"
            unreadable.parent.mkdir()
            unreadable.write_bytes(self.store.read_bytes())
            unreadable.chmod(0)
            self.addCleanup(unreadable.chmod, 0o644)
            cases.append((unreadable, "unable to open"))
        for store, cause in cases:
            for command in ("status", "doctor"):
                with self.subTest(command=command, store=store.parent.name):
                    before = tree(self.root)
                    stderr = self.refuse(command, store, "unreadable")
                    self.assertIn(cause, stderr)
                    self.assertEqual(without_new_sidecars(before, tree(self.root), store, self.root), before)

    def test_refresh_still_creates_store(self):
        explicit = self.root / "new-dir" / "nested" / "features.sqlite"
        state_root = self.root / "fresh-root"
        cases = (
            ("explicit", ["--features", str(explicit)], self.env, explicit),
            ("state root", [], dict(self.env, MUZAITEN_STATE_ROOT=str(state_root)),
             state_root / "data" / "features.sqlite"),
        )
        for name, args, env, store in cases:
            with self.subTest(paths=name):
                result = subprocess.run([str(BINARY), "refresh", "--library", str(self.library),
                                         "--no-semantic", *args],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                with closing(sqlite3.connect(f"file:{store}?mode=ro", uri=True)) as db:
                    self.assertEqual(db.execute("SELECT value FROM meta WHERE key = 'schema_version'")
                                     .fetchone(), ("5",))
                self.assertEqual(self.features("status", "--json", store=store, env=env,
                                               check=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
