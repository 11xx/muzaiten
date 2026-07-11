# muzaiten-features-clap

`muzaiten-features-clap` is the optional CLAP capability provider used by the
native `muzaiten-features` orchestrator. It is a small Python package by
default. Runtime inference uses NumPy, ONNX Runtime, and Tokenizers from the
`model` extra, without PyTorch, torchvision, librosa, numba, or LAION-CLAP.

Install the runtime in user space:

```sh
uv tool install 'muzaiten-features-clap[model]'
```

The provider is not a human-facing command. One process handles one versioned
JSON request on stdin and emits JSONL events on stdout. Supported operations are
`capabilities`, `status`, `model-download`, `scan`, `neighbors`, and `query`.
Diagnostics go to stderr. `muzaiten-features` owns provider discovery,
orchestration, locking, progress presentation, and cancellation.

Model download is always explicit. `scan` and `query` never fetch weights; they
return `model_missing` until the checkpoint has been converted into verified
ONNX artifacts. The one-time conversion uses the reference LAION-CLAP package
from the isolated `convert` extra:

```sh
uv tool install --reinstall 'muzaiten-features-clap[model,convert]'
muzaiten-features model download
uv tool install --reinstall 'muzaiten-features-clap[model]'
```

The middle command downloads and SHA-256-verifies the pinned CC0 checkpoint,
exports the audio and text towers, saves the exact RoBERTa tokenizer, verifies
the artifact manifest, and keeps the checkpoint as provenance. Reinstalling the
runtime-only extra removes the conversion stack without touching the model
cache or `features.sqlite`. The cache uses about 2.35 GB for the checkpoint and
790 MB for the fp32 ONNX artifacts. `status` reports source, license, cache
paths, checksums, device availability, and artifact validity without loading a
model session.

`FEATURE_REVISION` describes the input, preprocessing, and output semantics of
the stored vectors. It deliberately does not follow package or protocol
versions, so routine provider releases do not invalidate a large embedding
corpus.

Tests use fake inference and fake downloads; they do not download model weights:

```sh
uv sync --group dev
uv run python -m pytest
uv run ruff check .
```
