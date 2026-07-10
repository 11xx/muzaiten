from __future__ import annotations

import math
import sqlite3
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence

import numpy as np

SCHEMA_VERSION = 2
# Schema v4 only adds indexer-private per-file scalar rows. Embedding and
# neighbor tables retain their v2 shape, so the standalone tool can operate
# on v4 stores without migrating or downgrading them.
MAX_SCHEMA_VERSION = 4


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

    def as_dict(self) -> dict[str, int]:
        return {
            "schema_version": self.schema_version,
            "groups": self.groups,
            "embeddings": self.embeddings,
            "model_embeddings": self.model_embeddings,
            "neighbor_rows": self.neighbor_rows,
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

    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS embeddings(
            content_group_id INTEGER PRIMARY KEY,
            model TEXT NOT NULL,
            version TEXT NOT NULL,
            dim INTEGER NOT NULL,
            vector BLOB NOT NULL
        );
        CREATE TABLE IF NOT EXISTS track_neighbors(
            content_group_id INTEGER NOT NULL,
            neighbor_group_id INTEGER NOT NULL,
            rank INTEGER NOT NULL,
            cosine REAL NOT NULL,
            PRIMARY KEY(content_group_id, rank)
        );
        """
    )
    if version < SCHEMA_VERSION:
        conn.execute(
            "INSERT INTO meta(key, value) VALUES('schema_version', ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (str(SCHEMA_VERSION),),
        )
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
    model: str,
    version: str,
) -> set[int]:
    try:
        rows = conn.execute(
            "SELECT content_group_id FROM embeddings WHERE model = ? AND version = ?",
            (model, version),
        ).fetchall()
    except sqlite3.Error:
        return set()
    return {int(row["content_group_id"]) for row in rows}


def upsert_embedding(
    conn: sqlite3.Connection,
    content_group_id: int,
    model: str,
    version: str,
    vector: Sequence[float],
) -> None:
    normalized = normalize_vector(vector)
    conn.execute(
        "INSERT INTO embeddings(content_group_id, model, version, dim, vector) "
        "VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(content_group_id) DO UPDATE SET "
        "model = excluded.model, version = excluded.version, "
        "dim = excluded.dim, vector = excluded.vector",
        (content_group_id, model, version, len(normalized), pack_vector(normalized)),
    )


def _load_embedding_matrix(
    conn: sqlite3.Connection,
    model: str,
    version: str,
) -> tuple[np.ndarray, np.ndarray]:
    rows = conn.execute(
        "SELECT content_group_id, dim, vector FROM embeddings "
        "WHERE model = ? AND version = ? ORDER BY content_group_id",
        (model, version),
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
    model: str,
    version: str,
    top_k: int = 100,
    block_size: int = 1024,
    progress: Callable[[int, int], None] | None = None,
    canceled: Callable[[], bool] | None = None,
) -> int:
    group_ids, matrix = _load_embedding_matrix(conn, model, version)
    conn.execute("DELETE FROM track_neighbors")
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
                "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine) "
                "VALUES(?, ?, ?, ?)",
                [
                    (source_id, int(group_ids[neighbor]), rank, float(row[neighbor]))
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
    model_embeddings = _count(
        conn,
        "SELECT COUNT(*) FROM embeddings WHERE model = ? AND version = ?",
        (model, version),
    )
    neighbor_rows = _count(conn, "SELECT COUNT(*) FROM track_neighbors")
    return Status(schema_version, groups, embeddings, model_embeddings, neighbor_rows)


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
