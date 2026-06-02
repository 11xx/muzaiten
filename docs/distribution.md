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

## Package naming (recommendation)

Reserve all three names — `muzaiten`, `muzaiten-bin`, `muzaiten-git` — but
**publish only `muzaiten-bin` and `muzaiten-git`** for now:

- **`muzaiten-git`** — builds the latest commit from source. Best for people who
  want to build it themselves; it ships **no** embedded Last.fm key (users add
  their own in-app).
- **`muzaiten-bin`** — the maintainer-built prebuilt binary, with the embedded
  Last.fm key. Best for people who do not want to compile a Qt app.
- **`muzaiten`** (no suffix) — by AUR convention this is a *versioned source
  release* (a build of a tagged tarball). muzaiten currently ships rolling,
  date-based versions with no release tags, so this name has no distinct meaning
  yet. Adopt it only once you start cutting tagged source releases; until then it
  would just duplicate `-git`.

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

- `muzaiten-<version>-<arch>.tar.zst` — a prefixed tree (`usr/bin/muzaiten`
  plus the desktop entry, icon, metainfo, and license), and
- `muzaiten-<version>-<arch>.tar.zst.sha256`.

`<version>` is the date-based `YYYY.MM.DD.HHMMSS.g<sha>` derived from `HEAD`.
Upload both files to a Codeberg release whose tag is exactly `<version>`.

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

Each package is a self-contained directory under `packaging/aur/`. To publish or
update one:

```sh
cd packaging/aur/muzaiten-git      # or muzaiten-bin
makepkg --printsrcinfo > .SRCINFO  # regenerate after any PKGBUILD edit
git clone ssh://aur@aur.archlinux.org/muzaiten-git.git aur-repo
cp PKGBUILD .SRCINFO aur-repo/
cd aur-repo && git commit -am "update" && git push
```

For `muzaiten-bin`, before pushing a new release also:

1. set `pkgver` to the release `<version>`, and
2. replace `sha256sums=('SKIP')` with the checksum from
   `dist/muzaiten-<version>-<arch>.tar.zst.sha256`.

Validate locally with `namcap PKGBUILD` and a clean `makepkg` build.
