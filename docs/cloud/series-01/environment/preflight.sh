#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
import json, subprocess, tempfile
from pathlib import Path

def version(command):
    return subprocess.check_output(command, text=True).strip()

qt = ['Qt6Concurrent', 'Qt6Core', 'Qt6DBus', 'Qt6Gui', 'Qt6Multimedia', 'Qt6Network', 'Qt6Widgets', 'Qt6Sql', 'Qt6Test']
libs = ['taglib', 'libzstd', 'libchromaprint', 'gstreamer-1.0', 'gstreamer-audio-1.0', 'gstreamer-pbutils-1.0', 'glib-2.0', 'gobject-2.0']
versions = {name: version(['pkg-config', '--modversion', name]) for name in qt + libs}
assert tuple(map(int, versions['Qt6Core'].split('.')[:2])) >= (6, 11), 'Qt 6.11 or later is required'
assert int(versions['taglib'].split('.')[0]) >= 2, 'TagLib 2 or later is required'
cmake = version(['cmake', '--version']).splitlines()[0]
assert tuple(map(int, cmake.split()[-1].split('.')[:2])) >= (4, 0), 'CMake 4 or later is required'
subprocess.run(['g++', '-std=c++26', '-x', 'c++', '-fsyntax-only', '-'], input='#include <functional>\nint main() { auto f = [] { return 1; }; std::function_ref<int()> r(f); return r() - 1; }\n', text=True, check=True)
with tempfile.TemporaryDirectory(prefix='muzaiten-sdk-preflight-') as tmp:
    root = Path(tmp)
    (root / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 4.0)\n'
        'project(sdk_probe LANGUAGES CXX)\n'
        'find_package(Qt6 6.11 REQUIRED COMPONENTS Core)\n'
        'add_executable(sdk_probe main.cpp)\n'
        'set_target_properties(sdk_probe PROPERTIES CXX_STANDARD 26)\n'
        'target_link_libraries(sdk_probe PRIVATE Qt6::Core)\n')
    (root / 'main.cpp').write_text('#include <QString>\nint main() { return QString("sdk").size() != 3; }\n')
    subprocess.run(['cmake', '-S', tmp, '-B', str(root / 'build'), '-G', 'Ninja'], stdout=subprocess.DEVNULL, check=True)
    subprocess.run(['cmake', '--build', str(root / 'build')], stdout=subprocess.DEVNULL, check=True)
    subprocess.run([str(root / 'build/sdk_probe')], check=True)
for tool in ['pipewire', 'pipewire-pulse', 'wireplumber', 'pactl', 'parec', 'pw-dump', 'pw-metadata', 'ffmpeg', 'git', 'ninja']:
    subprocess.run(['bash', '-c', 'command -v "$1" >/dev/null', 'preflight', tool], check=True)
for element in ['fakesink', 'pulsesink', 'pipewiresink', 'audiotestsrc', 'avdec_aac', 'mpg123audiodec']:
    subprocess.run(['gst-inspect-1.0', element], stdout=subprocess.DEVNULL, check=True)
print(json.dumps({'schema': 'muzaiten-cloud-toolchain/1', 'compiler': version(['g++', '-dumpfullversion']), 'cmake': cmake, 'libraries': versions, 'function_ref': 'verified', 'qt_compile_link': 'verified', 'native_audio_tools': 'available', 'gstreamer_elements': 'available'}, indent=2))
PY
