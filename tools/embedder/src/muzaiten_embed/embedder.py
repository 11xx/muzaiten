from __future__ import annotations

from pathlib import Path
from typing import Protocol, Sequence


class Embedder(Protocol):
    model: str
    version: str

    def embed_audio_paths(self, paths: Sequence[Path]) -> Sequence[Sequence[float]]:
        """Return one embedding per audio file, preserving input order."""

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        """Return one embedding for an audio file."""

    def embed_text(self, text: str) -> Sequence[float]:
        """Return one embedding for a text query."""
