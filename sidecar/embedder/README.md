# muzaiten-embed

`muzaiten-embed` is a standalone sidecar for adding CLAP embeddings and
precomputed content-group neighbors to `features.sqlite`. It is not linked into
the Qt application.

```sh
uv run muzaiten-embed scan --features /path/to/features.sqlite
uv run muzaiten-embed neighbors --features /path/to/features.sqlite
uv run muzaiten-embed status --features /path/to/features.sqlite --json
uv run muzaiten-embed query "warm piano with brushed drums" --json
```

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
