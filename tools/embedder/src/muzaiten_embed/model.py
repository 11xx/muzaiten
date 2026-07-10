from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
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
MODEL_WINDOW_SECONDS = 10
DECODE_WORKERS = 4
SEEK_TIMEOUT_SECONDS = 30
DEVICE_CHOICES = ("auto", "cuda", "cpu")


def probe_device() -> str | None:
    """Device an ``auto`` run would pick, or None when torch is not installed."""
    try:
        import torch
    except ImportError:
        return None
    return "cuda" if torch.cuda.is_available() else "cpu"


def resolve_device(choice: str) -> str:
    try:
        import torch
    except ImportError as exc:  # pragma: no cover - depends on optional extra
        raise RuntimeError(
            "device selection requires optional dependencies; run "
            "`uv sync --extra model` in tools/embedder"
        ) from exc

    cuda_available = torch.cuda.is_available()
    if choice == "auto":
        return "cuda" if cuda_available else "cpu"
    if choice == "cuda" and not cuda_available:
        raise RuntimeError("--device cuda requested but torch reports no usable CUDA device")
    if choice not in DEVICE_CHOICES:
        raise RuntimeError(f"unknown device choice: {choice}")
    return choice


def device_label(device: str) -> str:
    if device == "cuda":
        import torch

        return f"cuda ({torch.cuda.get_device_name(0)})"
    return device


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


def _decode_audio_command(path: Path, duration_ms: int | None = None) -> list[str]:
    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
    ]
    window_ms = MODEL_WINDOW_SECONDS * 1000
    if duration_ms is not None and duration_ms > window_ms:
        # Non-fusion CLAP selects one uniformly random 10-second training
        # window after loading the whole track. A stable path hash preserves
        # that uniform corpus distribution without decoding discarded audio.
        span_ms = duration_ms - window_ms
        sample = int.from_bytes(hashlib.sha256(os.fsencode(path)).digest()[:8], "big")
        offset_ms = sample * span_ms // ((1 << 64) - 1)
        command.extend(["-ss", f"{offset_ms / 1000:.3f}"])
    command.extend(
        [
            "-i",
            str(path),
            "-t",
            str(MODEL_WINDOW_SECONDS),
            "-ac",
            "1",
            "-ar",
            str(MODEL_SAMPLE_RATE),
            "-f",
            "f32le",
            "-",
        ]
    )
    return command


def decode_audio_ffmpeg(path: Path, duration_ms: int | None = None):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - exercised only without dependencies
        raise RuntimeError("numpy is required for real audio embedding") from exc

    command = _decode_audio_command(path, duration_ms)
    try:
        completed = subprocess.run(
            command,
            check=True,
            capture_output=True,
            timeout=SEEK_TIMEOUT_SECONDS if "-ss" in command else None,
        )
    except subprocess.TimeoutExpired:
        completed = subprocess.run(
            _decode_audio_command(path),
            check=True,
            capture_output=True,
        )
    if not completed.stdout and "-ss" in command:
        # Indexed/container durations can overstate the decodable stream. A
        # successful seek beyond EOF produces no bytes, so keep the decode
        # bounded and fall back to the first model window.
        completed = subprocess.run(
            _decode_audio_command(path),
            check=True,
            capture_output=True,
        )
    audio = np.frombuffer(completed.stdout, dtype="<f4").astype("float32", copy=True)
    if audio.size == 0:
        raise RuntimeError(f"ffmpeg decoded no audio from {path}")
    return audio.reshape(1, -1)


class RealClapEmbedder:
    model = MODEL_NAME
    version = MODEL_VERSION

    def __init__(self, checkpoint: Path | None = None, device: str | None = None) -> None:
        try:
            import laion_clap
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise RuntimeError(
                "real CLAP embedding requires optional dependencies; run "
                "`uv sync --extra model` in tools/embedder"
            ) from exc

        self.device = device if device is not None else resolve_device("auto")
        checkpoint_path = checkpoint if checkpoint is not None else ensure_checkpoint().path
        self._model = laion_clap.CLAP_Module(
            enable_fusion=False,
            amodel=MODEL_AMODEL,
            device=self.device,
        )
        self._model.load_ckpt(str(checkpoint_path), verbose=False)

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        return self.embed_audio_paths([path])[0]

    def embed_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[Sequence[float]]:
        durations = [None] * len(paths) if durations_ms is None else durations_ms
        if len(durations) != len(paths):
            raise ValueError(
                f"received {len(durations)} durations for {len(paths)} audio paths"
            )
        if not paths:
            return []
        with ThreadPoolExecutor(max_workers=min(DECODE_WORKERS, len(paths))) as executor:
            decoded = [
                audio.reshape(-1)
                for audio in executor.map(decode_audio_ffmpeg, paths, durations)
            ]
        embedding = self._model.get_audio_embedding_from_data(
            x=decoded,
            use_tensor=False,
        )
        if len(embedding) != len(paths):
            raise RuntimeError(
                f"CLAP returned {len(embedding)} embeddings for {len(paths)} audio paths"
            )
        return [normalize_vector(row) for row in embedding]

    def embed_text(self, text: str) -> Sequence[float]:
        embedding = self._model.get_text_embedding([text], use_tensor=False)
        return normalize_vector(_first_row(embedding))


def _first_row(value) -> tuple[float, ...]:
    row = value[0] if hasattr(value, "__getitem__") else value
    return tuple(float(item) for item in row)
