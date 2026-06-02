#!/usr/bin/env bash
#
# Build an optimized muzaiten binary and package it as a distributable tarball
# (used for Codeberg release artifacts and the muzaiten-bin AUR package).
#
# Last.fm credentials are injected here at build time and never committed.
# Provide them via the environment or a local .env file:
#
#     MUZAITEN_LASTFM_API_KEY=...  MUZAITEN_LASTFM_SHARED_SECRET=...  ./packaging/build-release.sh
#
# An empty value simply leaves Last.fm inactive until the user configures their
# own key in the app. See docs/distribution.md for the key-embedding threat
# model — the values are compile-time XOR-obfuscated but recoverable, so only
# the maintainer-built -bin/release artifacts should carry real keys.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# Load .env (KEY=VALUE lines) if present, without echoing secrets.
if [[ -f .env ]]; then
    set -a
    # shellcheck disable=SC1091
    . ./.env
    set +a
fi

BUILD_DIR="${BUILD_DIR:-build-release}"
PREFIX="${PREFIX:-/usr}"
JOBS="${JOBS:-$(nproc)}"
: "${MUZAITEN_LASTFM_API_KEY:=}"
: "${MUZAITEN_LASTFM_SHARED_SECRET:=}"

if [[ -n "$MUZAITEN_LASTFM_API_KEY" ]]; then
    echo ">> Embedding a Last.fm API key (obfuscated)."
else
    echo ">> No Last.fm key provided; the build will ship without default credentials."
fi

echo ">> Configuring release build in '$BUILD_DIR'"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DBUILD_TESTING=OFF \
    -DMUZAITEN_LASTFM_API_KEY="$MUZAITEN_LASTFM_API_KEY" \
    -DMUZAITEN_LASTFM_SHARED_SECRET="$MUZAITEN_LASTFM_SHARED_SECRET"

echo ">> Building"
cmake --build "$BUILD_DIR" -j "$JOBS"

# Version derived the same way as cmake/MuzaitenVersion.cmake.
version="$(git show -s --format=%cd --date=format:%Y.%m.%d.%H%M%S HEAD).g$(git rev-parse --short HEAD)"
arch="$(uname -m)"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

DESTDIR="$stage" cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null
# Strip symbols: smaller, and removes the obfuscated-blob symbol names.
strip --strip-unneeded "$stage/$PREFIX/bin/muzaiten"
install -Dm644 UNLICENSE "$stage/$PREFIX/share/licenses/muzaiten/UNLICENSE"

mkdir -p dist
tarball="dist/muzaiten-${version}-${arch}.tar.zst"
tar --zstd -C "$stage" -cf "$tarball" .
( cd dist && sha256sum "$(basename "$tarball")" > "$(basename "$tarball").sha256" )

echo ">> Wrote $tarball"
echo ">> Wrote ${tarball}.sha256"
echo ">> Contents:"
tar -tf "$tarball" | sed 's/^/     /'
