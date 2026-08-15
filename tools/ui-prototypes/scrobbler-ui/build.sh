#!/usr/bin/env bash
# Build and run the scrobbling UI prototype. It links Qt Widgets directly and
# has no moc step, so a change is on screen in about a second.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Binaries land in the repo's gitignored build tree, never beside the source.
out_dir="$(cd "${here}/../../.." && pwd)/build/ui-prototypes"
out="${out_dir}/scrobbler-ui"
mkdir -p "${out_dir}"

g++ -std=c++26 -O0 -g -Wall -Wextra -fPIC \
    $(pkg-config --cflags Qt6Widgets) \
    "${here}/main.cpp" -o "${out}" \
    $(pkg-config --libs Qt6Widgets)

case "${1-}" in
--build-only) ;;
--shot)
    SCROBBLER_UI_SHOT_DIR="${2:?usage: build.sh --shot <dir>}" QT_QPA_PLATFORM=offscreen "${out}"
    ;;
*) exec "${out}" ;;
esac
