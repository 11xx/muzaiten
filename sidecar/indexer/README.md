# muzaiten-index

`muzaiten-index` is a standalone sidecar for building `features.sqlite` from a
muzaiten `library.sqlite`. It is intentionally not linked into the Qt
application or its CMake build.

```sh
cargo run --release -- scan \
  --library /path/to/library.sqlite \
  --features /path/to/features.sqlite \
  --stage all

cargo run --release -- status --features /path/to/features.sqlite
```

The indexer reads library track paths and metadata from `library.sqlite`, decodes
audio through the `ffmpeg` command-line binary, fingerprints through the `fpcalc`
command-line binary, and writes its own `features.sqlite`.

The intended bliss feature tier is currently blocked: the published Rust bliss
crates advertise GPL-3.0-only licensing, while this repository's settled
licensing rule only permits a permissively licensed bliss library. The schema is
created for later consumers, but `features` remains empty until a compatible
extractor is selected.
