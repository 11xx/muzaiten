from __future__ import annotations

import importlib.util
import json
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import __version__
from .model import (
    FEATURE_REVISION,
    MODEL_APPROXIMATE_BYTES,
    MODEL_LICENSE,
    MODEL_NAME,
    MODEL_SHA256,
    MODEL_URL,
    MODEL_VERSION,
    RealClapEmbedder,
    checkpoint_status,
    device_label,
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
    current = checkpoint_status(verify=False)
    return {
        "capability": "clap",
        "provider_version": __version__,
        "protocol_versions": [PROTOCOL_VERSION],
        "operations": list(OPERATIONS),
        "feature_revision": FEATURE_REVISION,
        "vector_dimension": 512,
        "model": _model_payload(current.present, current.valid, current.path),
    }


def run_request(
    request: Request,
    emit: Callable[[dict[str, object]], None],
    canceled: Callable[[], bool],
    embedder_factory: Callable[..., object] = RealClapEmbedder,
) -> dict[str, object]:
    params = request.parameters
    if request.operation == "capabilities":
        return capabilities()
    if request.operation == "status":
        current = checkpoint_status()
        payload: dict[str, object] = {
            **capabilities(),
            "model": _model_payload(current.present, current.valid, current.path),
            "model_extra_installed": importlib.util.find_spec("laion_clap") is not None,
            "device": probe_device() or "unavailable",
        }
        features = params.get("features")
        if features is not None:
            payload["store"] = status(_path(params, "features"), MODEL_NAME, MODEL_VERSION).as_dict()
        return payload
    if request.operation == "model-download":
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

        result = download_checkpoint(progress=download_progress, canceled=canceled)
        return {
            "path": str(result.path),
            "downloaded": result.downloaded,
            "sha256": MODEL_SHA256,
        }
    if request.operation == "scan":
        features_path = _path(params, "features")
        device_choice = _string(params, "device", "auto")
        limit = _optional_int(params, "limit")
        batch_size = _positive_int(params, "batch_size", 8)
        checkpoint = checkpoint_status()
        if not checkpoint.present or not checkpoint.valid:
            raise FileNotFoundError(f"CLAP checkpoint is missing or invalid: {checkpoint.path}")
        started = time.monotonic()
        embedder = embedder_factory(
            checkpoint=checkpoint.path,
            device=resolve_device(device_choice),
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
        )
        return {
            **result.as_dict(),
            "model": MODEL_NAME,
            "checkpoint_sha256": MODEL_SHA256,
            "feature_revision": FEATURE_REVISION,
            "device": device_label(str(getattr(embedder, "device", "unknown"))),
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
        checkpoint = checkpoint_status()
        if not checkpoint.present or not checkpoint.valid:
            raise FileNotFoundError(f"CLAP checkpoint is missing or invalid: {checkpoint.path}")
        embedder = embedder_factory(
            checkpoint=checkpoint.path,
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


def _model_payload(present: bool, valid: bool, path: Path) -> dict[str, object]:
    return {
        "name": MODEL_NAME,
        "checkpoint": MODEL_VERSION,
        "source": MODEL_URL,
        "sha256": MODEL_SHA256,
        "approximate_bytes": MODEL_APPROXIMATE_BYTES,
        "license": MODEL_LICENSE,
        "cache_path": str(path),
        "present": present,
        "valid": valid,
    }


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
