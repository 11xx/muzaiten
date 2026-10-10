"""Report the local build identity from the installed CLIs without state or IPC."""
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

CMAKE, BUILD = sys.argv.pop(1), Path(sys.argv.pop(1)).resolve()


class CliVersionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.prefix_dir = tempfile.TemporaryDirectory(prefix="muzaiten-cli-prefix-")
        prefix = Path(cls.prefix_dir.name)
        # Local-only install rules into a private prefix: the binaries as packaged,
        # never an account's live prefix and never the build's install_manifest.txt.
        subprocess.run([CMAKE, "-DCMAKE_INSTALL_LOCAL_ONLY=1", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                        "-P", str(BUILD / "cmake_install.cmake")], check=True, capture_output=True)
        (BUILD / "install_local_manifest.txt").unlink(missing_ok=True)
        cls.bin = prefix / "bin"

    @classmethod
    def tearDownClass(cls):
        cls.prefix_dir.cleanup()

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="muzaiten-cli-version-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("MUZAITEN_", "XDG_")) and k != "HOME"}
        self.env.update(HOME=str(self.root / "home"), XDG_RUNTIME_DIR=str(self.runtime),
                        MUZAITEN_STATE_ROOT=str(self.root / "state"), QT_QPA_PLATFORM="offscreen")
        # Clients of this isolated instance connect here (IpcSocket::serverPath).
        identity = str(self.root / "state" / "state").encode()
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(self.server.close)
        self.server.bind(str(self.runtime / f"muzaiten-{hashlib.sha1(identity).hexdigest()[:8]}.sock"))
        self.server.listen(4)
        self.server.setblocking(False)

    def run_binary(self, name, *args):
        return subprocess.run([str(self.bin / name), *args], env=self.env, capture_output=True,
                              text=True, timeout=30)

    def accepted(self):
        count = 0
        while True:
            try:
                connection, _ = self.server.accept()
            except BlockingIOError:
                return count
            connection.close()
            count += 1

    def entries(self):
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*"))

    def test_version_is_local_and_matches_application(self):
        before = self.entries()
        app = self.run_binary("muzaiten", "--version")
        self.assertEqual(app.returncode, 0, app.stderr)
        name, version = app.stdout.split()
        self.assertEqual(name, "muzaiten")
        for binary in ("muzaitenctl", "muzaiten-features"):
            with self.subTest(binary=binary):
                result = self.run_binary(binary, "--version")
                self.assertEqual((result.returncode, result.stdout, result.stderr),
                                 (0, f"{binary} {version}\n", ""))
                self.assertEqual(self.accepted(), 0)
                self.assertEqual(self.entries(), before)

    def test_ordinary_commands_are_unchanged(self):
        result = self.run_binary("muzaitenctl", "--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--version", result.stderr)
        self.assertEqual(self.run_binary("muzaitenctl").returncode, 1)
        result = self.run_binary("muzaiten-features", "--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("muzaiten-features --version", result.stderr)
        self.assertEqual(self.run_binary("muzaiten-features").returncode, 2)
        result = self.run_binary("muzaiten-features", "--versions")
        self.assertEqual((result.returncode, result.stderr),
                         (2, 'muzaiten-features: unknown command "--versions"\n'))
        self.assertEqual(self.accepted(), 0)
        # Version is the only argument: anything else keeps the ordinary command path,
        # which is what reaches the instance socket (the listener is live).
        result = self.run_binary("muzaitenctl", "--version", "status")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.accepted(), 1)


if __name__ == "__main__":
    unittest.main()
