# muzaiten-features-clap

`muzaiten-features-clap` is the optional CLAP capability provider used by the
native `muzaiten-features` orchestrator. It is a small Python package by
default; NumPy/PyTorch/torchvision and LAION-CLAP are confined to the `model`
extra.

Install it in user space with a backend selected by uv:

```sh
uv tool install 'muzaiten-features-clap[model]' --torch-backend auto
```

The provider is not a human-facing command. One process handles one versioned
JSON request on stdin and emits JSONL events on stdout. Supported operations are
`capabilities`, `status`, `model-download`, `scan`, `neighbors`, and `query`.
Diagnostics go to stderr. `muzaiten-features` owns provider discovery,
orchestration, locking, progress presentation, and cancellation.

Model download is always explicit. `scan` and `query` never fetch weights; they
return `model_missing` until `muzaiten-features model download` has installed
and SHA-256-verified the checkpoint atomically. `status` reports the source,
license, approximate size, cache path, checksum, device availability, and
current presence without loading LAION-CLAP.

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
