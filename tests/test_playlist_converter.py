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
from zoneinfo import ZoneInfo


CONVERTER = Path(sys.argv[1]).resolve()


def read_jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class PlaylistConverterTest(unittest.TestCase):
    def run_converter(self, source: Path, output: Path, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CONVERTER), "--src", str(source), "--out", str(output), *extra],
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

    def test_rdio_xspf_enriches_isrc_and_repairs_duration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Rdio - Mix.csv"
            source.write_text(
                "Name,Artist,Album,Track Number\n"
                "Short,Artist,Album,1\n"
                "Scaled,Artist,Album,2\n",
                encoding="utf-8",
            )
            source.with_suffix(".xspf").write_text(
                """<playlist xmlns=\"http://xspf.org/ns/0/\" version=\"1\">
<meta rel=\"https://rdio.com/xspf/key\">p1</meta><trackList>
<track><title>Short</title><creator>Artist</creator><duration>180000</duration><meta rel=\"https://rdio.com/xspf/t/isrcs\">USABC1200001</meta></track>
<track><title>Scaled</title><creator>Artist</creator><duration>154000000</duration><meta rel=\"https://rdio.com/xspf/t/isrcs\">bad, GBABC1200002</meta></track>
</trackList></playlist>""",
                encoding="utf-8",
            )
            output = root / "out"
            result = self.run_converter(source, output)
            lines = read_jsonl(next(output.glob("*.jsonl")))

            self.assertEqual(lines[1]["durationMs"], 180000)
            self.assertEqual(lines[2]["durationMs"], 154000)
            self.assertEqual(lines[1]["externalId"], "isrc:USABC1200001")
            self.assertEqual(lines[2]["externalId"], "isrc:GBABC1200002")
            self.assertIn("Supplemented by: Rdio - Mix.xspf", lines[0]["playlist"]["comment"])
            self.assertIn("repaired 1 duration values", result.stderr)

    def test_mismatched_rdio_xspf_is_not_applied(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Rdio - Mix.csv"
            source.write_text("Name,Artist,Album,Track Number\nWanted,Artist,Album,1\n", encoding="utf-8")
            source.with_suffix(".xspf").write_text(
                """<playlist xmlns=\"http://xspf.org/ns/0/\" version=\"1\">
<meta rel=\"https://rdio.com/xspf/key\">p1</meta><trackList>
<track><title>Different</title><creator>Artist</creator><duration>180000</duration></track>
</trackList></playlist>""",
                encoding="utf-8",
            )
            output = root / "out"
            result = self.run_converter(source, output)
            lines = read_jsonl(next(output.glob("*.jsonl")))

            self.assertNotIn("durationMs", lines[1])
            self.assertNotIn("Supplemented by:", lines[0]["playlist"]["comment"])
            self.assertIn("sidecar not applied", result.stderr)

    def test_soundcloud_xml_enriches_numeric_id(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Soundcloud - Mix - playlistCSV1605663596.csv"
            source.write_text(
                "title;artist;album;isrc;addedDate;addedBy;duration;url\n"
                "Song;Artist;;;;;120s;https://soundcloud.com/artist/song\n",
                encoding="utf-8",
            )
            (root / "Soundcloud - Mix - XML playlist1605663900.xml").write_text(
                "<playlist><track><id>169464796</id><title>Song</title><artist>Artist</artist></track></playlist>",
                encoding="utf-8",
            )
            output = root / "out"
            self.run_converter(source, output)
            lines = read_jsonl(next(output.glob("*.jsonl")))

            self.assertEqual(lines[1]["externalId"], "soundcloud:169464796")
            self.assertIn("Supplemented by: Soundcloud - Mix - XML playlist1605663900.xml",
                          lines[0]["playlist"]["comment"])

    def test_naive_added_date_requires_explicit_timezone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Spotify - Mix - playlistCSV1604954896.csv"
            source.write_text(
                "title;artist;album;isrc;addedDate;addedBy;duration;url\n"
                "Known;Artist;;;2019-01-02 03:04;user;120s;\n"
                "Gap;Artist;;;2018-11-04 00:30;user;120s;\n",
                encoding="utf-8",
            )
            output = root / "without-zone"
            self.run_converter(source, output)
            lines = read_jsonl(next(output.glob("*.jsonl")))
            self.assertNotIn("addedAt", lines[1])

            output = root / "with-zone"
            self.run_converter(source, output, "--naive-timezone", "America/Sao_Paulo")
            lines = read_jsonl(next(output.glob("*.jsonl")))
            expected = int(datetime(2019, 1, 2, 3, 4, tzinfo=ZoneInfo("America/Sao_Paulo")).timestamp())
            self.assertEqual(lines[1]["addedAt"], expected)
            self.assertNotIn("addedAt", lines[2])  # DST spring-forward gap

            result = subprocess.run(
                [sys.executable, str(CONVERTER), "--src", str(source), "--out", str(root / "invalid-zone"),
                 "--naive-timezone", "Not/A_Zone"],
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown IANA timezone", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
