#!/usr/bin/env python3
"""Stdlib fixture test for the standalone playlist-export converter."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path


CONVERTER = Path(sys.argv[1]).resolve()


def read_jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class PlaylistConverterTest(unittest.TestCase):
    def run_converter(self, source: Path, output: Path) -> None:
        subprocess.run(
            [sys.executable, str(CONVERTER), "--src", str(source), "--out", str(output)],
            check=True,
            text=True,
            capture_output=True,
        )

    def test_soundiiz_added_at_and_filename_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Spotify - Forza - (484) playlistCSV1604954896.csv"
            source.write_text(
                "title;artist;album;isrc;addedDate;addedBy;duration;url\n"
                "Alpha;Artist;Album;USAAA0000001;2019-01-02T03:04:05Z;user;225s;https://open.spotify.com/track/abc\n"
                "Beta;Artist;Album;USAAA0000002;;user;180s;https://open.spotify.com/track/def\n"
                "Gamma;Artist;Album;USAAA0000003;not-a-timestamp;user;210s;https://open.spotify.com/track/ghi\n",
                encoding="utf-8",
            )
            # The filename timestamp must win even when mtime says something else.
            os.utime(source, (946684800, 946684800))
            output = root / "out"
            self.run_converter(source, output)

            lines = read_jsonl(next(output.glob("*.jsonl")))
            comment = lines[0]["playlist"]["comment"]
            self.assertEqual(
                comment,
                "Source: Spotify via Soundiiz\n"
                "Exported: 2020-11-09 20:48:16 UTC (filename timestamp)\n"
                "File: Spotify - Forza - (484) playlistCSV1604954896.csv",
            )
            self.assertEqual(lines[1]["addedAt"], 1546398245)
            self.assertNotIn("addedAt", lines[2])
            self.assertNotIn("addedAt", lines[3])
            self.assertNotIn("comment", lines[1])
            self.assertNotIn("comment", lines[2])
            self.assertNotIn("comment", lines[3])

    def test_mtime_provenance_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Rdio - Morning.csv"
            source.write_text("Name,Artist,Album,Track Number\nMorning,Artist,Album,1\n", encoding="utf-8")
            timestamp = int(datetime(2015, 6, 7, 8, 9, 10, tzinfo=timezone.utc).timestamp())
            os.utime(source, (timestamp, timestamp))
            output = root / "out"
            self.run_converter(source, output)

            lines = read_jsonl(next(output.glob("*.jsonl")))
            self.assertEqual(
                lines[0]["playlist"]["comment"],
                "Source: Rdio\n"
                "File modified: 2015-06-07 08:09:10 UTC (filesystem timestamp)\n"
                "File: Rdio - Morning.csv",
            )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
