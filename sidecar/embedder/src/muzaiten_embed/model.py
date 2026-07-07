from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .db import normalize_vector

MODEL_NAME = "laion-clap-music-audioset"
MODEL_VERSION = "music_audioset_epoch_15_esc_90.14.pt"
MODEL_URL = (
    "https://huggingface.co/lukewys/laion_clap/resolve/main/"
    "music_audioset_epoch_15_esc_90.14.pt"
)
MODEL_SHA256 = "fae3e9c087f2909c28a09dc31c8dfcdacbc42ba44c70e972b58c1bd1caf6dedd"
MODEL_AMODEL = "HTSAT-base"
MODEL_SAMPLE_RATE = 48_000


@dataclass(frozen=True)
class ModelDownload:
    path: Path
    downloaded: bool


def model_cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME")
    root = Path(base) if base else Path.home() / ".cache"
    return root / "muzaiten" / "models"


def ensure_checkpoint() -> ModelDownload:
    cache_dir = model_cache_dir()
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_dir / MODEL_VERSION
    if path.exists():
        _verify_sha256(path)
        return ModelDownload(path, downloaded=False)

    with tempfile.NamedTemporaryFile(dir=cache_dir, delete=False) as temp:
        temp_path = Path(temp.name)
        digest = hashlib.sha256()
        with urllib.request.urlopen(MODEL_URL, timeout=60) as response:  # noqa: S310
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                temp.write(chunk)
    if digest.hexdigest() != MODEL_SHA256:
        temp_path.unlink(missing_ok=True)
        raise RuntimeError("downloaded CLAP checkpoint failed SHA-256 verification")
    temp_path.replace(path)
    return ModelDownload(path, downloaded=True)


def _verify_sha256(path: Path) -> None:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != MODEL_SHA256:
        raise RuntimeError(f"cached CLAP checkpoint has wrong SHA-256: {path}")


def decode_audio_ffmpeg(path: Path):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - exercised only without dependencies
        raise RuntimeError("numpy is required for real audio embedding") from exc

    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(path),
        "-ac",
        "1",
        "-ar",
        str(MODEL_SAMPLE_RATE),
        "-f",
        "f32le",
        "-",
    ]
    completed = subprocess.run(command, check=True, capture_output=True)
    audio = np.frombuffer(completed.stdout, dtype="<f4").astype("float32", copy=True)
    if audio.size == 0:
        raise RuntimeError(f"ffmpeg decoded no audio from {path}")
    return audio.reshape(1, -1)


class RealClapEmbedder:
    model = MODEL_NAME
    version = MODEL_VERSION

    def __init__(self, checkpoint: Path | None = None) -> None:
        try:
            import laion_clap
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise RuntimeError(
                "real CLAP embedding requires optional dependencies; run "
                "`uv sync --extra model` in sidecar/embedder"
            ) from exc

        checkpoint_path = checkpoint if checkpoint is not None else ensure_checkpoint().path
        self._model = laion_clap.CLAP_Module(enable_fusion=False, amodel=MODEL_AMODEL)
        self._model.load_ckpt(str(checkpoint_path), verbose=False)

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        embedding = self._model.get_audio_embedding_from_data(
            x=decode_audio_ffmpeg(path),
            use_tensor=False,
        )
        return normalize_vector(_first_row(embedding))

    def embed_text(self, text: str) -> Sequence[float]:
        embedding = self._model.get_text_embedding([text], use_tensor=False)
        return normalize_vector(_first_row(embedding))


def _first_row(value) -> tuple[float, ...]:
    row = value[0] if hasattr(value, "__getitem__") else value
    return tuple(float(item) for item in row)
