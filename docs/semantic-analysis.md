# Semantic Analysis

Semantic audio similarity and free-text search are optional. Native identity,
duplicate grouping, and scalar analysis work without Python or model files.
The saved setting `analysis.semantic.enabled` defaults to `false`.

## Install the provider

The GUI never runs uv. No Arch/AUR package currently provides the optional
provider. Arch and other Linux users install the published provider themselves
in user space:

```sh
uv tool install 'muzaiten-features-clap[model]'
```

The runtime `model` extra contains only NumPy, ONNX Runtime, and Tokenizers.
The standard ONNX Runtime package runs on CPU. A custom environment exposing
ONNX Runtime's CUDA execution provider can use the saved `auto` or `cuda`
device setting; an explicit `cuda` request fails instead of silently using CPU
when that provider is unavailable.

### Move a checkout installation to PyPI

`uv tool upgrade` preserves the requirement source recorded when the tool was
installed. If the original requirement was a checkout path, upgrading rebuilds
from that checkout; it does not switch the tool to the package index. Recreate
the tool with a registry requirement instead:

```sh
uv tool install --reinstall --no-sources \
  'muzaiten-features-clap[model]'
```

An explicit uninstall is unnecessary: `uv tool install` replaces the existing
uv-managed tool and records the new registry requirement. Add
`==<provider-version>` to the requirement when an exact release is required.

The core `muzaiten-bin` and `muzaiten-git` packages deliberately do not name a
nonexistent provider as an optional dependency. When the distro gate passes,
`muzaiten-features-clap` will be published as its own AUR package and restored
to their optional-dependency metadata. This changes only package-manager
discovery; the uv-installed executable is already found through the normal
provider search order.

Recreating the tool environment does not remove the model cache or touch
`features.sqlite`, so it does not force semantic reanalysis. The provider's stable
`feature_revision`, rather than its package-install source, controls embedding
compatibility.

Provider discovery is ordered and handshake-gated: `--provider`,
`MUZAITEN_FEATURES_CLAP`, the saved GUI path, a sibling executable, uv tool-bin
locations, then `PATH`. `muzaiten-features status --json` reports the accepted
path and source. The GUI's provider setup dialog can select a custom executable
or reset to automatic discovery.

## Consent and model lifecycle

Neither semantic scans nor text queries download weights. Use **Library > Audio
analysis > Download semantic model…** or:

```sh
muzaiten-features model download --progress=jsonl
```

Before the first download and conversion, temporarily install the isolated
conversion stack:

```sh
uv tool install --reinstall 'muzaiten-features-clap[model,convert]'
muzaiten-features model download --progress=jsonl
uv tool install --reinstall 'muzaiten-features-clap[model]'
```

The download writes a temporary file, streams byte progress, verifies the
checksum, and atomically installs the pinned CC0 checkpoint. Conversion then
reports `model-convert` progress, exports fp32 audio and text ONNX graphs plus
the exact tokenizer, and verifies their hashed manifest. The checkpoint stays
in the cache as the provenance source. Removing the `convert` extra afterward
drops PyTorch, torchvision, librosa, numba, and LAION-CLAP from the serving
environment without removing cached artifacts. Expect about 2.35 GB for the
checkpoint and 790 MB for the ONNX artifacts; the runtime tool environment is
roughly 150 MB instead of about 5 GB.

If conversion dependencies are missing, model download exits 3 with
`component_missing` and prints the required `[model,convert]` install command.
A missing or invalid converted model is reported as `model_missing` (exit 3).

## Refresh and search

Enable **Generate semantic/audio-similarity features**, then run the normal
audio analysis command. The orchestrator serializes native files, grouping,
scalar features, semantic embeddings, and—only when needed—neighbors while
holding `features.sqlite.lock` across all writing phases. Completed inference
batches are resumable.

```sh
muzaiten-features refresh --semantic --progress=jsonl
muzaitenctl semantic-search "melancholic shoegaze" --limit 10
```

`muzaitenctl` asks `muzaiten-features query --json` for the text vector, then
keeps ranking and preferred-copy selection locally. Query provenance must match
the active semantic generation. The hidden `--query-vector-json` option remains
available only for deterministic tests.

## Provenance

Schema v5 records one active semantic generation: capability, checkpoint hash,
stable feature revision, vector dimension, provider diagnostics, and timestamps.
Embeddings and neighbors are generation-scoped. Activating a changed generation
hides stale rows immediately and exposes truthful partial coverage during a
resumable refresh; matching legacy CLAP rows migrate into the known generation,
while mixed or unknown rows stay inactive.
