from __future__ import annotations

import importlib
import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import __version__
from .model import (
    ALL_COMPONENTS,
    AUDIO_COMPONENT_APPROXIMATE_BYTES,
    DECODE_WORKERS,
    FEATURE_REVISION,
    MODEL_APPROXIMATE_BYTES,
    MODEL_ARTIFACTS_URL,
    MODEL_LICENSE,
    MODEL_NAME,
    MODEL_SHA256,
    MODEL_URL,
    MODEL_VERSION,
    ONNX_APPROXIMATE_BYTES,
    OnnxClapEmbedder,
    artifact_status,
    download_artifacts,
    download_checkpoint,
    probe_device,
    resolve_device,
)
from .ops import neighbors, query_embedding, scan, status

PROTOCOL_VERSION = 1
OPERATIONS = ("capabilities", "status", "model-download", "scan", "neighbors", "query")


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Request:
    request_id: str
    operation: str
    parameters: dict[str, object]


def parse_request(payload: object) -> Request:
    if not isinstance(payload, dict):
        raise ProtocolError("request must be a JSON object")
    if payload.get("protocol_version") != PROTOCOL_VERSION:
        raise ProtocolError(f"unsupported protocol_version; expected {PROTOCOL_VERSION}")
    request_id = payload.get("request_id")
    if not isinstance(request_id, str) or not request_id:
        raise ProtocolError("request_id must be a non-empty string")
    operation = payload.get("operation")
    if operation not in OPERATIONS:
        raise ProtocolError(f"unsupported operation: {operation!r}")
    parameters = payload.get("parameters", {})
    if not isinstance(parameters, dict):
        raise ProtocolError("parameters must be a JSON object")
    return Request(request_id, str(operation), parameters)


def capabilities() -> dict[str, object]:
    current = artifact_status(verify=False, required=("audio",))
    return {
        "capability": "clap",
        "provider_version": __version__,
        "protocol_versions": [PROTOCOL_VERSION],
        "operations": list(OPERATIONS),
        "feature_revision": FEATURE_REVISION,
        "vector_dimension": 512,
        "model": _model_payload(current.present, current.valid, current.path, current.components),
    }


def run_request(
    request: Request,
    emit: Callable[[dict[str, object]], None],
    canceled: Callable[[], bool],
    embedder_factory: Callable[..., object] = OnnxClapEmbedder,
) -> dict[str, object]:
    params = request.parameters
    if request.operation == "capabilities":
        return capabilities()
    if request.operation == "status":
        current = artifact_status(verify=False, required=("audio",))
        payload: dict[str, object] = {
            **capabilities(),
            "model": _model_payload(current.present, current.valid, current.path, current.components),
            "model_extra_installed": _runtime_dependencies_installed(),
            "device": probe_device() or "unavailable",
        }
        features = params.get("features")
        if features is not None:
            payload["store"] = status(_path(params, "features"), MODEL_NAME, MODEL_VERSION).as_dict()
        return payload
    if request.operation == "model-download":
        components = _components(params)
        # Full hash verification is correct here: this is the install-time
        # boundary, and an invalid bundle must be repaired, not trusted.
        current = artifact_status(required=components)
        if current.valid:
            return {
                "path": str(current.path),
                "downloaded": False,
                "sha256": MODEL_SHA256,
                "artifacts_path": str(current.path),
                "converted": False,
                "components": list(current.components),
            }
        started = time.monotonic()

        def download_progress(completed: int, total: int | None) -> None:
            emit(
                _progress_event(
                    request,
                    "model-download",
                    completed,
                    total,
                    "bytes",
                    started,
                )
            )

        if MODEL_ARTIFACTS_URL is not None:
            hosted = download_artifacts(
                MODEL_ARTIFACTS_URL,
                progress=download_progress,
                canceled=canceled,
                components=components,
            )
            installed = artifact_status(verify=False, required=components)
            return {
                "path": str(hosted.path),
                "downloaded": hosted.downloaded,
                "sha256": MODEL_SHA256,
                "artifacts_path": str(hosted.path),
                "converted": False,
                "components": list(installed.components),
            }

        result = download_checkpoint(progress=download_progress, canceled=canceled)
        from .convert import convert_checkpoint

        # Conversion rate/ETA must not inherit the download's elapsed time.
        convert_started = time.monotonic()
        converted = convert_checkpoint(
            result.path,
            progress=lambda completed, total: emit(
                _progress_event(
                    request,
                    "model-convert",
                    completed,
                    total,
                    "steps",
                    convert_started,
                )
            ),
            canceled=canceled,
        )
        return {
            "path": str(result.path),
            "downloaded": result.downloaded,
            "sha256": MODEL_SHA256,
            "artifacts_path": str(converted.path),
            "converted": converted.converted,
        }
    if request.operation == "scan":
        features_path = _path(params, "features")
        device_choice = _string(params, "device", "auto")
        limit = _optional_int(params, "limit")
        batch_size = _positive_int(params, "batch_size", 8)
        decode_workers = _positive_int(params, "decode_workers", DECODE_WORKERS)
        # Structural manifest check only: artifact hashes are verified when
        # the model is installed, and hashing 790 MB per invocation would
        # dominate interactive latency. ONNX Runtime rejects corrupt graphs.
        artifacts = artifact_status(verify=False, required=("audio",))
        if not artifacts.present or not artifacts.valid:
            raise FileNotFoundError(f"converted CLAP model is missing or invalid: {artifacts.path}")
        started = time.monotonic()
        embedder = embedder_factory(
            artifacts=artifacts.path,
            device=resolve_device(device_choice),
            decode_workers=decode_workers,
        )
        result = scan(
            features_path,
            embedder,  # type: ignore[arg-type]
            limit=limit,
            batch_size=batch_size,
            progress=lambda completed, total: emit(
                _progress_event(request, "semantic-embeddings", completed, total, "groups", started)
            ),
            canceled=canceled,
            checkpoint_sha256=MODEL_SHA256,
            feature_revision=FEATURE_REVISION,
            vector_dim=512,
            provider_path="muzaiten-features-clap",
            provider_version=__version__,
        )
        return {
            **result.as_dict(),
            "model": MODEL_NAME,
            "checkpoint_sha256": MODEL_SHA256,
            "feature_revision": FEATURE_REVISION,
            "device": str(getattr(embedder, "device", "unknown")),
        }
    if request.operation == "neighbors":
        started = time.monotonic()
        count = neighbors(
            _path(params, "features"),
            MODEL_NAME,
            MODEL_VERSION,
            top_k=_positive_int(params, "top_k", 100),
            progress=lambda completed, total: emit(
                _progress_event(request, "semantic-neighbors", completed, total, "groups", started)
            ),
            canceled=canceled,
        )
        return {"neighbor_rows": count, "algorithm_revision": "cosine-blockwise-v1"}
    if request.operation == "query":
        text = _string(params, "text")
        device_choice = _string(params, "device", "auto")
        if not text.strip():
            raise ProtocolError("text must not be empty")
        # Structural manifest check only: artifact hashes are verified when
        # the model is installed, and hashing 790 MB per invocation would
        # dominate interactive latency. ONNX Runtime rejects corrupt graphs.
        artifacts = artifact_status(verify=False, required=("text",))
        if not artifacts.present or not artifacts.valid:
            if artifacts.present and "audio" in artifacts.components:
                raise FileNotFoundError(
                    "the text model component is not installed (audio-only download); "
                    "text queries need the full model download")
            raise FileNotFoundError(f"converted CLAP model is missing or invalid: {artifacts.path}")
        embedder = embedder_factory(
            artifacts=artifacts.path,
            device=resolve_device(device_choice),
        )
        vector = query_embedding(text, embedder)  # type: ignore[arg-type]
        return {
            "vector": list(vector),
            "dim": len(vector),
            "capability": "clap",
            "model": MODEL_NAME,
            "checkpoint_sha256": MODEL_SHA256,
            "feature_revision": FEATURE_REVISION,
        }
    raise ProtocolError(f"unsupported operation: {request.operation}")


def event(request_id: str, kind: str, **payload: object) -> dict[str, object]:
    return {
        "protocol_version": PROTOCOL_VERSION,
        "request_id": request_id,
        "event": kind,
        **payload,
    }


def encode_event(payload: dict[str, object]) -> str:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _components(params: dict[str, object]) -> tuple[str, ...]:
    value = params.get("components", "full")
    if value == "full":
        return ALL_COMPONENTS
    if value == "audio":
        return ("audio",)
    raise ProtocolError('components must be "full" or "audio"')


def _model_payload(present: bool, valid: bool, path: Path,
                   components: tuple[str, ...] = ()) -> dict[str, object]:
    # With a hosted bundle the consent-relevant download is the artifacts
    # themselves; without one it is the checkpoint that conversion needs.
    hosted = MODEL_ARTIFACTS_URL is not None
    return {
        "name": MODEL_NAME,
        "checkpoint": MODEL_VERSION,
        "source": MODEL_ARTIFACTS_URL if hosted else MODEL_URL,
        "sha256": MODEL_SHA256,
        "approximate_bytes": ONNX_APPROXIMATE_BYTES if hosted else MODEL_APPROXIMATE_BYTES,
        "converted_approximate_bytes": ONNX_APPROXIMATE_BYTES,
        "audio_component_approximate_bytes": AUDIO_COMPONENT_APPROXIMATE_BYTES,
        "components": list(components),
        "license": MODEL_LICENSE,
        "cache_path": str(path),
        "present": present,
        "valid": valid,
    }


def _runtime_dependencies_installed() -> bool:
    try:
        for name in ("onnxruntime", "tokenizers"):
            importlib.import_module(name)
        return True
    except (ImportError, ValueError):
        return False


def _progress_event(
    request: Request,
    phase: str,
    completed: int,
    total: int | None,
    unit: str,
    started: float,
) -> dict[str, object]:
    elapsed = max(time.monotonic() - started, 0.0)
    rate = completed / elapsed if elapsed > 0 and completed > 0 else None
    eta = (total - completed) / rate if rate and total is not None else None
    return event(
        request.request_id,
        "progress",
        phase=phase,
        completed=completed,
        total=total,
        unit=unit,
        rate=rate,
        eta_seconds=eta,
    )


def _path(params: dict[str, object], name: str) -> Path:
    value = params.get(name)
    if not isinstance(value, str) or not value:
        raise ProtocolError(f"{name} must be a non-empty path string")
    return Path(value)


def _string(params: dict[str, object], name: str, default: str | None = None) -> str:
    value = params.get(name, default)
    if not isinstance(value, str):
        raise ProtocolError(f"{name} must be a string")
    return value


def _optional_int(params: dict[str, object], name: str) -> int | None:
    value = params.get(name)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError(f"{name} must be an integer")
    return value


def _positive_int(params: dict[str, object], name: str, default: int) -> int:
    value = params.get(name, default)
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ProtocolError(f"{name} must be a positive integer")
    return value
