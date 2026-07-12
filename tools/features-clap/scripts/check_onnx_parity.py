from __future__ import annotations

import argparse
import json
import time
from collections.abc import Sequence
from pathlib import Path

import numpy as np

from muzaiten_features_clap.model import MODEL_AMODEL, OnnxClapEmbedder

AUDIO_MEAN_THRESHOLD = 0.9999
AUDIO_MIN_THRESHOLD = 0.999
TEXT_MIN_THRESHOLD = 0.999


def _waveforms() -> dict[str, list[np.ndarray]]:
    rng = np.random.default_rng(20260711)
    sample_rate = 48_000
    exact = 480_000
    classes: dict[str, list[np.ndarray]] = {
        "silence": [np.zeros(length, dtype=np.float32) for length in (48_000, 120_000, 240_000, exact)],
        "sines": [],
        "noise": [],
        "mixtures": [],
        "short-repeatpad": [],
        "exact-480000": [],
    }
    axis = np.arange(exact, dtype=np.float32) / sample_rate
    for index in range(16):
        frequency = 55.0 * 2.0 ** (index / 4.0)
        amplitude = 0.05 + 0.9 * index / 15.0
        classes["sines"].append(
            (amplitude * np.sin(2.0 * np.pi * frequency * axis)).astype(np.float32)
        )
        classes["noise"].append(
            rng.normal(0.0, 0.01 + 0.015 * index, exact).astype(np.float32)
        )
        mixture = (
            0.35 * np.sin(2.0 * np.pi * (110.0 + 17.0 * index) * axis)
            + 0.2 * np.sin(2.0 * np.pi * (880.0 + 31.0 * index) * axis)
            + rng.normal(0.0, 0.02, exact)
        )
        classes["mixtures"].append(mixture.astype(np.float32))
    for index, length in enumerate((1, 17, 127, 1001, 12_345, 47_999, 96_013, 239_999)):
        short_axis = np.arange(length, dtype=np.float32) / sample_rate
        classes["short-repeatpad"].append(
            (0.7 * np.sin(2.0 * np.pi * (220.0 + index * 37.0) * short_axis)).astype(np.float32)
        )
    for index in range(8):
        chirp = np.sin(2.0 * np.pi * (40.0 * axis + (20.0 + index * 5.0) * axis**2))
        envelope = np.linspace(0.05, 0.95, exact, dtype=np.float32)
        classes["exact-480000"].append((chirp * envelope).astype(np.float32))
    return classes


def _texts() -> list[str]:
    return [
        "melancholic shoegaze",
        "bright acoustic guitar with hand percussion",
        "deep bass, sparse drums, and distant vocals",
        "café com leite",
        "ピアノと静かな雨",
        "noise / drone (slowly evolving)",
        "",
        " ".join(f"token-{index}" for index in range(120)),
    ]


def _cosines(reference: np.ndarray, candidate: np.ndarray) -> np.ndarray:
    numerator = np.sum(reference * candidate, axis=1)
    denominator = np.linalg.norm(reference, axis=1) * np.linalg.norm(candidate, axis=1)
    return numerator / denominator


def _batched(values: Sequence[np.ndarray], size: int):
    for offset in range(0, len(values), size):
        yield values[offset : offset + size]


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare LAION-CLAP and converted ONNX pipelines")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--threads", type=int, default=8)
    args = parser.parse_args()

    import laion_clap
    import torch

    torch.set_num_threads(args.threads)
    started = time.perf_counter()
    reference = laion_clap.CLAP_Module(enable_fusion=False, amodel=MODEL_AMODEL, device="cpu")
    reference.load_ckpt(str(args.checkpoint), verbose=False)
    reference_load_seconds = time.perf_counter() - started

    started = time.perf_counter()
    candidate = OnnxClapEmbedder(artifacts=args.artifacts, device="cpu")
    candidate_init_seconds = time.perf_counter() - started

    tables: dict[str, dict[str, float | int]] = {}
    reference_seconds = 0.0
    candidate_seconds = 0.0
    all_reference: list[np.ndarray] = []
    all_candidate: list[np.ndarray] = []
    for class_name, waveforms in _waveforms().items():
        class_reference: list[np.ndarray] = []
        class_candidate: list[np.ndarray] = []
        for batch in _batched(waveforms, args.batch_size):
            started = time.perf_counter()
            reference_batch = reference.get_audio_embedding_from_data(list(batch), use_tensor=False)
            reference_seconds += time.perf_counter() - started
            started = time.perf_counter()
            candidate_batch = candidate.embed_audio_data(batch)
            candidate_seconds += time.perf_counter() - started
            class_reference.extend(reference_batch)
            class_candidate.extend(candidate_batch)
        reference_array = np.asarray(class_reference)
        candidate_array = np.asarray(class_candidate)
        cosines = _cosines(reference_array, candidate_array)
        tables[class_name] = {
            "n": len(waveforms),
            "mean_cosine": float(np.mean(cosines)),
            "min_cosine": float(np.min(cosines)),
        }
        all_reference.extend(class_reference)
        all_candidate.extend(class_candidate)

    texts = _texts()
    reference_tokens = reference.tokenizer(texts)
    candidate_ids, candidate_masks = candidate.tokenize_texts(texts)
    token_ids_equal = np.array_equal(reference_tokens["input_ids"].numpy(), candidate_ids)
    masks_equal = np.array_equal(reference_tokens["attention_mask"].numpy(), candidate_masks)
    reference_text = reference.get_text_embedding(texts, use_tensor=False)
    candidate_text = np.asarray(candidate.embed_texts(texts))
    text_cosines = _cosines(reference_text, candidate_text)
    started = time.perf_counter()
    for text in texts:
        reference.get_text_embedding([text], use_tensor=False)
    reference_text_seconds = time.perf_counter() - started
    started = time.perf_counter()
    for text in texts:
        candidate.embed_text(text)
    candidate_text_seconds = time.perf_counter() - started

    total_cosines = _cosines(np.asarray(all_reference), np.asarray(all_candidate))
    count = len(all_reference)
    result = {
        "audio": {
            "classes": tables,
            "total": {
                "n": count,
                "mean_cosine": float(np.mean(total_cosines)),
                "min_cosine": float(np.min(total_cosines)),
            },
        },
        "text": {
            "n": len(texts),
            "token_ids_exact": bool(token_ids_equal),
            "attention_masks_exact": bool(masks_equal),
            "mean_cosine": float(np.mean(text_cosines)),
            "min_cosine": float(np.min(text_cosines)),
        },
        "timing": {
            "reference_load_seconds": reference_load_seconds,
            "candidate_init_seconds": candidate_init_seconds,
            "reference_audio_ms_per_track": reference_seconds * 1000.0 / count,
            "candidate_audio_ms_per_track": candidate_seconds * 1000.0 / count,
            "reference_text_ms_per_query": reference_text_seconds * 1000.0 / len(texts),
            "candidate_text_ms_per_query": candidate_text_seconds * 1000.0 / len(texts),
        },
        "thresholds": {
            "audio_mean": AUDIO_MEAN_THRESHOLD,
            "audio_min": AUDIO_MIN_THRESHOLD,
            "text_min": TEXT_MIN_THRESHOLD,
        },
    }
    print(json.dumps(result, indent=2, sort_keys=True))

    passed = (
        token_ids_equal
        and masks_equal
        and float(np.mean(total_cosines)) >= AUDIO_MEAN_THRESHOLD
        and float(np.min(total_cosines)) >= AUDIO_MIN_THRESHOLD
        and float(np.min(text_cosines)) >= TEXT_MIN_THRESHOLD
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
