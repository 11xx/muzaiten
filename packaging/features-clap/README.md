# Distro provider packaging gate

The supported provider is currently the pure-Python wheel/sdist installed from
PyPI with uv. The Arch-native route is gated: do not publish a stub, embedded
virtual environment, pip/uv-driven PKGBUILD, model-bearing package, or
Muzaiten-only fork merely to make an optional-dependency link resolve.

## What the gate measures

The ONNX runtime provider serves with only NumPy, ONNX Runtime, and
Tokenizers, so the retired NumPy/Numba/LAION-CLAP compatibility chain no
longer gates distro packaging. A distro-native `muzaiten-features-clap`
package becomes a candidate when all three hold:

- every runtime dependency is resolvable on Arch: `python-numpy` (official),
  `python-onnxruntime` (official, satisfied by the `-cpu`/`-cuda`/`-rocm`
  variants through `provides`), and `python-tokenizers` (AUR, acceptable for
  an AUR package);
- the published PyPI provider's `[model]` extra is the ONNX runtime, proving
  the ONNX release actually shipped; and
- a hosted converted-artifact bundle is configured (`MODEL_ARTIFACTS_URL` in
  the provider's `model.py`), because a distro package cannot ask users to
  install the one-time `[convert]` stack, which is itself not
  distro-packageable (LAION-CLAP pins `numpy<2`, and its librosa/numba chain
  conflicts with current Arch NumPy).

The conversion stack stays confined to the PyPI `[convert]` extra and to
maintainer machines; see "Hosting the converted artifact bundle" in
`docs/distribution.md`.

`muzaiten-features-clap` is published on the AUR and restored as an optional
dependency of the core Muzaiten packages. Native analysis remains
Python-free, and an existing uv tool remains fully supported and
discoverable.

## Deterministic index check

Run the read-only checker instead of reconstructing package state manually:

```sh
python packaging/features-clap/check-arch-gate.py
python packaging/features-clap/check-arch-gate.py --json
python packaging/features-clap/check-arch-gate.py --json --strict
```

The stable JSON schema records the Arch runtime packages, the published
provider's `[model]` extra, the hosted-bundle configuration, planned AUR
packages, and reasons. Ordinary reporting exits zero even while blocked;
`--strict` exits 1 for `blocked` and 2 for an index/protocol error. An index
result can only be `blocked` or `candidate`: `candidate` means run the
runtime gates below, never that publication is already safe.

## Reopening and publication

When the gate reports `candidate`, publish in dependency order:

1. a date-versioned PyPI provider release whose `[model]` extra is the ONNX
   runtime and whose `MODEL_ARTIFACTS_URL` points at the hosted bundle;
2. `muzaiten-features-clap` on the AUR, built from that PyPI sdist, depending
   on `python-numpy` and `python-onnxruntime` (official; the backend variants
   satisfy it through `provides`), `python-tokenizers` (AUR), and `ffmpeg`;
   and
3. the restored optdepends entries in `muzaiten-bin` and `muzaiten-git`.

The AUR package uses `python-build --wheel --no-isolation` and
`python-installer` and installs ordinary files under `/usr`. Neither the
checkpoint nor the converted artifacts are packaged: the model remains an
explicit, checksum-verified user download outside pacman ownership, fetched
from the hosted bundle by `muzaiten-features model download`.

Validate in a clean devtools chroot with a temporary local repository:
source verification, Namcap, archive/privacy inspection, offline base status,
structured model-missing behavior, a mocked hosted-bundle download lifecycle,
and a real CPU text query plus synthetic-audio scan against an isolated test
cache. Never package the model or use live library data. Only after all
gates pass may the AUR package and optional-dependency metadata be published.
