from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

import pytest

from muzaiten_embed import db
from muzaiten_embed.cli import main
from muzaiten_embed.ops import neighbors, query_embedding, scan, status


@dataclass
class FakeEmbedder:
    vectors: dict[Path, tuple[float, ...]]
    text_vectors: dict[str, tuple[float, ...]] = field(default_factory=dict)
    model: str = "fake-clap"
    version: str = "fixture"
    calls: list[Path] = field(default_factory=list)

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        self.calls.append(path)
        return self.vectors[path]

    def embed_text(self, text: str) -> Sequence[float]:
        return self.text_vectors[text]


@pytest.fixture
def features_path(tmp_path: Path) -> Path:
    path = tmp_path / "features.sqlite"
    with sqlite3.connect(path) as conn:
        conn.executescript(
            """
            CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE files(
                path TEXT PRIMARY KEY,
                mtime INTEGER NOT NULL,
                size INTEGER NOT NULL,
                duration_ms INTEGER,
                decode_hash TEXT,
                chromaprint_fp BLOB,
                content_group_id INTEGER,
                analyzed_at INTEGER NOT NULL,
                status TEXT NOT NULL DEFAULT 'ok'
            );
            CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);
            CREATE TABLE features(
                content_group_id INTEGER PRIMARY KEY,
                bliss_vector BLOB NOT NULL,
                tempo_bpm REAL,
                loudness REAL,
                energy REAL,
                brightness REAL,
                extractor TEXT NOT NULL,
                version TEXT NOT NULL
            );
            """
        )
        conn.execute("INSERT INTO meta(key, value) VALUES('schema_version', '1')")
        conn.executemany(
            "INSERT INTO content_groups(id) VALUES(?)",
            [(10,), (11,), (12,), (13,)],
        )
        conn.executemany(
            "INSERT INTO files(path, mtime, size, duration_ms, decode_hash, content_group_id, "
            "analyzed_at, status) VALUES(?, 1, 2, 3000, ?, ?, 1000, ?)",
            [
                ("/music/a-copy.flac", "hash-a", 10, "ok"),
                ("/music/a.flac", "hash-a", 10, "ok"),
                ("/music/b.flac", "hash-b", 11, "ok"),
                ("/music/c.flac", "hash-c", 12, "ok"),
                ("/music/d.flac", "hash-d", 13, "ok"),
                ("/music/broken.flac", None, None, "decode_failed"),
            ],
        )
    return path


def test_scan_upgrades_schema_and_skips_existing_embeddings(features_path: Path) -> None:
    fake = FakeEmbedder(
        {
            Path("/music/a-copy.flac"): (3.0, 0.0),
            Path("/music/b.flac"): (0.0, 4.0),
            Path("/music/c.flac"): (1.0, 1.0),
            Path("/music/d.flac"): (-2.0, 0.0),
        },
    )

    first = scan(features_path, fake, limit=3)
    assert first.as_dict() == {"groups": 3, "embedded": 3, "skipped": 0}
    assert fake.calls == [
        Path("/music/a-copy.flac"),
        Path("/music/b.flac"),
        Path("/music/c.flac"),
    ]

    with db.connect(features_path) as conn:
        assert db.read_schema_version(conn) == 2
        rows = conn.execute(
            "SELECT content_group_id, model, version, dim, vector FROM embeddings "
            "ORDER BY content_group_id",
        ).fetchall()
        assert [int(row["content_group_id"]) for row in rows] == [10, 11, 12]
        assert {row["model"] for row in rows} == {"fake-clap"}
        assert {row["version"] for row in rows} == {"fixture"}
        assert {int(row["dim"]) for row in rows} == {2}
        assert db.unpack_vector(rows[0]["vector"], 2) == pytest.approx((1.0, 0.0))
        assert db.unpack_vector(rows[1]["vector"], 2) == pytest.approx((0.0, 1.0))

    second = scan(features_path, fake, limit=3)
    assert second.as_dict() == {"groups": 3, "embedded": 0, "skipped": 3}
    assert fake.calls == [
        Path("/music/a-copy.flac"),
        Path("/music/b.flac"),
        Path("/music/c.flac"),
    ]


def test_schema_three_is_accepted_without_downgrade(tmp_path: Path) -> None:
    path = tmp_path / "features.sqlite"
    with sqlite3.connect(path) as conn:
        conn.executescript(
            """
            CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE files(
                path TEXT PRIMARY KEY,
                mtime INTEGER NOT NULL,
                size INTEGER NOT NULL,
                duration_ms INTEGER,
                decode_hash TEXT,
                chromaprint_fp BLOB,
                content_group_id INTEGER,
                analyzed_at INTEGER NOT NULL,
                status TEXT NOT NULL DEFAULT 'ok'
            );
            CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);
            CREATE TABLE features(
                content_group_id INTEGER PRIMARY KEY,
                tempo_bpm REAL,
                loudness_lufs REAL,
                loudness_std_db REAL,
                spectral_centroid_mean_hz REAL,
                spectral_centroid_std_hz REAL,
                spectral_flatness_mean REAL,
                zero_crossing_rate REAL,
                onset_rate_hz REAL,
                energy REAL,
                extractor TEXT NOT NULL,
                version TEXT NOT NULL
            );
            """
        )
        conn.execute("INSERT INTO meta(key, value) VALUES('schema_version', '3')")

    with db.connect(path) as conn:
        db.ensure_schema(conn)
        assert db.read_schema_version(conn) == 3
        assert conn.execute("SELECT name FROM sqlite_master WHERE name = 'embeddings'").fetchone() is not None
        assert conn.execute("SELECT name FROM sqlite_master WHERE name = 'track_neighbors'").fetchone() is not None


def test_neighbors_are_ranked_by_cosine_then_group_id(features_path: Path) -> None:
    fake = FakeEmbedder(
        {
            Path("/music/a-copy.flac"): (1.0, 0.0),
            Path("/music/b.flac"): (0.0, 1.0),
            Path("/music/c.flac"): (1.0, 1.0),
            Path("/music/d.flac"): (-1.0, 0.0),
        },
    )
    scan(features_path, fake)

    count = neighbors(features_path, fake.model, fake.version, top_k=2)

    assert count == 8
    with db.connect(features_path) as conn:
        rows = conn.execute(
            "SELECT content_group_id, neighbor_group_id, rank, cosine "
            "FROM track_neighbors ORDER BY content_group_id, rank",
        ).fetchall()
    actual = [
        (
            int(row["content_group_id"]),
            int(row["neighbor_group_id"]),
            int(row["rank"]),
            float(row["cosine"]),
        )
        for row in rows
    ]
    expected = [
        (10, 12, 1, 2**-0.5),
        (10, 11, 2, 0.0),
        (11, 12, 1, 2**-0.5),
        (11, 10, 2, 0.0),
        (12, 10, 1, 2**-0.5),
        (12, 11, 2, 2**-0.5),
        (13, 11, 1, 0.0),
        (13, 12, 2, -(2**-0.5)),
    ]
    assert [row[:3] for row in actual] == [row[:3] for row in expected]
    assert [row[3] for row in actual] == pytest.approx([row[3] for row in expected])


def test_status_reports_coverage_before_and_after_scan(features_path: Path) -> None:
    before = status(features_path, "fake-clap", "fixture")
    assert before.as_dict() == {
        "schema_version": 1,
        "groups": 4,
        "embeddings": 0,
        "model_embeddings": 0,
        "neighbor_rows": 0,
    }

    fake = FakeEmbedder(
        {
            Path("/music/a-copy.flac"): (1.0, 0.0),
            Path("/music/b.flac"): (0.0, 1.0),
            Path("/music/c.flac"): (1.0, 1.0),
            Path("/music/d.flac"): (-1.0, 0.0),
        },
    )
    scan(features_path, fake, limit=2)
    neighbors(features_path, fake.model, fake.version, top_k=1)

    after = status(features_path, fake.model, fake.version)
    assert after.as_dict() == {
        "schema_version": 2,
        "groups": 4,
        "embeddings": 2,
        "model_embeddings": 2,
        "neighbor_rows": 2,
    }


def test_query_embedding_normalizes_fake_text_vector() -> None:
    fake = FakeEmbedder({}, text_vectors={"piano": (3.0, 4.0)})

    assert query_embedding("piano", fake) == pytest.approx((0.6, 0.8))


def test_cli_status_json_does_not_load_real_model(features_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = main(["status", "--features", str(features_path), "--json"])

    assert exit_code == 0
    out = capsys.readouterr().out
    payload = json.loads(out)
    assert payload["schema_version"] == 1
    assert payload["groups"] == 4
    assert payload["embeddings"] == 0
    assert payload["neighbor_rows"] == 0
    assert payload["model"] == "laion-clap-music-audioset"
