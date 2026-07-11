# Distro provider packaging gate

The supported provider is currently the pure-Python wheel/sdist installed from
PyPI with uv. The Arch-native route is upstream-gated: do not publish a stub,
embedded virtual environment, pip/uv-driven PKGBUILD, model-bearing package, or
Muzaiten-only fork merely to make an optional-dependency link resolve.

## Current blockers

Rechecked on 2026-07-11:

- Arch NumPy 2.5.1 does not satisfy Numba 0.66.0's `numpy<2.5` requirement.
- LAION-CLAP 1.1.7 itself requires `numpy<2.0.0`, so a future Numba update alone
  is insufficient.
- `python-torchlibrosa` and `python-laion-clap` are absent from Arch/AUR.
- LAION-CLAP's complete runtime chain also consumes several AUR packages,
  including ftfy, braceexpand, webdataset, wget, wandb, and transformers.

The core Muzaiten AUR packages therefore do not advertise the unresolved
provider. Native analysis remains Python-free, and an existing uv tool remains
fully supported and discoverable.

## Deterministic index check

Run the read-only checker instead of reconstructing package state manually:

```sh
python packaging/features-clap/check-arch-gate.py
python packaging/features-clap/check-arch-gate.py --json
python packaging/features-clap/check-arch-gate.py --json --strict
```

The stable JSON schema records Arch, PyPI, and AUR versions, requirements,
planned packages, and reasons. Ordinary reporting exits zero even while blocked;
`--strict` exits 1 for `blocked` and 2 for an index/protocol error. An index
result can only be `blocked` or `candidate`: `candidate` means run the runtime
gates below, never that publication is already safe.

## Reopening and publication

Reopen only after official Arch NumPy imports with official Numba and an
upstream LAION-CLAP release declares support for that NumPy. Do not carry a
downstream behavioral compatibility patch. Before changing the provider pin,
compare fixed synthetic audio/text vectors with the current 1.1.7 music-model
baseline: cosine similarity must be at least 0.999999 and maximum absolute
component error at most 1e-5. A mismatch requires a separate semantic-generation
change and `feature_revision` bump, not silent packaging.

Publish in dependency order:

1. `python-torchlibrosa`, from an immutable upstream sdist;
2. `python-laion-clap`, as the complete upstream package with system deps;
3. a newly date-versioned PyPI provider with the tested dependency pin;
4. `muzaiten-features-clap`, built from that PyPI sdist; and
5. the restored optdepends entries in `muzaiten-bin` and `muzaiten-git`.

All Python packages use `python-build --wheel --no-isolation` and
`python-installer`, install ordinary files under `/usr`, and declare system
NumPy, PyTorch, torchvision, ffmpeg, and the full upstream dependency chain as
appropriate. CUDA variants satisfy the same dependency names through Arch's
`provides`; no backend-specific provider package is needed. The model remains an
explicit, checksum-verified user download outside pacman ownership.

Build the chain in a clean devtools chroot with a temporary local repository.
Require source verification, Namcap, archive/privacy inspection, offline base
status, structured model-missing behavior, mocked download lifecycle, a real
CPU query plus synthetic-audio scan, and a real CUDA query/scan on an available
host. Copy the already verified checkpoint into an isolated test cache; never
package it or use live library data. Only after all gates pass may the AUR
packages and optional-dependency metadata be published.
