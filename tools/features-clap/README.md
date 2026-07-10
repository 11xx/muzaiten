# muzaiten-features-clap

`muzaiten-features-clap` is a standalone tool for adding CLAP embeddings and
precomputed content-group neighbors to `features.sqlite`. It is not linked into
the Qt application.

```sh
uv run muzaiten-features-clap scan --features /path/to/features.sqlite
uv run muzaiten-features-clap neighbors --features /path/to/features.sqlite
uv run muzaiten-features-clap status --features /path/to/features.sqlite --json
uv run muzaiten-features-clap query "warm piano with brushed drums" --json
```

The model-loading commands (`scan`, `query`) accept `--device auto|cuda|cpu`
(default `auto` = CUDA when available, else CPU) and log the chosen device to
stderr at startup, so a silent CPU fallback is visible immediately. An explicit
`--device cuda` fails instead of quietly falling back. `status` reports the
device an `auto` run would pick. `scan` submits eight audio files per model
call by default so CUDA is used efficiently; `--batch-size N` tunes that
bounded batch for the available host/GPU memory. Each completed batch is
committed, so rerunning after a failure resumes by skipping durable rows.
Because non-fusion CLAP consumes one 10-second window, the scanner seeks to a
stable uniformly distributed window using the indexed duration instead of
decoding the rest of every track only to discard it; up to four of those
bounded windows decode concurrently before inference. If container metadata
overstates the decodable duration and a seek lands beyond EOF, the scanner
falls back to the first 10-second window. The same fallback applies when a
poorly seekable file does not produce its window within 30 seconds.

Tests use a fake embedder and do not download model weights. The real CLAP stack
is installed separately:

```sh
uv sync --extra model
```

The intended checkpoint is
`music_audioset_epoch_15_esc_90.14.pt` from `lukewys/laion_clap` on Hugging
Face. The repository and checkpoint are marked CC0-1.0; the checkpoint is
downloaded on first real-model use into `$XDG_CACHE_HOME/muzaiten/models/` (or
`~/.cache/muzaiten/models/`) and verified by SHA-256 before loading. Model
weights are never committed.
