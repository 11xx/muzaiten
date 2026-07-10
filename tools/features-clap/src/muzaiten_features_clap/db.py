from __future__ import annotations

import math
import sqlite3
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence

import numpy as np

SCHEMA_VERSION = 5
MAX_SCHEMA_VERSION = 5
NEIGHBOR_ALGORITHM_REVISION = "cosine-blockwise-v1"


@dataclass(frozen=True)
class Representative:
    content_group_id: int
    path: Path
    duration_ms: int | None


@dataclass(frozen=True)
class Status:
    schema_version: int
    groups: int
    embeddings: int
    model_embeddings: int
    neighbor_rows: int
    active_generation_id: int | None = None

    def as_dict(self) -> dict[str, int | None]:
        return {
            "schema_version": self.schema_version,
            "groups": self.groups,
            "embeddings": self.embeddings,
            "model_embeddings": self.model_embeddings,
            "neighbor_rows": self.neighbor_rows,
            "active_generation_id": self.active_generation_id,
        }


def connect(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    return conn


def read_schema_version(conn: sqlite3.Connection) -> int:
    try:
        row = conn.execute("SELECT value FROM meta WHERE key = 'schema_version'").fetchone()
    except sqlite3.Error:
        return -1
    if row is None:
        return -1
    try:
        return int(row["value"])
    except (TypeError, ValueError):
        return -1


def ensure_schema(conn: sqlite3.Connection) -> None:
    version = read_schema_version(conn)
    if version < 1:
        raise ValueError("features.sqlite is missing schema_version")
    if version > MAX_SCHEMA_VERSION:
        raise ValueError(f"unsupported features.sqlite schema_version {version}")

    embedding_columns = _columns(conn, "embeddings")
    neighbor_columns = _columns(conn, "track_neighbors")
    if embedding_columns and "generation_id" not in embedding_columns:
        conn.execute("ALTER TABLE embeddings RENAME TO embeddings_v4")
    if neighbor_columns and "generation_id" not in neighbor_columns:
        conn.execute("ALTER TABLE track_neighbors RENAME TO track_neighbors_v4")
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS semantic_generations(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            capability TEXT NOT NULL,
            model TEXT NOT NULL,
            checkpoint_sha256 TEXT NOT NULL,
            feature_revision TEXT NOT NULL,
            vector_dim INTEGER NOT NULL,
            provider_path TEXT,
            provider_version TEXT,
            created_at INTEGER NOT NULL,
            completed_at INTEGER,
            active INTEGER NOT NULL DEFAULT 0,
            UNIQUE(capability, model, checkpoint_sha256, feature_revision, vector_dim)
        );
        CREATE TABLE IF NOT EXISTS embeddings(
            content_group_id INTEGER NOT NULL,
            generation_id INTEGER NOT NULL,
            dim INTEGER NOT NULL,
            vector BLOB NOT NULL,
            PRIMARY KEY(content_group_id, generation_id)
        );
        CREATE TABLE IF NOT EXISTS track_neighbors(
            content_group_id INTEGER NOT NULL,
            neighbor_group_id INTEGER NOT NULL,
            rank INTEGER NOT NULL,
            cosine REAL NOT NULL,
            generation_id INTEGER NOT NULL,
            algorithm_revision TEXT NOT NULL,
            top_k INTEGER NOT NULL,
            PRIMARY KEY(content_group_id, generation_id, rank)
        );
        """
    )
    if _table_exists(conn, "embeddings_v4"):
        _migrate_legacy_semantics(conn)
    if version < SCHEMA_VERSION:
        conn.execute(
            "INSERT INTO meta(key, value) VALUES('schema_version', ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (str(SCHEMA_VERSION),),
        )
    conn.commit()


def _columns(conn: sqlite3.Connection, table: str) -> set[str]:
    try:
        return {str(row[1]) for row in conn.execute(f"PRAGMA table_info({table})")}
    except sqlite3.Error:
        return set()


def _table_exists(conn: sqlite3.Connection, table: str) -> bool:
    row = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", (table,)
    ).fetchone()
    return row is not None


def _migrate_legacy_semantics(conn: sqlite3.Connection) -> None:
    from .model import FEATURE_REVISION, MODEL_NAME, MODEL_SHA256, MODEL_VERSION

    rows = conn.execute(
        "SELECT DISTINCT model, version, dim FROM embeddings_v4 ORDER BY model, version, dim"
    ).fetchall()
    generation_id: int | None = None
    known = (
        len(rows) == 1
        and rows[0]["model"] == MODEL_NAME
        and rows[0]["version"] == MODEL_VERSION
        and int(rows[0]["dim"]) == 512
    )
    if rows:
        dim = int(rows[0]["dim"])
        cursor = conn.execute(
            "INSERT INTO semantic_generations(capability, model, checkpoint_sha256, feature_revision, "
            "vector_dim, provider_path, provider_version, created_at, completed_at, active) "
            "VALUES('clap', ?, ?, ?, ?, 'legacy-v4-migration', NULL, unixepoch(), "
            "CASE WHEN ? THEN unixepoch() END, ?)",
            (
                MODEL_NAME if known else "legacy-unknown",
                MODEL_SHA256 if known else "unknown",
                FEATURE_REVISION if known else "unknown",
                dim,
                known,
                int(known),
            ),
        )
        generation_id = int(cursor.lastrowid)
        conn.execute(
            "INSERT INTO embeddings(content_group_id, generation_id, dim, vector) "
            "SELECT content_group_id, ?, dim, vector FROM embeddings_v4",
            (generation_id,),
        )
    if known and generation_id is not None and _table_exists(conn, "track_neighbors_v4"):
        top_k = _count(conn, "SELECT COALESCE(MAX(rank), 0) FROM track_neighbors_v4")
        conn.execute(
            "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine, "
            "generation_id, algorithm_revision, top_k) "
            "SELECT content_group_id, neighbor_group_id, rank, cosine, ?, ?, ? "
            "FROM track_neighbors_v4",
            (generation_id, NEIGHBOR_ALGORITHM_REVISION, top_k),
        )
    conn.execute("DROP TABLE embeddings_v4")
    if _table_exists(conn, "track_neighbors_v4"):
        conn.execute("DROP TABLE track_neighbors_v4")


def ensure_generation(
    conn: sqlite3.Connection,
    capability: str,
    model: str,
    checkpoint_sha256: str,
    feature_revision: str,
    vector_dim: int,
    provider_path: str | None = None,
    provider_version: str | None = None,
) -> int:
    row = conn.execute(
        "SELECT id FROM semantic_generations WHERE capability = ? AND model = ? "
        "AND checkpoint_sha256 = ? AND feature_revision = ? AND vector_dim = ?",
        (capability, model, checkpoint_sha256, feature_revision, vector_dim),
    ).fetchone()
    if row is None:
        conn.execute("UPDATE semantic_generations SET active = 0 WHERE active <> 0")
        cursor = conn.execute(
            "INSERT INTO semantic_generations(capability, model, checkpoint_sha256, feature_revision, "
            "vector_dim, provider_path, provider_version, created_at, active) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, unixepoch(), 1)",
            (
                capability,
                model,
                checkpoint_sha256,
                feature_revision,
                vector_dim,
                provider_path,
                provider_version,
            ),
        )
        conn.execute("DELETE FROM track_neighbors")
        conn.commit()
        return int(cursor.lastrowid)
    generation_id = int(row["id"])
    conn.execute("UPDATE semantic_generations SET active = (id = ?)", (generation_id,))
    conn.execute(
        "UPDATE semantic_generations SET provider_path = COALESCE(?, provider_path), "
        "provider_version = COALESCE(?, provider_version) WHERE id = ?",
        (provider_path, provider_version, generation_id),
    )
    conn.commit()
    return generation_id


def complete_generation(conn: sqlite3.Connection, generation_id: int) -> None:
    conn.execute(
        "UPDATE semantic_generations SET completed_at = unixepoch() WHERE id = ?",
        (generation_id,),
    )
    conn.execute("DELETE FROM embeddings WHERE generation_id <> ?", (generation_id,))
    conn.execute("DELETE FROM track_neighbors WHERE generation_id <> ?", (generation_id,))
    conn.execute("DELETE FROM semantic_generations WHERE id <> ?", (generation_id,))
    conn.commit()


def representative_groups(conn: sqlite3.Connection, limit: int | None = None) -> list[Representative]:
    sql = (
        "WITH representatives AS ("
        "SELECT content_group_id, MIN(path) AS path FROM files "
        "WHERE content_group_id IS NOT NULL AND status = 'ok' "
        "GROUP BY content_group_id) "
        "SELECT r.content_group_id, r.path, f.duration_ms "
        "FROM representatives r JOIN files f ON f.path = r.path "
        "ORDER BY r.content_group_id"
    )
    params: tuple[int, ...] = ()
    if limit is not None:
        sql += " LIMIT ?"
        params = (limit,)
    rows = conn.execute(sql, params).fetchall()
    return [
        Representative(
            int(row["content_group_id"]),
            Path(row["path"]),
            int(row["duration_ms"]) if row["duration_ms"] is not None else None,
        )
        for row in rows
    ]


def normalize_vector(values: Sequence[float]) -> tuple[float, ...]:
    vector = tuple(float(value) for value in values)
    norm = math.sqrt(sum(value * value for value in vector))
    if norm == 0.0 or not math.isfinite(norm):
        raise ValueError("embedding vector must have a finite non-zero norm")
    return tuple(value / norm for value in vector)


def pack_vector(values: Sequence[float]) -> bytes:
    packed = array("f", normalize_vector(values))
    if sys.byteorder != "little":
        packed.byteswap()
    return packed.tobytes()


def unpack_vector(blob: bytes, dim: int) -> tuple[float, ...]:
    values = array("f")
    values.frombytes(blob)
    if sys.byteorder != "little":
        values.byteswap()
    if len(values) != dim:
        raise ValueError(f"embedding blob has {len(values)} floats, expected {dim}")
    return tuple(float(value) for value in values)


def existing_embedding_groups(
    conn: sqlite3.Connection,
    generation_id: int,
) -> set[int]:
    try:
        rows = conn.execute(
            "SELECT content_group_id FROM embeddings WHERE generation_id = ?",
            (generation_id,),
        ).fetchall()
    except sqlite3.Error:
        return set()
    return {int(row["content_group_id"]) for row in rows}


def upsert_embedding(
    conn: sqlite3.Connection,
    content_group_id: int,
    generation_id: int,
    vector: Sequence[float],
) -> None:
    normalized = normalize_vector(vector)
    conn.execute(
        "INSERT INTO embeddings(content_group_id, generation_id, dim, vector) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(content_group_id, generation_id) DO UPDATE SET "
        "dim = excluded.dim, vector = excluded.vector",
        (content_group_id, generation_id, len(normalized), pack_vector(normalized)),
    )


def _load_embedding_matrix(
    conn: sqlite3.Connection,
    generation_id: int,
) -> tuple[np.ndarray, np.ndarray]:
    rows = conn.execute(
        "SELECT content_group_id, dim, vector FROM embeddings "
        "WHERE generation_id = ? ORDER BY content_group_id",
        (generation_id,),
    ).fetchall()
    if not rows:
        return np.empty(0, dtype=np.int64), np.empty((0, 0), dtype=np.float32)
    dim = int(rows[0]["dim"])
    group_ids = np.empty(len(rows), dtype=np.int64)
    matrix = np.empty((len(rows), dim), dtype=np.float32)
    for index, row in enumerate(rows):
        if int(row["dim"]) != dim:
            raise ValueError(
                f"embedding for group {int(row['content_group_id'])} has dim "
                f"{int(row['dim'])}, expected {dim}"
            )
        vector = np.frombuffer(row["vector"], dtype="<f4")
        if vector.size != dim:
            raise ValueError(f"embedding blob has {vector.size} floats, expected {dim}")
        group_ids[index] = int(row["content_group_id"])
        matrix[index] = vector
    return group_ids, matrix


def rebuild_neighbors(
    conn: sqlite3.Connection,
    generation_id: int,
    top_k: int = 100,
    block_size: int = 1024,
    progress: Callable[[int, int], None] | None = None,
    canceled: Callable[[], bool] | None = None,
) -> int:
    group_ids, matrix = _load_embedding_matrix(conn, generation_id)
    conn.execute("DELETE FROM track_neighbors WHERE generation_id = ?", (generation_id,))
    total = int(group_ids.size)
    if progress is not None:
        progress(0, total)
    if total < 2:
        conn.commit()
        return 0

    # Blockwise so memory stays O(block_size * total) instead of the full
    # total^2 similarity matrix (24 GB at 77k groups).
    keep = min(top_k, total - 1)
    inserted = 0
    for start in range(0, total, block_size):
        if canceled is not None and canceled():
            conn.commit()
            raise InterruptedError("neighbor rebuild canceled")
        stop = min(start + block_size, total)
        similarities = matrix[start:stop] @ matrix.T
        for local, source_index in enumerate(range(start, stop)):
            row = similarities[local]
            row[source_index] = -np.inf
            # Exact (-cosine, neighbor_group_id) top-k: partition first, then
            # widen to every candidate tied with the selection floor so
            # boundary ties resolve by ascending group id, not partition luck.
            partitioned = np.argpartition(row, -keep)[-keep:]
            floor = row[partitioned].min()
            candidates = np.flatnonzero(row >= floor)
            order = np.lexsort((group_ids[candidates], -row[candidates]))
            chosen = candidates[order][:keep]
            source_id = int(group_ids[source_index])
            conn.executemany(
                "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine, "
                "generation_id, algorithm_revision, top_k) VALUES(?, ?, ?, ?, ?, ?, ?)",
                [
                    (
                        source_id,
                        int(group_ids[neighbor]),
                        rank,
                        float(row[neighbor]),
                        generation_id,
                        NEIGHBOR_ALGORITHM_REVISION,
                        top_k,
                    )
                    for rank, neighbor in enumerate(chosen, start=1)
                ],
            )
            inserted += int(chosen.size)
        conn.commit()
        if progress is not None:
            progress(stop, total)
    return inserted


def status(conn: sqlite3.Connection, model: str, version: str) -> Status:
    schema_version = read_schema_version(conn)
    groups = _count(
        conn,
        "SELECT COUNT(DISTINCT content_group_id) FROM files "
        "WHERE content_group_id IS NOT NULL AND status = 'ok'",
    )
    embeddings = _count(conn, "SELECT COUNT(*) FROM embeddings")
    active = conn.execute(
        "SELECT id FROM semantic_generations WHERE active <> 0 ORDER BY id DESC LIMIT 1"
    ).fetchone() if _table_exists(conn, "semantic_generations") else None
    active_id = int(active["id"]) if active is not None else None
    model_embeddings = _count(
        conn,
        "SELECT COUNT(*) FROM embeddings WHERE generation_id = ?",
        (active_id,),
    ) if active_id is not None else 0
    neighbor_rows = _count(
        conn, "SELECT COUNT(*) FROM track_neighbors WHERE generation_id = ?", (active_id,)
    ) if active_id is not None else 0
    return Status(schema_version, groups, embeddings, model_embeddings, neighbor_rows, active_id)


def _count(
    conn: sqlite3.Connection,
    sql: str,
    params: Iterable[object] = (),
) -> int:
    try:
        row = conn.execute(sql, tuple(params)).fetchone()
    except sqlite3.Error:
        return 0
    return int(row[0]) if row is not None else 0
