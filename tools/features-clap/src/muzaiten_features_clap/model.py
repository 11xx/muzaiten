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
# v2: three deterministic 10 s windows per track, mean-pooled and
# renormalized, instead of one hash-placed window. Whole-track retrieval
# suffers when a single window lands on an unrepresentative section.
FEATURE_REVISION = "clap-htsat-base-audio-window-v2"
# What pre-v5 stores actually contained: one hash-placed window. Legacy
# migration must label those rows with the revision they were computed
# under, never the current one, or a revision bump would forge provenance.
LEGACY_FEATURE_REVISION = "clap-htsat-base-audio-window-v1"
# Windows per track for stored audio embeddings. Each window is a full
# HTSAT forward pass, so scan cost scales linearly with this.
MODEL_WINDOW_COUNT = 3
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
AUDIO_COMPONENT_APPROXIMATE_BYTES = 285_177_239
# Installable slices of the bundle. Radio, neighbors, and the analysis scan
# need only the audio tower; the text tower (plus its tokenizer) exists
# solely for free-text semantic queries, so audio-only installs save the
# ~500 MB text graph. The manifest always describes the full bundle; which
# files are on disk decides what is installed.
COMPONENT_FILES: dict[str, tuple[str, ...]] = {
    "audio": (AUDIO_MODEL_FILENAME,),
    "text": (TEXT_MODEL_FILENAME, TOKENIZER_FILENAME),
}
ALL_COMPONENTS = ("audio", "text")
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
    valid: bool  # every `required` component is installed and consistent
    manifest: dict[str, object] | None = None
    components: tuple[str, ...] = ()  # complete components on disk


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


def artifact_status(*, verify: bool = True, path: Path | None = None,
                    required: tuple[str, ...] = ALL_COMPONENTS) -> ArtifactStatus:
    current = artifact_dir() if path is None else path
    if not current.is_dir():
        return ArtifactStatus(current, present=False, valid=False)
    try:
        manifest, components = _read_and_validate_manifest(current, verify=verify)
    except (OSError, RuntimeError, TypeError, ValueError, json.JSONDecodeError):
        return ArtifactStatus(current, present=True, valid=False)
    valid = all(component in components for component in required)
    return ArtifactStatus(current, present=True, valid=valid, manifest=manifest,
                          components=components)


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
    components: tuple[str, ...] = ALL_COMPONENTS,
) -> ArtifactDownload:
    """Install a hosted, pre-converted artifact bundle without a checkpoint.

    `components` selects which slices land on disk; files belonging to an
    already-installed component are reused (hash-checked) rather than
    re-downloaded, so upgrading audio-only to full fetches only text files.
    """
    for component in components:
        if component not in COMPONENT_FILES:
            raise RuntimeError(f"unknown model component: {component}")
    if "audio" not in components:
        raise RuntimeError("model download always includes the audio component")
    target = artifact_dir()
    if artifact_status(path=target, required=components).valid:
        return ArtifactDownload(target, downloaded=False)

    prefix = base_url.rstrip("/")
    with opener(f"{prefix}/{MANIFEST_FILENAME}", timeout=60) as response:  # noqa: S310
        manifest = validate_manifest_identity(json.load(response))
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, dict)
    filenames = tuple(dict.fromkeys(
        filename for component in components for filename in COMPONENT_FILES[component]))
    reusable = []
    to_download = []
    for filename in filenames:
        existing = target / filename
        if existing.is_file() and file_sha256(existing) == artifacts[filename]["sha256"]:
            reusable.append(filename)
        else:
            to_download.append(filename)
    sizes = [artifacts[filename].get("bytes") for filename in to_download]
    total = sum(size for size in sizes if isinstance(size, int)) or None

    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{ARTIFACT_DIRNAME}-", dir=target.parent))
    try:
        for filename in reusable:
            shutil.copy2(target / filename, staging / filename)
        completed = 0
        if progress is not None:
            progress(completed, total)
        for filename in to_download:
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
        if not artifact_status(path=staging, required=components).valid:
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
        "vector_dimension": 512,
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise RuntimeError(f"ONNX artifact manifest has unexpected {key}")
    # feature_revision is deliberately NOT identity-matched: it names the
    # provider's preprocessing (window selection, pooling), not the graph
    # bytes. Exact-matching it would brick every installed bundle on a
    # windowing revision bump even though the artifacts are byte-identical;
    # the manifest keeps the value it was exported with, informationally.
    if not isinstance(manifest.get("feature_revision"), str) or not manifest.get("feature_revision"):
        raise RuntimeError("ONNX artifact manifest is missing feature_revision")
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


def _read_and_validate_manifest(path: Path, *, verify: bool) -> tuple[dict[str, object], tuple[str, ...]]:
    """Manifest identity plus which components are completely installed.

    The manifest always describes the full bundle; files may be absent for
    components the user chose not to download. An absent file only excludes
    its component, but a present file with a wrong hash poisons the whole
    directory: partial corruption must repair, not silently degrade.
    """
    with (path / MANIFEST_FILENAME).open(encoding="utf-8") as handle:
        manifest = validate_manifest_identity(json.load(handle))
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, dict)
    components: list[str] = []
    for component, filenames in COMPONENT_FILES.items():
        complete = True
        for filename in filenames:
            artifact = artifacts[filename]
            artifact_path = path / filename
            if not artifact_path.is_file():
                complete = False
                continue
            if verify and file_sha256(artifact_path) != artifact["sha256"]:
                raise RuntimeError(f"ONNX artifact has wrong SHA-256: {artifact_path}")
        if complete:
            components.append(component)
    if not components:
        raise RuntimeError(f"ONNX artifact directory has no complete component: {path}")
    return manifest, tuple(components)


def window_offsets_ms(path: Path, duration_ms: int | None, count: int = MODEL_WINDOW_COUNT) -> list[int | None]:
    """Deterministic window start offsets for one track.

    Extends the original single-window scheme: non-fusion CLAP trained on
    one uniformly random 10 s window, and a stable path hash preserved that
    corpus distribution. With `count` windows the legal start span splits
    into equal bands and each window is hash-placed uniformly inside its
    band, so windows spread across the track (early/middle/late for the
    default three) while the corpus-level distribution stays uniform.
    Unknown or window-or-shorter durations collapse to one whole read.
    """
    window_ms = MODEL_WINDOW_SECONDS * 1000
    if duration_ms is None or duration_ms <= window_ms:
        return [None]
    span_ms = duration_ms - window_ms
    bounded = max(1, count)
    offsets: list[int | None] = []
    for index in range(bounded):
        digest = hashlib.sha256(os.fsencode(path) + b"?window=%d" % index).digest()
        sample = int.from_bytes(digest[:8], "big")
        band_start = span_ms * index // bounded
        band_end = span_ms * (index + 1) // bounded
        offsets.append(band_start + sample * (band_end - band_start) // ((1 << 64) - 1))
    return offsets


def _decode_audio_command(path: Path, offset_ms: int | None = None) -> list[str]:
    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
    ]
    if offset_ms is not None:
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


def decode_audio_ffmpeg(path: Path, duration_ms: int | None = None, offset_ms: int | None = None):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - exercised only without dependencies
        raise RuntimeError("numpy is required for real audio embedding") from exc

    if offset_ms is None and duration_ms is not None:
        # Single-window compatibility entry: place the one window with the
        # same banded scheme the multi-window path uses.
        offset_ms = window_offsets_ms(path, duration_ms, 1)[0]
    command = _decode_audio_command(path, offset_ms)
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

    def __init__(
        self,
        artifacts: Path | None = None,
        device: str | None = None,
        decode_workers: int = DECODE_WORKERS,
    ) -> None:
        try:
            import numpy as np
            import onnxruntime as ort
            from tokenizers import Tokenizer
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise RuntimeError(
                "real CLAP embedding requires optional dependencies; run "
                "`uv sync --extra model` in tools/features-clap"
            ) from exc

        if decode_workers < 1:
            raise ValueError("decode workers must be at least 1")
        self.device = device if device is not None else resolve_device("auto")
        self.decode_workers = decode_workers
        if artifacts is None:
            # Audio is the floor of every install; text-only operations get
            # a friendlier error at session-creation time.
            current = artifact_status(required=("audio",))
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
        # Lazy: the tokenizer belongs to the text component, which an
        # audio-only install does not ship.
        self._tokenizer_factory = Tokenizer
        self._tokenizer = None

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

    def _decode_audio_window(self, path: Path, duration_ms: int | None, offset_ms: int | None):
        started = time.perf_counter()
        try:
            return decode_audio_ffmpeg(path, duration_ms, offset_ms)
        finally:
            self._decode_timings_ms.append((time.perf_counter() - started) * 1000)

    def embed_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[Sequence[float]]:
        decoded = self.decode_audio_paths(paths, durations_ms)
        embedding = self.embed_audio_data(decoded)
        if len(embedding) != len(paths):
            raise RuntimeError(f"CLAP returned {len(embedding)} embeddings for {len(paths)} paths")
        return [normalize_vector(row) for row in embedding]

    def decode_audio_paths(
        self,
        paths: Sequence[Path],
        durations_ms: Sequence[int | None] | None = None,
    ) -> Sequence[object]:
        durations = [None] * len(paths) if durations_ms is None else durations_ms
        if len(durations) != len(paths):
            raise ValueError(
                f"received {len(durations)} durations for {len(paths)} audio paths"
            )
        if not paths:
            return []
        # One flat decode job per (track, window): windows of different
        # tracks share the pool, so a short track cannot serialize behind a
        # long one's remaining windows.
        specs: list[tuple[Path, int | None, int | None]] = []
        counts: list[int] = []
        for path, duration in zip(paths, durations, strict=True):
            offsets = window_offsets_ms(path, duration)
            counts.append(len(offsets))
            specs.extend((path, duration, offset) for offset in offsets)
        with ThreadPoolExecutor(max_workers=min(self.decode_workers, len(specs))) as executor:
            flat = [
                audio.reshape(-1)
                for audio in executor.map(lambda spec: self._decode_audio_window(*spec), specs)
            ]
        grouped: list[object] = []
        start = 0
        for count in counts:
            grouped.append(tuple(flat[start : start + count]))
            start += count
        return grouped

    def embed_audio_data(self, waveforms: Sequence[object]) -> Sequence[Sequence[float]]:
        """One embedding per item; an item may be one waveform or a window group.

        A tuple/list item is treated as that track's windows: every window
        runs through the model (the graph L2-normalizes each row) and the
        rows are mean-pooled. Callers normalize the returned rows, which
        renormalizes the pooled mean; plain waveform items pass through
        exactly as before.
        """
        if not waveforms:
            return []
        groups = [
            list(value) if isinstance(value, (list, tuple)) else [value]
            for value in waveforms
        ]
        flat = [prepare_waveform(window) for group in groups for window in group]
        batch = self._np.stack(flat)
        session = self._get_audio_session()
        started = time.perf_counter()
        try:
            rows = session.run(["embedding"], {"waveform": batch})[0]
        finally:
            self._infer_timings_ms.append((time.perf_counter() - started) * 1000)
        pooled = []
        start = 0
        for group in groups:
            segment = rows[start : start + len(group)]
            start += len(group)
            pooled.append(segment[0] if len(group) == 1 else segment.mean(axis=0))
        return pooled

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
        encodings = self._get_tokenizer().encode_batch(list(texts))
        input_ids = self._np.asarray([item.ids for item in encodings], dtype=self._np.int64)
        masks = self._np.asarray([item.attention_mask for item in encodings], dtype=self._np.int64)
        return input_ids, masks

    def _require_component_file(self, filename: str, component: str) -> Path:
        path = self._artifact_path / filename
        if not path.is_file():
            raise FileNotFoundError(
                f"the {component} model component is not installed ({path} is missing); "
                "run `muzaiten-features model download` with the full bundle"
            )
        return path

    def _get_tokenizer(self):
        if self._tokenizer is None:
            tokenizer = self._tokenizer_factory.from_file(
                str(self._require_component_file(TOKENIZER_FILENAME, "text")))
            tokenizer.enable_truncation(max_length=77)
            tokenizer.enable_padding(length=77, pad_id=1, pad_token="<pad>")
            self._tokenizer = tokenizer
        return self._tokenizer

    def _get_audio_session(self):
        if self._audio_session is None:
            self._audio_session = self._ort.InferenceSession(
                str(self._require_component_file(AUDIO_MODEL_FILENAME, "audio")),
                sess_options=self._session_options,
                providers=self._providers,
            )
        return self._audio_session

    def _get_text_session(self):
        if self._text_session is None:
            self._text_session = self._ort.InferenceSession(
                str(self._require_component_file(TEXT_MODEL_FILENAME, "text")),
                sess_options=self._session_options,
                providers=self._providers,
            )
        return self._text_session
