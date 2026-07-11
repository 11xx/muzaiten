from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence

from . import __version__
from .model import (
    ARTIFACT_DIRNAME,
    ARTIFACT_FORMAT_VERSION,
    AUDIO_MODEL_FILENAME,
    FEATURE_REVISION,
    MANIFEST_FILENAME,
    MODEL_AMODEL,
    MODEL_NAME,
    MODEL_SHA256,
    MODEL_VERSION,
    TEXT_MODEL_FILENAME,
    TOKENIZER_FILENAME,
    artifact_dir,
    artifact_status,
    checkpoint_status,
    file_sha256,
)

CONVERSION_STEPS = 5


@dataclass(frozen=True)
class ConversionResult:
    path: Path
    converted: bool


def convert_checkpoint(
    checkpoint: Path,
    *,
    output: Path | None = None,
    progress: Callable[[int, int], None] | None = None,
    canceled: Callable[[], bool] | None = None,
) -> ConversionResult:
    target = artifact_dir() if output is None else output
    current = artifact_status(path=target)
    if current.valid:
        return ConversionResult(target, converted=False)
    _require_conversion_dependencies()
    if file_sha256(checkpoint) != MODEL_SHA256:
        raise RuntimeError(f"CLAP checkpoint has wrong SHA-256: {checkpoint}")

    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{ARTIFACT_DIRNAME}-", dir=target.parent))
    try:
        _check_canceled(canceled)
        _emit_progress(progress, 0)
        clap = _load_reference_model(checkpoint)
        _emit_progress(progress, 1)
        _check_canceled(canceled)

        _save_tokenizer(clap.tokenize, staging / TOKENIZER_FILENAME)
        _emit_progress(progress, 2)
        _check_canceled(canceled)

        _export_audio(clap.model, staging / AUDIO_MODEL_FILENAME)
        _emit_progress(progress, 3)
        _check_canceled(canceled)

        _export_text(clap.model, staging / TEXT_MODEL_FILENAME)
        _emit_progress(progress, 4)
        _check_canceled(canceled)

        _verify_onnx(staging / AUDIO_MODEL_FILENAME)
        _verify_onnx(staging / TEXT_MODEL_FILENAME)
        manifest = _manifest(staging)
        with (staging / MANIFEST_FILENAME).open("w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write("\n")
        if not artifact_status(path=staging).valid:
            raise RuntimeError("converted CLAP artifacts failed manifest verification")
        _emit_progress(progress, 5)

        if target.exists():
            shutil.rmtree(target)
        os.replace(staging, target)
        return ConversionResult(target, converted=True)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def _require_conversion_dependencies() -> None:
    missing: list[str] = []
    for module in ("laion_clap", "onnx", "torch"):
        try:
            __import__(module)
        except ImportError:
            missing.append(module)
    if missing:
        names = ", ".join(missing)
        raise ModuleNotFoundError(
            f"model conversion requires {names}; install it with "
            "`uv tool install 'muzaiten-features-clap[model,convert]'`"
        )


def _load_reference_model(checkpoint: Path):
    import laion_clap

    clap = laion_clap.CLAP_Module(enable_fusion=False, amodel=MODEL_AMODEL, device="cpu")
    clap.load_ckpt(str(checkpoint), verbose=False)
    clap.model.eval()
    return clap


def _save_tokenizer(reference, output: Path) -> None:
    from tokenizers import Tokenizer

    vocab_path = reference.init_kwargs.get("vocab_file")
    candidate = Path(vocab_path).with_name(TOKENIZER_FILENAME) if vocab_path else None
    if candidate is not None and candidate.is_file():
        tokenizer = Tokenizer.from_file(str(candidate))
    else:
        tokenizer = Tokenizer.from_pretrained("roberta-base")
    tokenizer.save(str(output))


def _export_audio(model, output: Path) -> None:
    import torch

    def dynamic_window_reverse(windows, window_size, height, width):
        windows_per_batch = (height // window_size) * (width // window_size)
        batch = windows.shape[0] // windows_per_batch
        value = windows.view(
            batch,
            height // window_size,
            width // window_size,
            window_size,
            window_size,
            -1,
        )
        return value.permute(0, 1, 3, 2, 4, 5).contiguous().view(
            batch, height, width, -1
        )

    # LAION-CLAP's window helper casts the inferred batch size to a Python
    # int. That is mathematically redundant but freezes the exported graph to
    # its example batch. Replace only that shape helper in the export process;
    # all model layers and weights remain the reference package's own.
    for module in model.audio_branch.modules():
        if module.__class__.__name__ == "SwinTransformerBlock":
            module.forward.__func__.__globals__["window_reverse"] = dynamic_window_reverse
            break

    class AudioTower(torch.nn.Module):
        def __init__(self, clap_model) -> None:
            super().__init__()
            self.audio_branch = clap_model.audio_branch
            self.audio_projection = clap_model.audio_projection

        def forward(self, waveform):
            encoded = self.audio_branch(
                {"waveform": waveform}, mixup_lambda=None, device=waveform.device
            )["embedding"]
            return torch.nn.functional.normalize(self.audio_projection(encoded), dim=-1)

    tower = AudioTower(model).eval()
    example = torch.zeros((2, 480_000), dtype=torch.float32)
    batch = torch.export.Dim("batch", min=1)
    torch.onnx.export(
        tower,
        (example,),
        output,
        input_names=["waveform"],
        output_names=["embedding"],
        dynamic_shapes={"waveform": {0: batch}},
        opset_version=18,
        dynamo=True,
    )


def _export_text(model, output: Path) -> None:
    import torch

    class TextTower(torch.nn.Module):
        def __init__(self, clap_model) -> None:
            super().__init__()
            self.text_branch = clap_model.text_branch
            self.text_projection = clap_model.text_projection

        def forward(self, input_ids, attention_mask):
            pooled = self.text_branch(
                input_ids=input_ids,
                attention_mask=attention_mask,
            )["pooler_output"]
            return torch.nn.functional.normalize(self.text_projection(pooled), dim=-1)

    tower = TextTower(model).eval()
    input_ids = torch.ones((1, 77), dtype=torch.int64)
    attention_mask = torch.ones((1, 77), dtype=torch.int64)
    torch.onnx.export(
        tower,
        (input_ids, attention_mask),
        output,
        input_names=["input_ids", "attention_mask"],
        output_names=["embedding"],
        dynamic_axes={
            "input_ids": {0: "batch"},
            "attention_mask": {0: "batch"},
            "embedding": {0: "batch"},
        },
        opset_version=18,
        dynamo=False,
    )


def _verify_onnx(path: Path) -> None:
    import onnx

    onnx.checker.check_model(str(path))


def _manifest(path: Path) -> dict[str, object]:
    filenames = (AUDIO_MODEL_FILENAME, TEXT_MODEL_FILENAME, TOKENIZER_FILENAME)
    return {
        "format_version": ARTIFACT_FORMAT_VERSION,
        "model": MODEL_NAME,
        "checkpoint": MODEL_VERSION,
        "checkpoint_sha256": MODEL_SHA256,
        "provider_version": __version__,
        "feature_revision": FEATURE_REVISION,
        "vector_dimension": 512,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "artifacts": {
            filename: {
                "sha256": file_sha256(path / filename),
                "bytes": (path / filename).stat().st_size,
            }
            for filename in filenames
        },
    }


def _check_canceled(canceled: Callable[[], bool] | None) -> None:
    if canceled is not None and canceled():
        raise InterruptedError("model conversion canceled")


def _emit_progress(progress: Callable[[int, int], None] | None, completed: int) -> None:
    if progress is not None:
        progress(completed, CONVERSION_STEPS)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Convert the pinned CLAP checkpoint to ONNX")
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args(argv)
    checkpoint = args.checkpoint
    if checkpoint is None:
        current = checkpoint_status()
        if not current.present or not current.valid:
            parser.error(f"CLAP checkpoint is missing or invalid: {current.path}")
        checkpoint = current.path
    try:
        result = convert_checkpoint(
            checkpoint,
            output=args.output_dir,
            progress=lambda completed, total: print(
                f"Converting CLAP model: {completed}/{total}", file=sys.stderr
            ),
        )
    except (ModuleNotFoundError, FileNotFoundError) as exc:
        print(str(exc), file=sys.stderr)
        return 3
    except InterruptedError as exc:
        print(str(exc), file=sys.stderr)
        return 130
    except Exception as exc:  # noqa: BLE001 - standalone command boundary
        print(str(exc), file=sys.stderr)
        return 4
    action = "converted" if result.converted else "already valid"
    print(f"CLAP ONNX artifacts {action}: {result.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
