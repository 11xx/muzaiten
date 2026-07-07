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


def scan(features_path: Path, embedder: Embedder, limit: int | None = None) -> ScanResult:
    with db.connect(features_path) as conn:
        db.ensure_schema(conn)
        representatives = db.representative_groups(conn, limit=limit)
        existing = db.existing_embedding_groups(conn, embedder.model, embedder.version)
        embedded = 0
        skipped = 0
        for representative in representatives:
            if representative.content_group_id in existing:
                skipped += 1
                continue
            vector = embedder.embed_audio_path(representative.path)
            db.upsert_embedding(
                conn,
                representative.content_group_id,
                embedder.model,
                embedder.version,
                vector,
            )
            embedded += 1
        conn.commit()
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
