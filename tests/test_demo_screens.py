"""Exercise the demo runner with a live WAL profile and replaceable outputs."""

import base64
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[1] / "tools/demo-screens.py"
PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0ioAAAAASUVORK5CYII="


class DemoRunnerTest(unittest.TestCase):
    def test_live_wal_repeated_capture_and_optimizer_skip(self):
        with tempfile.TemporaryDirectory(prefix="demo-test-") as temporary:
            root = Path(temporary)
            data = root / "data/muzaiten"
            data.mkdir(parents=True)
            source = sqlite3.connect(data / "library.sqlite")
            self.addCleanup(source.close)
            source.execute("PRAGMA journal_mode=WAL")
            source.execute("CREATE TABLE queue (value TEXT)")
            source.commit()
            source.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            source.execute("INSERT INTO queue VALUES ('fresh')")
            source.commit()
            # A raw main-file copy misses the committed WAL row.
            raw = root / "raw.sqlite"
            raw.write_bytes((data / "library.sqlite").read_bytes())
            with sqlite3.connect(raw) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM queue").fetchone()[0], 0)
            app = root / "capture"
            app.write_text(f'''#!{sys.executable}
import os, sqlite3, sys, base64
from pathlib import Path
state = Path(os.environ['MUZAITEN_STATE_ROOT'])
assert 'MUZAITEN_DATA_DIR' not in os.environ
with sqlite3.connect(state / 'data/library.sqlite') as db:
    value = db.execute('SELECT value FROM queue').fetchone()[0]
assert value == os.environ['EXPECTED_QUEUE']
out = Path(sys.argv[sys.argv.index('--demo-screens') + 1]) / 'light'
out.mkdir(parents=True)
(out / '01-library.png').write_bytes(base64.b64decode('{PNG}'))
''')
            app.chmod(0o700)
            optimizer = root / "optimizer"
            optimizer.write_text(f"#!{sys.executable}\nimport os\nraise SystemExit(int(os.environ['OPTIMIZER_EXIT']))\n")
            optimizer.chmod(0o700)
            output = root / "output"
            (output / "light").mkdir(parents=True)
            image = output / "light/01-library.png"
            image.write_bytes(b"old")
            unrelated = output / "keep.txt"
            unrelated.write_text("keep")
            obsolete = output / "light/05-explorer.png"
            obsolete.write_bytes(b"stale explorer")
            env = dict(os.environ, XDG_DATA_HOME=str(root / "data"),
                       XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"),
                       DEMO_PNG_QUANTIZER=str(optimizer), DEMO_PNG_OPTIMIZER=str(optimizer),
                       DEMO_PNG_OPTIMIZER_FLAGS="", DEMO_PNG_QUANTIZER_FLAGS="",
                       DEMO_OPTIMIZE_PNG="1", DEMO_PNG_LOSSY="1", OPTIMIZER_EXIT="0",
                       MUZAITEN_STATE_ROOT=str(root / "stale"), EXPECTED_QUEUE="fresh")
            env["MUZAITEN_DATA_DIR"] = str(root / "wrong-data")
            command = [sys.executable, str(RUNNER), "--app", str(app), "--output", str(output)]
            for value in ("fresh", "newer"):
                source.execute("UPDATE queue SET value=?", (value,))
                source.commit()
                env["EXPECTED_QUEUE"] = value
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(image.read_bytes(), base64.b64decode(PNG))
            # pngquant skips are successful; oxipng errors remain failures.
            env["DEMO_PNG_OPTIMIZER"] = "missing-demo-optimizer"
            for skip in ("98", "99"):
                env["OPTIMIZER_EXIT"] = skip
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            image.write_bytes(b"last successful capture")
            env["OPTIMIZER_EXIT"] = "7"
            result = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(image.read_bytes(), b"last successful capture")
            self.assertEqual(unrelated.read_text(), "keep")
            self.assertFalse(obsolete.exists())
            self.assertFalse(list(root.glob(".demo-run-*")))


if __name__ == "__main__":
    unittest.main()
