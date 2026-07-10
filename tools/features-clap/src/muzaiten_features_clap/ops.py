from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from . import db
from .embedder import Embedder


@dataclass(frozen=True)
class ScanResult:
    groups: int
    embedded: int
    skipped: int

    def as_dict(self) -> dict[str, int]:
        return {
            "groups": self.groups,
            "embedded": self.embedded,
            "skipped": self.skipped,
        }


def scan(
    features_path: Path,
    embedder: Embedder,
    limit: int | None = None,
    batch_size: int = 8,
) -> ScanResult:
    if batch_size < 1:
        raise ValueError("batch size must be at least 1")
    with db.connect(features_path) as conn:
        db.ensure_schema(conn)
        representatives = db.representative_groups(conn, limit=limit)
        existing = db.existing_embedding_groups(conn, embedder.model, embedder.version)
        embedded = 0
        pending = [
            representative
            for representative in representatives
            if representative.content_group_id not in existing
        ]
        for start in range(0, len(pending), batch_size):
            batch = pending[start : start + batch_size]
            vectors = embedder.embed_audio_paths(
                [item.path for item in batch],
                [item.duration_ms for item in batch],
            )
            if len(vectors) != len(batch):
                raise RuntimeError(
                    f"embedder returned {len(vectors)} vectors for {len(batch)} audio paths"
                )
            for representative, vector in zip(batch, vectors, strict=True):
                db.upsert_embedding(
                    conn,
                    representative.content_group_id,
                    embedder.model,
                    embedder.version,
                    vector,
                )
                embedded += 1
            # Each batch is a durable resume point: a later decode/model failure
            # leaves completed work available for the next scan to skip.
            conn.commit()
        skipped = len(representatives) - len(pending)
        return ScanResult(len(representatives), embedded, skipped)


def neighbors(
    features_path: Path,
    model: str,
    version: str,
    top_k: int = 100,
) -> int:
    with db.connect(features_path) as conn:
        db.ensure_schema(conn)
        return db.rebuild_neighbors(conn, model, version, top_k=top_k)


def status(features_path: Path, model: str, version: str) -> db.Status:
    with db.connect(features_path) as conn:
        return db.status(conn, model, version)


def query_embedding(text: str, embedder: Embedder) -> tuple[float, ...]:
    return db.normalize_vector(embedder.embed_text(text))
