"""Exercise startup diagnostics against isolated essential stores."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv.pop(1)).resolve()
STORES = {"library": "data/library.sqlite", "playlists": "data/playlists.sqlite",
          "state": "state/state.sqlite", "history": "data/history.sqlite"}


class StartupStorageTest(unittest.TestCase):
    def run_app(self, root, check=False):
        env = {k: v for k, v in os.environ.items() if not k.startswith("MUZAITEN_")}
        env.update(QT_QPA_PLATFORM="offscreen", MUZAITEN_STATE_ROOT=str(root), MUZAITEN_DEMO_SILENT_AUDIO="1")
        args = [str(APP)] + (["--check-storage"] if check else [])
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=15)
        reports = [json.loads(line) for line in (result.stdout + result.stderr).splitlines() if line.startswith('{')]
        self.assertTrue(reports, result.stderr)
        return result, reports[-1]

    def test_corrupt_required_store_blocks_startup_before_other_stores(self):
        for name, relative in STORES.items():
            with self.subTest(store=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                path = root / relative
                path.parent.mkdir(parents=True)
                path.write_bytes(b"invalid SQLite database")
                result, report = self.run_app(root)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(report["code"], "storage_unavailable")
                self.assertTrue(any(i["store"] == name and i["required"] for i in report["issues"]))
                self.assertEqual(path.read_bytes(), b"invalid SQLite database")
                self.assertEqual({p.relative_to(root).as_posix() for p in root.rglob("*.sqlite")}, {relative})

    def test_newer_formats_are_rejected_and_not_rewritten(self):
        for name, relative in STORES.items():
            with self.subTest(store=name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                path = root / relative
                path.parent.mkdir(parents=True)
                with sqlite3.connect(path) as db:
                    if name in ("library", "playlists"):
                        db.execute("CREATE TABLE schema_migrations(version INTEGER, applied_at TEXT)")
                        db.execute("INSERT INTO schema_migrations VALUES (999, 'fixture')")
                    else:
                        db.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)")
                        db.execute("INSERT INTO meta VALUES ('schemaVersion', '999')")
                result, report = self.run_app(root)
                self.assertEqual(result.returncode, 2)
                self.assertIn("Unsupported schema version", report["issues"][0]["error"])
                with sqlite3.connect(path) as db:
                    value = db.execute("SELECT MAX(version) FROM schema_migrations" if name in ("library", "playlists")
                                       else "SELECT value FROM meta WHERE key='schemaVersion'").fetchone()[0]
                    self.assertEqual(str(value), "999")

    def test_optional_corruption_is_degraded_not_fatal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "cache/artwork.sqlite"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"bad cache")
            result, report = self.run_app(root, check=True)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(report["code"], "degraded")
            self.assertFalse(report["issues"][0]["required"])

    def test_bad_parent_and_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "data").write_text("not a directory")
            result, _ = self.run_app(root, check=True)
            self.assertEqual(result.returncode, 2)
            (root / "data").unlink()
            result, report = self.run_app(root, check=True)
            self.assertEqual(result.returncode, 0)
            self.assertTrue(report["ok"])
            self.assertFalse(list(root.rglob("*.sqlite")))

    def test_optional_cache_failure_still_captures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / "cache/artwork.sqlite"
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b"unavailable cache")
            env = {k: v for k, v in os.environ.items() if not k.startswith("MUZAITEN_")}
            env.update(QT_QPA_PLATFORM="offscreen", MUZAITEN_STATE_ROOT=str(root))
            result = subprocess.run([str(APP), "--demo-screens", str(root / "images"), "--demo-theme", "light",
                                     "--demo-size", "640x480", "--demo-file-explorer-system-path", str(root)],
                                    env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("cache", result.stderr.lower())
            self.assertEqual(len(list((root / "images").rglob("*.png"))), 6)
            self.assertEqual(cache.read_bytes(), b"unavailable cache")

    def test_readonly_required_file(self):
        if os.geteuid() == 0:
            self.skipTest("root bypasses permissions")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "data/library.sqlite"
            path.parent.mkdir(parents=True)
            with sqlite3.connect(path):
                pass
            path.chmod(0o400)
            result, report = self.run_app(root)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(report["issues"][0]["store"], "library")


if __name__ == "__main__":
    unittest.main()
