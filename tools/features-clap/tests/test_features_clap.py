from __future__ import annotations

import hashlib
import io
import json
import sqlite3
import sys
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import Sequence

import pytest

from muzaiten_features_clap import db
from muzaiten_features_clap.cli import main
from muzaiten_features_clap import model
from muzaiten_features_clap.model import (
    _decode_audio_command,
    checkpoint_status,
    device_label,
    download_checkpoint,
    probe_device,
    resolve_device,
)
from muzaiten_features_clap.ops import neighbors, query_embedding, scan, status


@dataclass
class FakeEmbedder:
    vectors: dict[Path, tuple[float, ...]]
    text_vectors: dict[str, tuple[float, ...]] = field(default_factory=dict)
    model: str = "fake-clap"
    version: str = "fixture"
    dimension: int = 2
    calls: list[Path] = field(default_factory=list)
    batches: list[list[Path]] = field(default_factory=list)
    duration_batches: list[list[int | None]] = field(default_factory=list)

    def embed_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[Sequence[float]]:
        batch = list(paths)
        self.calls.extend(batch)
        self.batches.append(batch)
        self.duration_batches.append(
            list(durations_ms) if durations_ms is not None else [None] * len(batch)
        )
        return [self.vectors[path] for path in batch]

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        self.calls.append(path)
        return self.vectors[path]

    def embed_text(self, text: str) -> Sequence[float]:
        return self.text_vectors[text]


class FailingSecondBatchEmbedder(FakeEmbedder):
    def embed_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[Sequence[float]]:
        if self.batches:
            raise RuntimeError("fixture batch failure")
        return super().embed_audio_paths(paths, durations_ms)


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

    first = scan(features_path, fake, limit=3, batch_size=2)
    assert first.as_dict() == {"groups": 3, "embedded": 3, "skipped": 0}
    assert fake.calls == [
        Path("/music/a-copy.flac"),
        Path("/music/b.flac"),
        Path("/music/c.flac"),
    ]
    assert fake.batches == [
        [Path("/music/a-copy.flac"), Path("/music/b.flac")],
        [Path("/music/c.flac")],
    ]
    assert fake.duration_batches == [[3000, 3000], [3000]]

    with db.connect(features_path) as conn:
        assert db.read_schema_version(conn) == 5
        rows = conn.execute(
            "SELECT content_group_id, generation_id, dim, vector FROM embeddings "
            "ORDER BY content_group_id",
        ).fetchall()
        assert [int(row["content_group_id"]) for row in rows] == [10, 11, 12]
        assert len({int(row["generation_id"]) for row in rows}) == 1
        assert {int(row["dim"]) for row in rows} == {2}
        assert db.unpack_vector(rows[0]["vector"], 2) == pytest.approx((1.0, 0.0))
        assert db.unpack_vector(rows[1]["vector"], 2) == pytest.approx((0.0, 1.0))

    second = scan(features_path, fake, limit=3, batch_size=2)
    assert second.as_dict() == {"groups": 3, "embedded": 0, "skipped": 3}
    assert fake.calls == [
        Path("/music/a-copy.flac"),
        Path("/music/b.flac"),
        Path("/music/c.flac"),
    ]


def test_scan_rejects_non_positive_batch_size(features_path: Path) -> None:
    fake = FakeEmbedder({})

    with pytest.raises(ValueError, match="batch size must be at least 1"):
        scan(features_path, fake, batch_size=0)


def test_scan_commits_each_completed_batch(features_path: Path) -> None:
    fake = FailingSecondBatchEmbedder(
        {
            Path("/music/a-copy.flac"): (1.0, 0.0),
            Path("/music/b.flac"): (0.0, 1.0),
            Path("/music/c.flac"): (1.0, 1.0),
            Path("/music/d.flac"): (-1.0, 0.0),
        }
    )

    with pytest.raises(RuntimeError, match="fixture batch failure"):
        scan(features_path, fake, batch_size=2)

    with db.connect(features_path) as conn:
        group_ids = conn.execute(
            "SELECT content_group_id FROM embeddings ORDER BY content_group_id"
        ).fetchall()
    assert [int(row["content_group_id"]) for row in group_ids] == [10, 11]


def test_generation_change_activates_partial_new_corpus_and_hides_old_neighbors(
    features_path: Path,
) -> None:
    fake = FakeEmbedder(
        {
            Path("/music/a-copy.flac"): (1.0, 0.0),
            Path("/music/b.flac"): (0.0, 1.0),
            Path("/music/c.flac"): (1.0, 1.0),
            Path("/music/d.flac"): (-1.0, 0.0),
        }
    )
    scan(features_path, fake, checkpoint_sha256="sha-a", feature_revision="revision-a")
    neighbors(features_path, fake.model, fake.version, top_k=1)
    scan(
        features_path,
        fake,
        limit=1,
        checkpoint_sha256="sha-b",
        feature_revision="revision-b",
    )

    with db.connect(features_path) as conn:
        active = conn.execute(
            "SELECT id, checkpoint_sha256, feature_revision FROM semantic_generations "
            "WHERE active <> 0"
        ).fetchone()
        assert active is not None
        assert active["checkpoint_sha256"] == "sha-b"
        assert active["feature_revision"] == "revision-b"
        assert db._count(  # noqa: SLF001 - schema contract assertion
            conn, "SELECT COUNT(*) FROM embeddings WHERE generation_id = ?", (active["id"],)
        ) == 1
        assert db._count(  # noqa: SLF001 - schema contract assertion
            conn, "SELECT COUNT(*) FROM track_neighbors WHERE generation_id = ?", (active["id"],)
        ) == 0


@pytest.mark.parametrize("schema_version", [3, 4])
def test_current_indexer_schemas_are_accepted_without_downgrade(
    tmp_path: Path,
    schema_version: int,
) -> None:
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
        if schema_version == 4:
            conn.execute(
                "CREATE TABLE file_features("
                "path TEXT PRIMARY KEY, extractor TEXT NOT NULL, version TEXT NOT NULL)"
            )
        conn.execute(
            "INSERT INTO meta(key, value) VALUES('schema_version', ?)",
            (str(schema_version),),
        )

    with db.connect(path) as conn:
        db.ensure_schema(conn)
        assert db.read_schema_version(conn) == 5
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
        "active_generation_id": None,
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
        "schema_version": 5,
        "groups": 4,
        "embeddings": 2,
        "model_embeddings": 2,
        "neighbor_rows": 2,
        "active_generation_id": 1,
    }


def test_query_embedding_normalizes_fake_text_vector() -> None:
    fake = FakeEmbedder({}, text_vectors={"piano": (3.0, 4.0)})

    assert query_embedding("piano", fake) == pytest.approx((0.6, 0.8))


def test_audio_decode_is_bounded_to_a_stable_ten_second_window() -> None:
    long_path = Path("/music/long.flac")
    long_command = _decode_audio_command(long_path, duration_ms=240_000)
    repeated_command = _decode_audio_command(long_path, duration_ms=240_000)
    short_command = _decode_audio_command(Path("/music/short.flac"), duration_ms=3_000)

    assert long_command == repeated_command
    offset = float(long_command[long_command.index("-ss") + 1])
    assert 0.0 <= offset <= 230.0
    assert "-ss" not in short_command
    assert long_command[long_command.index("-t") + 1] == "10"
    assert short_command[short_command.index("-t") + 1] == "10"


def test_protocol_status_does_not_load_real_model(
    features_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        sys,
        "stdin",
        io.StringIO(
            json.dumps(
                {
                    "protocol_version": 1,
                    "request_id": "status-1",
                    "operation": "status",
                    "parameters": {"features": str(features_path)},
                }
            )
        ),
    )
    exit_code = main()

    assert exit_code == 0
    out = capsys.readouterr().out
    payload = json.loads(out)["result"]
    assert payload["store"]["schema_version"] == 1
    assert payload["store"]["groups"] == 4
    assert payload["store"]["embeddings"] == 0
    assert payload["store"]["neighbor_rows"] == 0
    assert payload["model"]["name"] == "laion-clap-music-audioset"
    assert payload["feature_revision"] == "clap-htsat-base-audio-window-v1"


def _fake_torch(cuda_available: bool, name: str = "NVIDIA GeForce RTX 3080") -> SimpleNamespace:
    return SimpleNamespace(
        cuda=SimpleNamespace(
            is_available=lambda: cuda_available,
            get_device_name=lambda index: name,
        ),
    )


def test_resolve_device_auto_prefers_cuda(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=True))

    assert resolve_device("auto") == "cuda"


def test_resolve_device_auto_falls_back_to_cpu(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=False))

    assert resolve_device("auto") == "cpu"


def test_resolve_device_explicit_cuda_errors_instead_of_silent_fallback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=False))

    with pytest.raises(RuntimeError, match="no usable CUDA device"):
        resolve_device("cuda")


def test_resolve_device_explicit_cpu_ignores_cuda(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=True))

    assert resolve_device("cpu") == "cpu"


def test_device_label_names_the_gpu(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=True))

    assert device_label("cuda") == "cuda (NVIDIA GeForce RTX 3080)"
    assert device_label("cpu") == "cpu"


def test_probe_device_is_none_without_torch(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(sys.modules, "torch", None)

    assert probe_device() is None


@pytest.mark.parametrize(
    "parameters,message",
    [
        ({"features": "x.sqlite", "device": 7}, "device must be a string"),
        ({"features": "x.sqlite", "batch_size": 0}, "batch_size must be a positive integer"),
    ],
)
def test_protocol_rejects_invalid_scan_parameters(
    parameters: dict[str, object],
    message: str,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        sys,
        "stdin",
        io.StringIO(
            json.dumps(
                {
                    "protocol_version": 1,
                    "request_id": "invalid-scan",
                    "operation": "scan",
                    "parameters": parameters,
                }
            )
        ),
    )
    exit_code = main()

    assert exit_code == 2
    payload = json.loads(capsys.readouterr().out)
    assert message in payload["message"]


class FakeDownloadResponse:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0
        self.headers = {"Content-Length": str(len(payload))}

    def __enter__(self) -> FakeDownloadResponse:
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def read(self, size: int) -> bytes:
        chunk = self.payload[self.offset : self.offset + size]
        self.offset += len(chunk)
        return chunk


def test_model_download_reports_bytes_verifies_and_installs_atomically(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    payload = b"fixture checkpoint bytes"
    monkeypatch.setattr(model, "model_cache_dir", lambda: tmp_path)
    monkeypatch.setattr(model, "MODEL_SHA256", hashlib.sha256(payload).hexdigest())
    progress: list[tuple[int, int | None]] = []

    result = download_checkpoint(
        progress=lambda completed, total: progress.append((completed, total)),
        opener=lambda *_args, **_kwargs: FakeDownloadResponse(payload),
    )

    assert result.downloaded is True
    assert result.path.read_bytes() == payload
    assert progress == [(0, len(payload)), (len(payload), len(payload))]
    assert not [path for path in tmp_path.iterdir() if path != result.path]


def test_model_download_checksum_failure_keeps_existing_state_clean(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(model, "model_cache_dir", lambda: tmp_path)
    monkeypatch.setattr(model, "MODEL_SHA256", "0" * 64)

    with pytest.raises(RuntimeError, match="SHA-256"):
        download_checkpoint(opener=lambda *_args, **_kwargs: FakeDownloadResponse(b"bad"))

    assert list(tmp_path.iterdir()) == []
    assert checkpoint_status().present is False


def test_model_download_cancellation_removes_temporary_file(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(model, "model_cache_dir", lambda: tmp_path)

    with pytest.raises(InterruptedError, match="canceled"):
        download_checkpoint(
            canceled=lambda: True,
            opener=lambda *_args, **_kwargs: FakeDownloadResponse(b"unused"),
        )

    assert list(tmp_path.iterdir()) == []


def test_protocol_status_reports_device_probe(
    features_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setitem(sys.modules, "torch", _fake_torch(cuda_available=True))

    monkeypatch.setattr(
        sys,
        "stdin",
        io.StringIO(
            json.dumps(
                {
                    "protocol_version": 1,
                    "request_id": "status-device",
                    "operation": "status",
                }
            )
        ),
    )
    exit_code = main()

    assert exit_code == 0
    payload = json.loads(capsys.readouterr().out)["result"]
    assert payload["device"] == "cuda"


def _naive_neighbor_reference(
    group_ids: list[int],
    vectors: list[tuple[float, ...]],
    top_k: int,
) -> list[tuple[int, int, int]]:
    import numpy as np

    matrix = np.array(vectors, dtype=np.float32)
    similarities = matrix @ matrix.T
    expected: list[tuple[int, int, int]] = []
    for source_index, source_id in enumerate(group_ids):
        ranked = [
            (neighbor_index, float(similarities[source_index, neighbor_index]))
            for neighbor_index in range(len(group_ids))
            if neighbor_index != source_index
        ]
        ranked.sort(key=lambda item: (-item[1], group_ids[item[0]]))
        for rank, (neighbor_index, _cosine) in enumerate(ranked[:top_k], start=1):
            expected.append((source_id, group_ids[neighbor_index], rank))
    return expected


def test_blockwise_neighbors_match_naive_reference(tmp_path: Path) -> None:
    import random

    rng = random.Random(20260710)
    dim = 8
    count = 137
    group_ids = sorted(rng.sample(range(1, 10_000), count))
    vectors: list[tuple[float, ...]] = []
    for index in range(count):
        if index % 7 == 3:
            # Duplicate an earlier vector so cosine ties exercise the
            # ascending-group-id tie-break at the top-k boundary.
            vectors.append(vectors[index - 1])
            continue
        raw = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
        vectors.append(db.normalize_vector(raw))

    path = tmp_path / "features.sqlite"
    with sqlite3.connect(path) as conn:
        conn.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)")
        conn.execute("INSERT INTO meta(key, value) VALUES('schema_version', '2')")
    with db.connect(path) as conn:
        db.ensure_schema(conn)
        generation_id = db.ensure_generation(
            conn, "clap", "fake-clap", "fixture-sha", "fixture-revision", dim
        )
        for group_id, vector in zip(group_ids, vectors):
            db.upsert_embedding(conn, group_id, generation_id, vector)
        conn.commit()

        stored = [
            db.unpack_vector(row["vector"], dim)
            for row in conn.execute(
                "SELECT vector FROM embeddings ORDER BY content_group_id"
            ).fetchall()
        ]
        expected = _naive_neighbor_reference(group_ids, stored, top_k=10)

        inserted = db.rebuild_neighbors(conn, generation_id, top_k=10, block_size=17)

        actual = [
            (int(row["content_group_id"]), int(row["neighbor_group_id"]), int(row["rank"]))
            for row in conn.execute(
                "SELECT content_group_id, neighbor_group_id, rank FROM track_neighbors "
                "ORDER BY content_group_id, rank",
            ).fetchall()
        ]

    assert inserted == count * 10
    assert actual == expected
