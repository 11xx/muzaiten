from __future__ import annotations

import math
import sqlite3
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np

SCHEMA_VERSION = 2


@dataclass(frozen=True)
class Representative:
    content_group_id: int
    path: Path


@dataclass(frozen=True)
class EmbeddingRow:
    content_group_id: int
    vector: tuple[float, ...]


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
    if version > SCHEMA_VERSION:
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
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('schema_version', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        (str(SCHEMA_VERSION),),
    )
    conn.commit()


def representative_groups(conn: sqlite3.Connection, limit: int | None = None) -> list[Representative]:
    sql = (
        "SELECT content_group_id, MIN(path) AS path FROM files "
        "WHERE content_group_id IS NOT NULL AND status = 'ok' "
        "GROUP BY content_group_id ORDER BY content_group_id"
    )
    params: tuple[int, ...] = ()
    if limit is not None:
        sql += " LIMIT ?"
        params = (limit,)
    rows = conn.execute(sql, params).fetchall()
    return [Representative(int(row["content_group_id"]), Path(row["path"])) for row in rows]


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


def load_embeddings(
    conn: sqlite3.Connection,
    model: str,
    version: str,
) -> list[EmbeddingRow]:
    rows = conn.execute(
        "SELECT content_group_id, dim, vector FROM embeddings "
        "WHERE model = ? AND version = ? ORDER BY content_group_id",
        (model, version),
    ).fetchall()
    return [
        EmbeddingRow(int(row["content_group_id"]), unpack_vector(row["vector"], int(row["dim"])))
        for row in rows
    ]


def rebuild_neighbors(
    conn: sqlite3.Connection,
    model: str,
    version: str,
    top_k: int = 100,
) -> int:
    rows = load_embeddings(conn, model, version)
    conn.execute("DELETE FROM track_neighbors")
    if len(rows) < 2:
        conn.commit()
        return 0

    matrix = np.array([row.vector for row in rows], dtype=np.float32)
    similarities = matrix @ matrix.T
    inserted = 0
    for source_index, source in enumerate(rows):
        ranked: list[tuple[int, float]] = []
        for neighbor_index, neighbor in enumerate(rows):
            if neighbor_index == source_index:
                continue
            ranked.append((neighbor_index, float(similarities[source_index, neighbor_index])))
        ranked.sort(key=lambda item: (-item[1], rows[item[0]].content_group_id))
        for rank, (neighbor_index, cosine) in enumerate(ranked[:top_k], start=1):
            conn.execute(
                "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine) "
                "VALUES(?, ?, ?, ?)",
                (source.content_group_id, rows[neighbor_index].content_group_id, rank, cosine),
            )
            inserted += 1
    conn.commit()
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
