# Distro provider packaging gate

The provider itself builds as a pure-Python wheel/sdist. A distro package must
install that wheel with the distro's NumPy, PyTorch, and torchvision and package
pinned `laion-clap` 1.1.7 as ordinary Python files—never an embedded venv and
never `pip install` from `package()`.

Verified on 2026-07-10:

- base wheel install: NumPy only; no Torch/LAION-CLAP and no model download;
- model import and real CUDA text query: NumPy 2.4.2, Torch 2.12.1+cu130,
  torchvision 0.27.1+cu130, LAION-CLAP 1.1.7;
- NumPy 2.5.0 currently fails in Numba 0.66 before LAION-CLAP model
  construction (`Numba needs NumPy 2.4 or less`). This is a distro dependency
  compatibility gate, not a reason to hide a pip/venv environment in a package.

Consequently no misleading PKGBUILD is shipped yet: the current Arch clean
chroot lacks several LAION-CLAP runtime packages and its NumPy/Numba pair does
not pass the CPU import gate. Add the recipe only after those dependencies are
packaged or patched as distro packages, then require both clean-chroot CPU import
and a real CUDA query smoke before publishing it.
