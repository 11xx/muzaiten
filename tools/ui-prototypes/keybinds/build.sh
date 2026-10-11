#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="$(cd "${here}/../../.." && pwd)/build/ui-prototypes"
out="${out_dir}/keybinds"
mkdir -p "${out_dir}"
g++ -std=c++26 -O0 -g -Wall -Wextra -fPIC \
    $(pkg-config --cflags Qt6Widgets) "${here}/main.cpp" -o "${out}" \
    $(pkg-config --libs Qt6Widgets)
case "${1-}" in
--build-only) ;;
--shot) PROTOTYPE_SHOT_DIR="${2:?usage: build.sh --shot <dir>}" QT_QPA_PLATFORM=offscreen "${out}" ;;
*) exec "${out}" ;;
esac
