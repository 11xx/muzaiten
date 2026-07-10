from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

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
    progress: Callable[[int, int], None] | None = None,
    canceled: Callable[[], bool] | None = None,
    capability: str = "clap",
    checkpoint_sha256: str = "unknown",
    feature_revision: str = "unknown",
    vector_dim: int | None = None,
    provider_path: str | None = None,
    provider_version: str | None = None,
) -> ScanResult:
    if batch_size < 1:
        raise ValueError("batch size must be at least 1")
    with db.connect(features_path) as conn:
        db.ensure_schema(conn)
        dimension = vector_dim if vector_dim is not None else int(getattr(embedder, "dimension", 512))
        generation_id = db.ensure_generation(
            conn,
            capability,
            embedder.model,
            checkpoint_sha256,
            feature_revision,
            dimension,
            provider_path,
            provider_version,
        )
        representatives = db.representative_groups(conn, limit=limit)
        existing = db.existing_embedding_groups(conn, generation_id)
        embedded = 0
        pending = [
            representative
            for representative in representatives
            if representative.content_group_id not in existing
        ]
        if progress is not None:
            progress(0, len(pending))
        for start in range(0, len(pending), batch_size):
            if canceled is not None and canceled():
                raise InterruptedError("semantic scan canceled")
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
                    generation_id,
                    vector,
                )
                embedded += 1
            # Each batch is a durable resume point: a later decode/model failure
            # leaves completed work available for the next scan to skip.
            conn.commit()
            if progress is not None:
                progress(embedded, len(pending))
        skipped = len(representatives) - len(pending)
        if limit is None and embedded + skipped == len(representatives):
            db.complete_generation(conn, generation_id)
        return ScanResult(len(representatives), embedded, skipped)


def neighbors(
    features_path: Path,
    model: str,
    version: str,
    top_k: int = 100,
    progress: Callable[[int, int], None] | None = None,
    canceled: Callable[[], bool] | None = None,
) -> int:
    with db.connect(features_path) as conn:
        db.ensure_schema(conn)
        row = conn.execute(
            "SELECT id FROM semantic_generations WHERE active <> 0 ORDER BY id DESC LIMIT 1"
        ).fetchone()
        if row is None:
            raise ValueError("features.sqlite has no active semantic generation")
        return db.rebuild_neighbors(
            conn,
            int(row["id"]),
            top_k=top_k,
            progress=progress,
            canceled=canceled,
        )


def status(features_path: Path, model: str, version: str) -> db.Status:
    with db.connect(features_path) as conn:
        return db.status(conn, model, version)


def query_embedding(text: str, embedder: Embedder) -> tuple[float, ...]:
    return db.normalize_vector(embedder.embed_text(text))
