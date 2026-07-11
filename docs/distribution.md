# Distribution & Packaging

This document covers how muzaiten is packaged and published, how Last.fm
credentials are embedded (and what that does and does not protect), and the
recommended AUR naming.

Everything packaging-related lives under `packaging/`:

```
packaging/
├── org.11xx.muzaiten.desktop        # freedesktop launcher
├── org.11xx.muzaiten.metainfo.xml   # AppStream metadata
├── org.11xx.muzaiten.svg            # scalable app icon
├── build-release.sh                 # build + tar a release artifact
└── aur/
    ├── muzaiten-git/                 # PKGBUILD + .SRCINFO (source/VCS)
    └── muzaiten-bin/                 # PKGBUILD + .SRCINFO (prebuilt)
```

The desktop entry, icon, and metainfo are installed by the normal CMake
`install()` rules, so `cmake --install` (and therefore every package below) drops
full freedesktop integration into the prefix.

## Installing from source (local)

`make install` is **user-space by default**: with no `PREFIX` it installs into
`~/.local`, so a normal install needs no root.

```sh
make build CMAKE_BUILD_TYPE=Release
make install                  # -> ~/.local (no sudo)
sudo make install PREFIX=/usr # system-wide instead

make uninstall                # reverse the last install
```

`make uninstall` deletes exactly the files the last `make install` recorded in
`<BUILD_DIR>/install_manifest.txt`. Because that manifest stores absolute paths,
uninstall is **prefix-agnostic** — it cleans whichever prefix you installed into,
whether the `~/.local` default or a custom `PREFIX` (use `sudo` for a system
prefix). Only files are removed, never the shared directories
(`share/applications`, `icons/hicolor`, …) that other packages also populate.
Distro packages do not use this path: they stage into `$pkgdir`/`DESTDIR` and let
the package manager own removal.

## Package naming (recommendation)

Reserve all three names — `muzaiten`, `muzaiten-bin`, `muzaiten-git` — but
**publish only `muzaiten-bin` and `muzaiten-git`** for now:

- **`muzaiten-git`** — builds the latest commit from source. Best for people who
  want to build it themselves; it ships **no** embedded Last.fm key (users add
  their own in-app).
- **`muzaiten-bin`** — the maintainer-built prebuilt binary, with the embedded
  Last.fm key. Best for people who do not want to compile a Qt app.
- **`muzaiten`** (no suffix) — by AUR convention this is a *versioned source
  release* (a build of a tagged source tarball). muzaiten now has date-tagged
  releases, but no separate source-release AUR package is maintained yet. Adopt
  this name only when you want a stable source package distinct from both the
  prebuilt artifact and the VCS build; until then it would mostly duplicate
  `-git`.

So: use the two suffixed names now, and keep the bare `muzaiten` reserved for a
future tagged-release source package. The three packages `provides`/`conflicts`
each other, so only one can be installed at a time.

## Last.fm credentials: what "embedded" really means

muzaiten can ship a default Last.fm API key + shared secret so scrobbling works
out of the box. These are injected **at build time** (`-DMUZAITEN_LASTFM_API_KEY`,
`-DMUZAITEN_LASTFM_SHARED_SECRET`); they never live in the source tree, and a
build with no key simply leaves Last.fm inactive until the user supplies their
own (`Scrobblers > Last.fm API settings…`, or the `LASTFM_API_KEY` /
`LASTFM_SHARED_SECRET` env vars).

**Is it safe / obfuscated enough? Be honest with yourself: no client-side secret
is truly secret.** This is a fundamental limitation, not a muzaiten bug — every
open-source desktop scrobbler (Rhythmbox, Quod Libet, …) ships an app key the
same way. What muzaiten does and does not do:

- The key/secret are **XOR-obfuscated at compile time** (`LastFmCredentials.h`)
  so the plaintext does **not** appear in a `strings` dump and is only
  reconstructed in memory at runtime. `build-release.sh` also strips the binary.
  This defeats casual `strings`/grep scraping and automated key-harvesting bots.
- It does **not** defeat a determined reverse-engineer: the XOR pad ships in the
  same binary, so anyone willing to read the disassembly or dump process memory
  can recover the secret. Treat the embedded key as *public-ish*.

Practical guidance:

1. Only put real keys in artifacts **you** build and control — the release
   tarball and `muzaiten-bin`. `muzaiten-git` (and any from-source build) ships
   with empty defaults on purpose.
2. The Last.fm **API key** is sent on every request and is effectively public
   anyway. The **shared secret** is the sensitive half; rotate it from your
   Last.fm API account if you ever see abuse — the embedded value being leaked
   only lets someone forge requests *as your application*, not access user
   accounts.
3. If you would rather not embed anything, ship with empty defaults; users paste
   their own key once and it is stored in their local config, never in the
   binary.

## Cutting a release

```sh
# keys come from the environment or a local .env (never committed)
MUZAITEN_LASTFM_API_KEY=... MUZAITEN_LASTFM_SHARED_SECRET=... ./packaging/build-release.sh
```

This produces, under `dist/`:

- `muzaiten-<version>-<arch>.tar.zst` — a prefixed tree containing `muzaiten`,
  `muzaitenctl`, `muzaiten-features`, and the lightweight `muzaiten-import`
  helper, plus the desktop entry, icon, metainfo, and license; and
- `muzaiten-<version>-<arch>.tar.zst.sha256`.

`<version>` is the date-based `YYYY.MM.DD.N.g<sha>` derived from `HEAD`, where
`N` is the number of commits made on HEAD's UTC calendar day. The release tag is
the shorter UTC date, `YYYY.MM.DD`, or the next same-day iteration if that date
already exists (`YYYY.MM.DD.1`, then `.2`, and so on). Build the artifact from
the tested, tagged release commit, then upload both files to the Codeberg release
attached to that tag. The tarball name still uses `<version>`, not the shorter
tag name.

The optional Python provider is not bundled into native artifacts. Build its
pure-Python wheel and sdist independently:

```sh
cd tools/features-clap
uv build --no-sources
```

The base wheel depends only on NumPy; LAION-CLAP, PyTorch, and torchvision are
confined to `[model]`. Provider releases use the same UTC date as the matching
native release. Python package indexes normalize out leading zeroes under PEP
440, so native tag `2026.07.11` corresponds to provider version `2026.7.11`;
same-day release iterations append the same final numeric component. Recheck the
PyPI project endpoint immediately before a separately approved first
publication. Core AUR packages list `muzaiten-features-clap` only as an optional
dependency.

### Dry-running the prebuilt-dist packaging (dev)

Before uploading an artifact and bumping the published `muzaiten-bin` PKGBUILD,
you can rehearse the whole prebuilt-dist flow against the freshly built tarball —
no upload, no checksum bookkeeping:

```sh
./packaging/build-release.sh --dev-pkgbuild   # or DEV_PKGBUILD=1 ...
```

This is a hidden dev option (not shown in `--help`). In addition to the normal
`dist/` tarball, it writes:

```
dist/aur-dev/muzaiten-dev/
├── PKGBUILD     # a muzaiten-bin clone, sourcing the local tarball
├── .SRCINFO
└── muzaiten-<version>-<arch>.tar.zst   # copied next to the PKGBUILD
```

The generated `PKGBUILD` sources the local tarball (no protocol → makepkg finds
it beside the PKGBUILD) with its real `sha256sums`, so you can build and inspect
the actual package locally:

```sh
( cd dist/aur-dev/muzaiten-dev && makepkg -f )   # then namcap / pacman -Qlp the result
```

`dist/` is gitignored, so none of this is published — it only proves the
prebuilt-dist `package()` produces a correct `/usr` tree before you commit to the
real release. The dev package mirrors the standard `/usr` tarball, so leave
`PREFIX` at its default when generating it (the script warns otherwise).

### Why not a single fully-static binary?

A fully statically linked Qt GUI binary is impractical: a stock Qt is not built
for static linking, and GStreamer/GLib/D-Bus load plugins at runtime regardless.
The realistic options are:

- **Dynamically linked against system libraries** (what the release tarball and
  `muzaiten-bin` do) — small, and Arch already provides Qt 6, GStreamer, TagLib,
  and zstd. This is the recommended path for AUR.
- **AppImage** — bundle Qt + plugins for a cross-distro portable binary. Not
  needed for AUR; consider it only for non-Arch users. It does not change the
  credential story above.

## Publishing to the AUR

Each package is mirrored in this repo under `packaging/aur/<pkgname>/`, but the
AUR publication target is a separate Git repository per package base:

- `ssh://aur@aur.archlinux.org/muzaiten-git.git`
- `ssh://aur@aur.archlinux.org/muzaiten-bin.git`

The AUR repos contain only `PKGBUILD` and `.SRCINFO`. Keep the copies in this
source repo as the source of truth, then copy those two files into the AUR repos
when publishing. On first publication, cloning an empty AUR package repo is
expected; the first successful push creates the package page.

### Publish `muzaiten-git`

`muzaiten-git` can be published or updated as soon as the source branch is pushed
to Codeberg, because its `source=()` pulls the repository directly.

```sh
cd "$(git rev-parse --show-toplevel)/packaging/aur/muzaiten-git"
makepkg --printsrcinfo > .SRCINFO
namcap PKGBUILD

workdir="$(mktemp -d)"
git clone ssh://aur@aur.archlinux.org/muzaiten-git.git "$workdir/muzaiten-git"
cp PKGBUILD .SRCINFO "$workdir/muzaiten-git/"
cd "$workdir/muzaiten-git"
git add PKGBUILD .SRCINFO
git commit -m "update muzaiten-git"
git push origin master
```

### Publish `muzaiten-bin`

`muzaiten-bin` must not be pushed until the release tarball is uploaded and
fetchable from Codeberg. AUR users build from the `source=()` URL, so a missing
release asset makes the package immediately broken.

1. Cut and push the release tag.
2. Build the release artifact with Last.fm credentials supplied from the
   environment or local `.env`:

   ```sh
   ./packaging/build-release.sh --dev-pkgbuild
   ```

3. Upload both generated files to the Codeberg release attached to the tag:

   ```text
   dist/muzaiten-<version>-<arch>.tar.zst
   dist/muzaiten-<version>-<arch>.tar.zst.sha256
   ```

4. Update `packaging/aur/muzaiten-bin/PKGBUILD`:

   - set `_release_tag` to the Codeberg release tag (`YYYY.MM.DD` or same-day
     iteration),
   - set `pkgver` to the artifact `<version>` from `build-release.sh`, and
   - set `sha256sums` to the checksum from the generated `.sha256` file.

5. Regenerate metadata and validate:

   ```sh
   cd "$(git rev-parse --show-toplevel)/packaging/aur/muzaiten-bin"
   makepkg --printsrcinfo > .SRCINFO
   namcap PKGBUILD
   ```

6. Validate the prebuilt package path locally before publishing. The generated
   dev package sources the local tarball and catches archive/layout issues
   before the public AUR recipe is pushed:

   ```sh
   cd "$(git rev-parse --show-toplevel)/dist/aur-dev/muzaiten-dev"
   makepkg -f
   namcap muzaiten-dev-*.pkg.tar.zst
   ```

7. After the Codeberg release asset is live, verify the public download path from
   the AUR PKGBUILD:

   ```sh
   cd "$(git rev-parse --show-toplevel)/packaging/aur/muzaiten-bin"
   workdir="$(mktemp -d)"
   git clone ssh://aur@aur.archlinux.org/muzaiten-bin.git "$workdir/muzaiten-bin"
   cp PKGBUILD .SRCINFO "$workdir/muzaiten-bin/"
   cd "$workdir/muzaiten-bin"
   makepkg --verifysource
   git add PKGBUILD .SRCINFO
   git commit -m "update muzaiten-bin"
   git push origin master
   ```

`makepkg --verifysource` should download the Codeberg tarball and pass the
recorded `sha256sums` check. If the URL returns 404, upload or fix the Codeberg
release asset before pushing the AUR repo.
