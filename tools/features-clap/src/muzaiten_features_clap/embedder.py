from __future__ import annotations

from pathlib import Path
from typing import Protocol, Sequence


class Embedder(Protocol):
    model: str
    version: str

    def embed_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[Sequence[float]]:
        """Return one embedding per audio file, preserving input order."""

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        """Return one embedding for an audio file."""

    def embed_text(self, text: str) -> Sequence[float]:
        """Return one embedding for a text query."""


class SplitAudioEmbedder(Protocol):
    """Optional scan API that permits decode/inference lookahead."""

    def decode_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[object]:
        """Decode audio files while preserving input order."""

    def embed_audio_data(self, waveforms: Sequence[object]) -> Sequence[Sequence[float]]:
        """Return one embedding per decoded waveform, preserving input order."""
