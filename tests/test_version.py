"""Exercise native version callers against isolated Git histories."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VersionTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="muzaiten-version-", dir=Path.home())
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.env = dict(os.environ, GIT_AUTHOR_DATE="2026-09-11T02:00:00Z",
                        GIT_COMMITTER_DATE="2026-09-11T02:00:00Z", TZ="Pacific/Honolulu")
        self.git("init", "-q")
        (self.root / "tools").mkdir()
        (self.root / "cmake").mkdir()
        shutil.copy(ROOT / "tools/version.py", self.root / "tools/version.py")
        shutil.copy(ROOT / "cmake/MuzaitenVersion.cmake", self.root / "cmake/MuzaitenVersion.cmake")
        (self.root / "probe.cmake").write_text(
            'include(cmake/MuzaitenVersion.cmake)\n'
            'message("VERSION=${MUZAITEN_VERSION}")\n')
        self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-c", "commit.gpgsign=false", "-c",
                                       "user.name=Test", "-c", "user.email=test@example.invalid",
                                       *args], cwd=self.root, env=self.env, text=True).strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "--allow-empty", "-qm", "fixture")

    def version(self):
        result = subprocess.check_output([sys.executable, str(self.root / "tools/version.py"),
                                          "--json"], cwd=self.root, env=self.env, text=True)
        return json.loads(result)

    def test_release_development_dirty_and_next_day(self):
        self.assertRegex(self.version()["version"], r"^2026\.9\.11a0\.dev1\+g[0-9a-f]+$")
        self.git("tag", "2026.9.11")
        self.assertEqual(self.version()["version"], "2026.9.11")
        (self.root / "untracked").touch()
        self.assertRegex(self.version()["version"], r"^2026\.9\.11\.1a0\.dev0\+g[0-9a-f]+\.dirty$")
        self.commit()
        self.assertRegex(self.version()["version"], r"^2026\.9\.11\.1a0\.dev1\+g[0-9a-f]+$")
        self.git("tag", "2026.9.11.1")
        self.assertEqual(self.version()["version"], "2026.9.11.1")
        self.env.update(GIT_AUTHOR_DATE="2026-09-12T00:01:00Z", GIT_COMMITTER_DATE="2026-09-12T00:01:00Z")
        self.commit()
        self.assertRegex(self.version()["version"], r"^2026\.9\.12a0\.dev1\+g[0-9a-f]+$")

    def test_legacy_tags_and_cmake_agree(self):
        self.git("tag", "2026.09.11")
        self.git("tag", "2026.99.99")
        self.assertEqual(self.version()["version"], "2026.9.11")
        result = subprocess.run(["cmake", "-P", "probe.cmake"], cwd=self.root,
                                env=self.env, text=True, capture_output=True, check=True)
        self.assertIn("VERSION=2026.9.11", result.stderr)
        package = subprocess.check_output(
            ["bash", "-c", 'source "$1"; srcdir="$2"; pkgname="$3"; pkgver', "version-test",
             str(ROOT / "packaging/aur/muzaiten-git/PKGBUILD"),
             str(self.root.parent), self.root.name], text=True)
        self.assertEqual(package.strip(), "2026.9.11")

    @unittest.skipUnless(shutil.which("vercmp"), "Arch comparator is not installed")
    def test_arch_ordering(self):
        versions = ["2026.9.11a0.dev1+gabc", "2026.9.11",
                    "2026.9.11.1a0.dev1+gabc", "2026.9.11.1"]
        for older, newer in zip(versions, versions[1:]):
            self.assertEqual(subprocess.check_output(["vercmp", older, newer], text=True).strip(), "-1")

    def test_release_guard_and_source_archive_fallback(self):
        command = [sys.executable, str(self.root / "tools/version.py"), "--release"]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
        self.git("tag", "2026.9.11")
        self.assertEqual(subprocess.check_output(command, text=True).strip(), "2026.9.11")
        self.git("update-ref", "-d", "refs/tags/2026.9.11")
        shutil.rmtree(self.root / ".git")
        self.assertEqual(self.version()["version"], "0.0.0+unknown")


if __name__ == "__main__":
    unittest.main()
