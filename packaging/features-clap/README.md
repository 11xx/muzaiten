# Distro provider packaging gate

The provider itself builds as a pure-Python wheel/sdist. A distro package must
install that wheel with the distro's NumPy, PyTorch, and torchvision and package
pinned `laion-clap` 1.1.7 as ordinary Python files—never an embedded venv and
never `pip install` from `package()`.

Rechecked and explicitly deferred on 2026-07-11:

- base wheel install: NumPy only; no Torch/LAION-CLAP and no model download;
- model import and real CUDA text query: NumPy 2.4.2, Torch 2.12.1+cu130,
  torchvision 0.27.1+cu130, LAION-CLAP 1.1.7;
- Arch ships NumPy 2.5.1 with Numba 0.66.0; that pair fails before LAION-CLAP
  model construction because Numba requires NumPy 2.4 or earlier;
- `python-webdataset` and `python-wandb` are available only from the AUR, while
  neither the Arch repositories nor the AUR provide LAION-CLAP 1.1.7.

Consequently distro-native provider packaging is deferred for this release and
no misleading PKGBUILD is shipped. Reopen the gate only when Arch's NumPy/Numba
pair imports together and LAION-CLAP plus all runtime dependencies can be
packaged as ordinary distro packages. Then require a clean-chroot CPU import and
query smoke plus a real CUDA query smoke before publishing. The supported
user-space provider remains the PyPI package installed by uv; this deferral does
not affect native-only analysis or the already published optional provider.
