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

### Publishing the optional Python provider

The optional provider is not bundled into native artifacts and does not release
in lockstep with the application. Publish it only when the provider package,
protocol implementation, dependency contract, or other provider-facing behavior
changes. A native-only Muzaiten release can continue using the existing PyPI
release; do not rebuild or republish an unchanged provider merely to match a new
application date.

The base wheel depends only on NumPy; LAION-CLAP, PyTorch, and torchvision stay
confined to the `[model]` extra. Installing or inspecting the base package must
not download a model checkpoint.

When the provider does ship as part of a coordinated Muzaiten release, use the
same UTC release date. Python package indexes normalize out leading zeroes under
PEP 440, so native tag `2026.07.11` corresponds to provider version
`2026.7.11`; same-day release iterations append the same final numeric
component. Package version and `feature_revision` are deliberately independent:
a package-only or protocol-version bump does not invalidate stored embeddings
unless the model input, preprocessing, or vector semantics actually changed.

#### One-time publication configuration

Both PyPI and TestPyPI trust the GitHub repository `11xx/muzaiten`, workflow
`publish-features-clap.yml`, and their respective `pypi` or `testpypi`
environment. The matching GitHub environments:

- allow deployments only from the `master` branch;
- disallow administrator bypass of their protection rules; and
- contain no passwords, API tokens, secrets, or package-index credentials.

The workflow grants `id-token: write` only to its two-step publication job. The
build and archive-audit job has read-only repository access, and passes its
immutable artifacts to the publication job. PyPI exchanges the short-lived OIDC
identity for a short-lived upload token; no reusable credential belongs in the
repository or GitHub settings.

#### Release runbook

1. Choose a new provider version. Distribution filenames are immutable within
   each index: never upload different contents under a version already present
   on that index. Use the same candidate version on TestPyPI and then PyPI.
   Update the version in
   `tools/features-clap/pyproject.toml` and
   `tools/features-clap/src/muzaiten_features_clap/__init__.py`, then regenerate
   `tools/features-clap/uv.lock` with `uv lock`.

2. Build and inspect the distributions locally:

   ```sh
   cd tools/features-clap
   rm -rf dist
   uv sync --group dev
   uv build --no-sources
   uvx --from twine==6.2.0 twine check dist/*
   .venv/bin/python -m pytest -q
   .venv/bin/python -m ruff check .
   cd ../..
   make build && make test
   ```

   Confirm that the wheel and sdist contain only the intended package source and
   metadata. The GitHub workflow repeats the metadata and privacy/archive audits
   on a clean runner.

3. Commit the tested release source and push the same commit to Codeberg and
   GitHub. Confirm that GitHub's `master` resolves to that commit before
   dispatching publication.

4. Dispatch **Publish CLAP provider** for `testpypi`, always selecting
   `master`. This can be done in the GitHub Actions UI or with an authenticated
   GitHub CLI:

   ```sh
   gh workflow run publish-features-clap.yml \
     --repo 11xx/muzaiten --ref master -f index=testpypi
   gh run watch --repo 11xx/muzaiten
   ```

5. Check the TestPyPI project JSON and install the exact version in a fresh
   environment. Run a `status` protocol request with isolated `XDG_CACHE_HOME`
   and `XDG_DATA_HOME`; the base install must report the expected provider
   version, no model extra, and no downloaded model/cache files. Do not rely only
   on the green workflow header.

6. Recheck the committed diff, workflow run SHA, public package metadata, and
   archive contents. Do not advance GitHub `master` between the TestPyPI and
   PyPI dispatches: they are separate snapshot runs, and production must build
   the same commit that passed TestPyPI. Only after TestPyPI succeeds, dispatch
   the same workflow for `pypi`:

   ```sh
   gh workflow run publish-features-clap.yml \
     --repo 11xx/muzaiten --ref master -f index=pypi
   gh run watch --repo 11xx/muzaiten
   ```

   Production publication is irreversible and remains an explicit maintainer
   decision even when the surrounding checks are automated.

7. Verify the real PyPI JSON metadata, install the exact version from PyPI in a
   fresh isolated environment, and verify both wheel and sdist attestations:

   ```sh
   uvx --from pypi-attestations pypi-attestations verify pypi \
     --repository https://github.com/11xx/muzaiten <distribution-file-url>
   ```

   The distribution URLs and SHA-256 digests are available from
   `https://pypi.org/pypi/muzaiten-features-clap/<version>/json`.

Each manual dispatch is a commit snapshot. For `workflow_dispatch`, GitHub binds
the run to the last commit on the selected branch or tag; checkout therefore
builds that recorded SHA, and both jobs within that run use the artifact named
for that SHA. Commits pushed after dispatch do not alter that run. Because
TestPyPI and PyPI currently require separate dispatches, keep `master` fixed
between those two runs and verify both `head_sha` values match. After production
publication, PyPI retains the published files independently of all future
repository work.

It is normal for later native commits—and even later native releases—to move
past the provider's published source snapshot. Installed users continue to get
the latest published provider until a genuinely new provider version is
released. This independent cadence is safe while the native orchestrator and
provider retain compatible protocol, capability, generation-fingerprint, and
`feature_revision` contracts. If a native change breaks that compatibility,
publish and verify the matching provider before or together with the native
release.

GitHub supports dispatch through the web UI, `gh workflow run`, or its REST API,
so an agent can automate version edits, local gates, pushes, TestPyPI dispatch,
run monitoring, isolated installation, metadata checks, and attestation
verification. A future single-run promotion workflow could build once, publish
to TestPyPI, pause for environment approval, and send the same stored artifacts
to PyPI; that would remove the temporary `master` freeze between two dispatches.
Keep the production `pypi` step behind explicit approval either way. Automation
changes how the action is invoked, not the permanence or authority of
publication.

#### Preferred deterministic automation contract

The two-dispatch release runbook above is the current runnable procedure. Keep
using it until the `pypi` GitHub environment has a required reviewer and the
workflow implements the state machine below. Branch restriction and disabled
administrator bypass are valuable, but they do not create a pause by
themselves; changing to automatic promotion before adding a reviewer would make
publication less safe.

The preferred future workflow is a single run whose complete release identity
is the pair `(provider_version, source_sha)`. It should accept those two values
as required inputs, plus a production-intent input that defaults to false, and
enforce all of the following in CI rather than relying on an operator or agent
to remember them:

1. Assert `github.sha == source_sha`; require a clean full-length hexadecimal
   SHA rather than a branch name or abbreviated revision.
2. Assert `provider_version` matches `pyproject.toml`, runtime `__version__`, and
   the root package entry in `uv.lock` exactly.
3. Build once. Produce wheel, sdist, and a JSON manifest containing version,
   source SHA, filenames, byte sizes, and SHA-256 digests. Expose the digests as
   build-job outputs as well as storing the manifest with the distributions, so
   a downloaded artifact cannot validate itself.
4. Publish that artifact to TestPyPI from the minimal `testpypi` OIDC job.
   TestPyPI retry may use skip-existing semantics, but only the following
   verification job decides whether reuse is safe.
5. Poll TestPyPI's version JSON to a fixed deadline. An absent version is an
   indexing delay and may be retried. Once present, any unexpected filename,
   size, or digest is a permanent mismatch and fails immediately. Download the
   exact advertised wheel, verify its digest, install it with dependencies from
   real PyPI—not TestPyPI—and run the isolated-cache provider status smoke.
6. If production intent is false, stop successfully after TestPyPI verification.
   If true, wait at the `pypi` environment for its required reviewer. The
   environment approval in GitHub's UI is the irreversible-action boundary;
   CLI dispatch confirmation is only defense in depth.
7. After approval, download the same run artifact. Before requesting an OIDC
   token upload, compare its files against the independent build-job digest
   outputs. Then publish through the minimal `pypi` job.
8. Poll real PyPI with the same absent-versus-mismatch rules, repeat the isolated
   install/status smoke, and verify wheel and sdist attestations against
   `https://github.com/11xx/muzaiten`.

Use one global concurrency group for provider releases so candidates cannot
interleave. Retain artifacts for seven days and require approval within six;
after that, cancel and create a new provider version instead of approving a run
whose artifact may expire. A transient verification failure may retry only when
the already-published TestPyPI files match the manifest exactly. Never enable
skip-existing behavior for production PyPI.

For both a human and an agent, the preferred operator surface is deliberately
small:

```sh
gh auth status
gh workflow run publish-features-clap.yml \
  --repo 11xx/muzaiten --ref master \
  -f provider_version="$version" \
  -f source_sha="$sha" \
  -f publish_pypi=false
gh run watch --repo 11xx/muzaiten --compact --exit-status <run-id>
gh run view --repo 11xx/muzaiten <run-id> \
  --json headSha,status,conclusion,url
```

Those inputs describe the target contract, not the current workflow CLI. Do not
use them until the single-run workflow lands. At that point, an agent should run
deterministic commands and consume the workflow's compact JSON summary; it
should not scrape prose logs, reinterpret release policy, browse for package
state that the index JSON APIs expose, or regenerate the checklist from model
memory. Set `publish_pypi=true` only when intentionally starting a
production-capable run. Production intent may be supplied through the CLI, but
final approval must remain in the protected GitHub environment.

Recovery is version-based, never mutation-based. If TestPyPI contains files
whose hashes differ from the manifest, or a production upload partially
succeeds, stop and diagnose before choosing the next same-day version
iteration. Do not delete, overwrite, or reuse published filenames. These rules
make the workflow deterministic enough for unattended verification while
keeping the one irreversible decision visibly human.

See GitHub's [manual workflow documentation](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)
and PyPI's [Trusted Publishing security model](https://docs.pypi.org/trusted-publishers/security-model/).
The core AUR packages do not list `muzaiten-features-clap` while that package is
unavailable from Arch/AUR. Advertising an unresolved optional dependency makes
the package page imply an installation route that does not exist. Restore it in
the same publication slice that creates the real provider package; until then,
the supported provider is the PyPI tool installed with uv. The deterministic
distro gate and eventual package order are recorded in
`packaging/features-clap/README.md`.

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
