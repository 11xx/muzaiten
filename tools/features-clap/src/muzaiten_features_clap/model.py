from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

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
MODEL_APPROXIMATE_BYTES = 2_352_471_003
MODEL_LICENSE = "CC0-1.0"
# Changes only when model input, preprocessing, or output semantics change.
FEATURE_REVISION = "clap-htsat-base-audio-window-v1"
DECODE_WORKERS = 4
SEEK_TIMEOUT_SECONDS = 30
DEVICE_CHOICES = ("auto", "cuda", "cpu")
ARTIFACT_FORMAT_VERSION = 1
ARTIFACT_DIRNAME = "clap-onnx-v1"
AUDIO_MODEL_FILENAME = "audio.onnx"
TEXT_MODEL_FILENAME = "text.onnx"
TOKENIZER_FILENAME = "tokenizer.json"
MANIFEST_FILENAME = "manifest.json"
ONNX_APPROXIMATE_BYTES = 790_186_110
# Base URL of the hosted, pre-converted artifact bundle (manifest.json plus
# the three artifacts it names). With this set, model download fetches the
# artifacts directly and end users need no checkpoint download or conversion
# stack; the [convert] extra remains for building from the checkpoint.
MODEL_ARTIFACTS_URL: str | None = (
    "https://huggingface.co/muzaiten/clap-htsat-base-onnx/resolve/main"
)
INFERENCE_THREADS_ENV = "MUZAITEN_CLAP_THREADS"
DEFAULT_MAX_INFERENCE_THREADS = 8


def probe_device() -> str | None:
    """Device an ``auto`` run would pick, or None without ONNX Runtime."""
    try:
        import onnxruntime as ort
    except ImportError:
        return None
    return "cuda" if "CUDAExecutionProvider" in ort.get_available_providers() else "cpu"


def resolve_device(choice: str) -> str:
    try:
        import onnxruntime as ort
    except ImportError as exc:  # pragma: no cover - depends on optional extra
        raise RuntimeError(
            "device selection requires optional dependencies; run "
            "`uv sync --extra model` in tools/features-clap"
        ) from exc

    cuda_available = "CUDAExecutionProvider" in ort.get_available_providers()
    if choice == "auto":
        return "cuda" if cuda_available else "cpu"
    if choice == "cuda" and not cuda_available:
        raise RuntimeError(
            "--device cuda requested but ONNX Runtime reports no CUDA execution provider"
        )
    if choice not in DEVICE_CHOICES:
        raise RuntimeError(f"unknown device choice: {choice}")
    return choice


def inference_thread_count() -> int:
    configured = os.environ.get(INFERENCE_THREADS_ENV)
    if configured is not None:
        try:
            threads = int(configured)
        except ValueError as exc:
            raise RuntimeError(f"{INFERENCE_THREADS_ENV} must be a positive integer") from exc
        if threads < 1:
            raise RuntimeError(f"{INFERENCE_THREADS_ENV} must be a positive integer")
        return threads
    try:
        available = len(os.sched_getaffinity(0))
    except AttributeError:  # pragma: no cover - non-POSIX fallback
        available = os.cpu_count() or 1
    return max(1, min(DEFAULT_MAX_INFERENCE_THREADS, available))


@dataclass(frozen=True)
class ModelDownload:
    path: Path
    downloaded: bool


@dataclass(frozen=True)
class CheckpointStatus:
    path: Path
    present: bool
    valid: bool


@dataclass(frozen=True)
class ArtifactStatus:
    path: Path
    present: bool
    valid: bool
    manifest: dict[str, object] | None = None


def model_cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME")
    root = Path(base) if base else Path.home() / ".cache"
    return root / "muzaiten" / "models"


def checkpoint_status(*, verify: bool = True) -> CheckpointStatus:
    path = model_cache_dir() / MODEL_VERSION
    if not path.exists():
        return CheckpointStatus(path, present=False, valid=False)
    if not verify:
        return CheckpointStatus(path, present=True, valid=True)
    try:
        _verify_sha256(path)
    except RuntimeError:
        return CheckpointStatus(path, present=True, valid=False)
    return CheckpointStatus(path, present=True, valid=True)


def artifact_dir() -> Path:
    return model_cache_dir() / ARTIFACT_DIRNAME


def artifact_status(*, verify: bool = True, path: Path | None = None) -> ArtifactStatus:
    current = artifact_dir() if path is None else path
    if not current.is_dir():
        return ArtifactStatus(current, present=False, valid=False)
    try:
        manifest = _read_and_validate_manifest(current, verify=verify)
    except (OSError, RuntimeError, TypeError, ValueError, json.JSONDecodeError):
        return ArtifactStatus(current, present=True, valid=False)
    return ArtifactStatus(current, present=True, valid=True, manifest=manifest)


def download_checkpoint(
    progress: Callable[[int, int | None], None] | None = None,
    canceled: Callable[[], bool] | None = None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> ModelDownload:
    cache_dir = model_cache_dir()
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_dir / MODEL_VERSION
    if path.exists():
        _verify_sha256(path)
        return ModelDownload(path, downloaded=False)

    temp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=cache_dir, delete=False) as temp:
            temp_path = Path(temp.name)
            digest = hashlib.sha256()
            with opener(MODEL_URL, timeout=60) as response:  # type: ignore[call-arg] # noqa: S310
                length = response.headers.get("Content-Length")
                total = int(length) if length else None
                completed = 0
                if progress is not None:
                    progress(completed, total)
                while True:
                    if canceled is not None and canceled():
                        raise InterruptedError("checkpoint download canceled")
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    digest.update(chunk)
                    temp.write(chunk)
                    completed += len(chunk)
                    if progress is not None:
                        progress(completed, total)
        if digest.hexdigest() != MODEL_SHA256:
            raise RuntimeError("downloaded CLAP checkpoint failed SHA-256 verification")
        temp_path.replace(path)
    except BaseException:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)
        raise
    return ModelDownload(path, downloaded=True)


@dataclass(frozen=True)
class ArtifactDownload:
    path: Path
    downloaded: bool


def download_artifacts(
    base_url: str,
    progress: Callable[[int, int | None], None] | None = None,
    canceled: Callable[[], bool] | None = None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> ArtifactDownload:
    """Install a hosted, pre-converted artifact bundle without a checkpoint."""
    target = artifact_dir()
    if artifact_status(path=target).valid:
        return ArtifactDownload(target, downloaded=False)

    prefix = base_url.rstrip("/")
    with opener(f"{prefix}/{MANIFEST_FILENAME}", timeout=60) as response:  # noqa: S310
        manifest = validate_manifest_identity(json.load(response))
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, dict)
    filenames = (AUDIO_MODEL_FILENAME, TEXT_MODEL_FILENAME, TOKENIZER_FILENAME)
    sizes = [artifacts[filename].get("bytes") for filename in filenames]
    total = sum(size for size in sizes if isinstance(size, int)) or None

    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{ARTIFACT_DIRNAME}-", dir=target.parent))
    try:
        completed = 0
        if progress is not None:
            progress(completed, total)
        for filename in filenames:
            digest = hashlib.sha256()
            with opener(f"{prefix}/{filename}", timeout=60) as response:  # noqa: S310
                with (staging / filename).open("wb") as handle:
                    while True:
                        if canceled is not None and canceled():
                            raise InterruptedError("artifact download canceled")
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        digest.update(chunk)
                        handle.write(chunk)
                        completed += len(chunk)
                        if progress is not None:
                            progress(completed, total)
            if digest.hexdigest() != artifacts[filename]["sha256"]:
                raise RuntimeError(f"downloaded CLAP artifact failed SHA-256: {filename}")
        with (staging / MANIFEST_FILENAME).open("w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write("\n")
        if not artifact_status(path=staging).valid:
            raise RuntimeError("downloaded CLAP artifacts failed manifest verification")
        if target.exists():
            shutil.rmtree(target)
        os.replace(staging, target)
        return ArtifactDownload(target, downloaded=True)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def _verify_sha256(path: Path) -> None:
    if file_sha256(path) != MODEL_SHA256:
        raise RuntimeError(f"cached CLAP checkpoint has wrong SHA-256: {path}")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_manifest_identity(manifest: object) -> dict[str, object]:
    """Check a manifest's model identity and artifact table, not its files."""
    if not isinstance(manifest, dict):
        raise RuntimeError("invalid ONNX artifact manifest")
    expected = {
        "format_version": ARTIFACT_FORMAT_VERSION,
        "model": MODEL_NAME,
        "checkpoint": MODEL_VERSION,
        "checkpoint_sha256": MODEL_SHA256,
        "feature_revision": FEATURE_REVISION,
        "vector_dimension": 512,
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise RuntimeError(f"ONNX artifact manifest has unexpected {key}")
    if not isinstance(manifest.get("provider_version"), str):
        raise RuntimeError("ONNX artifact manifest is missing provider_version")
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise RuntimeError("ONNX artifact manifest is missing artifact hashes")
    for filename in (AUDIO_MODEL_FILENAME, TEXT_MODEL_FILENAME, TOKENIZER_FILENAME):
        artifact = artifacts.get(filename)
        if not isinstance(artifact, dict) or not isinstance(artifact.get("sha256"), str):
            raise RuntimeError(f"ONNX artifact manifest is missing {filename}")
    return manifest


def _read_and_validate_manifest(path: Path, *, verify: bool) -> dict[str, object]:
    with (path / MANIFEST_FILENAME).open(encoding="utf-8") as handle:
        manifest = validate_manifest_identity(json.load(handle))
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, dict)
    for filename in (AUDIO_MODEL_FILENAME, TEXT_MODEL_FILENAME, TOKENIZER_FILENAME):
        artifact = artifacts[filename]
        artifact_path = path / filename
        if not artifact_path.is_file():
            raise RuntimeError(f"ONNX artifact is missing: {artifact_path}")
        if verify and file_sha256(artifact_path) != artifact["sha256"]:
            raise RuntimeError(f"ONNX artifact has wrong SHA-256: {artifact_path}")
    return manifest


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


def _run_ffmpeg(command: list[str], timeout: float | None = None):
    try:
        return subprocess.run(command, check=True, capture_output=True, timeout=timeout)
    except FileNotFoundError as exc:
        # Without this, a missing ffmpeg binary surfaces as a misleading
        # model_missing protocol error instead of naming the real dependency.
        raise RuntimeError(
            "ffmpeg is required to decode audio; install ffmpeg and ensure it is on PATH"
        ) from exc


def decode_audio_ffmpeg(path: Path, duration_ms: int | None = None):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - exercised only without dependencies
        raise RuntimeError("numpy is required for real audio embedding") from exc

    command = _decode_audio_command(path, duration_ms)
    try:
        completed = _run_ffmpeg(
            command,
            timeout=SEEK_TIMEOUT_SECONDS if "-ss" in command else None,
        )
    except subprocess.TimeoutExpired:
        completed = _run_ffmpeg(_decode_audio_command(path))
    if not completed.stdout and "-ss" in command:
        # Indexed/container durations can overstate the decodable stream. A
        # successful seek beyond EOF produces no bytes, so keep the decode
        # bounded and fall back to the first model window.
        completed = _run_ffmpeg(_decode_audio_command(path))
    audio = np.frombuffer(completed.stdout, dtype="<f4").astype("float32", copy=True)
    if audio.size == 0:
        raise RuntimeError(f"ffmpeg decoded no audio from {path}")
    return audio.reshape(1, -1)


def prepare_waveform(value):
    """Replicate LAION-CLAP's non-fusion preprocessing for one waveform."""
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - optional runtime dependency
        raise RuntimeError("numpy is required for real audio embedding") from exc

    waveform = np.asarray(value, dtype=np.float32).reshape(-1)
    if waveform.size == 0:
        raise ValueError("cannot embed an empty audio waveform")
    waveform = (np.clip(waveform, -1.0, 1.0) * 32767.0).astype(np.int16)
    waveform = (waveform / 32767.0).astype(np.float32)
    target = MODEL_SAMPLE_RATE * MODEL_WINDOW_SECONDS
    if waveform.size < target:
        repeats = target // waveform.size
        waveform = np.tile(waveform, repeats)
        waveform = np.pad(waveform, (0, target - waveform.size))
    elif waveform.size > target:
        waveform = waveform[:target]
    return np.ascontiguousarray(waveform, dtype=np.float32)


class OnnxClapEmbedder:
    model = MODEL_NAME
    version = MODEL_VERSION
    dimension = 512

    def __init__(self, artifacts: Path | None = None, device: str | None = None) -> None:
        try:
            import numpy as np
            import onnxruntime as ort
            from tokenizers import Tokenizer
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise RuntimeError(
                "real CLAP embedding requires optional dependencies; run "
                "`uv sync --extra model` in tools/features-clap"
            ) from exc

        self.device = device if device is not None else resolve_device("auto")
        if artifacts is None:
            current = artifact_status()
            if not current.present or not current.valid:
                raise FileNotFoundError(
                    f"converted CLAP model is missing or invalid: {current.path}; "
                    "run `muzaiten-features model download`"
                )
            artifact_path = current.path
        else:
            artifact_path = artifacts
        self._np = np
        self._ort = ort
        self._artifact_path = artifact_path
        self._providers = (
            ["CUDAExecutionProvider", "CPUExecutionProvider"]
            if self.device == "cuda"
            else ["CPUExecutionProvider"]
        )
        self._session_options = ort.SessionOptions()
        self._session_options.intra_op_num_threads = inference_thread_count()
        self._session_options.inter_op_num_threads = 1
        self._audio_session = None
        self._text_session = None
        self._decode_timings_ms: list[float] = []
        self._infer_timings_ms: list[float] = []
        self._tokenizer = Tokenizer.from_file(str(artifact_path / TOKENIZER_FILENAME))
        self._tokenizer.enable_truncation(max_length=77)
        self._tokenizer.enable_padding(length=77, pad_id=1, pad_token="<pad>")

    def embed_audio_path(self, path: Path) -> Sequence[float]:
        return self.embed_audio_paths([path])[0]

    def reset_scan_timings(self) -> None:
        self._decode_timings_ms.clear()
        self._infer_timings_ms.clear()

    @property
    def scan_timings(self) -> dict[str, list[float]]:
        return {
            "decode_ms": self._decode_timings_ms.copy(),
            "infer_ms": self._infer_timings_ms.copy(),
        }

    def _decode_audio_path(self, path: Path, duration_ms: int | None):
        started = time.perf_counter()
        try:
            return decode_audio_ffmpeg(path, duration_ms)
        finally:
            self._decode_timings_ms.append((time.perf_counter() - started) * 1000)

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
                for audio in executor.map(self._decode_audio_path, paths, durations)
            ]
        embedding = self.embed_audio_data(decoded)
        if len(embedding) != len(paths):
            raise RuntimeError(f"CLAP returned {len(embedding)} embeddings for {len(paths)} paths")
        return [normalize_vector(row) for row in embedding]

    def embed_audio_data(self, waveforms: Sequence[object]) -> Sequence[Sequence[float]]:
        if not waveforms:
            return []
        batch = self._np.stack([prepare_waveform(value) for value in waveforms])
        session = self._get_audio_session()
        started = time.perf_counter()
        try:
            return session.run(["embedding"], {"waveform": batch})[0]
        finally:
            self._infer_timings_ms.append((time.perf_counter() - started) * 1000)

    def embed_text(self, text: str) -> Sequence[float]:
        return normalize_vector(self.embed_texts([text])[0])

    def embed_texts(self, texts: Sequence[str]) -> Sequence[Sequence[float]]:
        if not texts:
            return []
        input_ids, attention_mask = self.tokenize_texts(texts)
        session = self._get_text_session()
        return session.run(
            ["embedding"],
            {"input_ids": input_ids, "attention_mask": attention_mask},
        )[0]

    def tokenize_texts(self, texts: Sequence[str]):
        encodings = self._tokenizer.encode_batch(list(texts))
        input_ids = self._np.asarray([item.ids for item in encodings], dtype=self._np.int64)
        masks = self._np.asarray([item.attention_mask for item in encodings], dtype=self._np.int64)
        return input_ids, masks

    def _get_audio_session(self):
        if self._audio_session is None:
            self._audio_session = self._ort.InferenceSession(
                str(self._artifact_path / AUDIO_MODEL_FILENAME),
                sess_options=self._session_options,
                providers=self._providers,
            )
        return self._audio_session

    def _get_text_session(self):
        if self._text_session is None:
            self._text_session = self._ort.InferenceSession(
                str(self._artifact_path / TEXT_MODEL_FILENAME),
                sess_options=self._session_options,
                providers=self._providers,
            )
        return self._text_session
