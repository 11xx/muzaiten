#!/usr/bin/env bash
set -euo pipefail
setup_dir=/opt/muzaiten-cloud-setup
mkdir -p "$setup_dir"
cat > "$setup_dir/shell.nix" <<'SDK_2E47D058460F30A36EBB843A247410E7A973974F381C1B53B8AE4F76BF86280F'
let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/e7439b6b14ad3cc35d05608ebca9bce01a25f5f8.tar.gz";
    sha256 = "19y4py973w62z4qb7gzn0chjs67ihxy8rbjb7g71iyk8yz7z5k8z";
  }) { system = "x86_64-linux"; };
  gst = pkgs.gst_all_1;
in
(pkgs.mkShell.override { stdenv = pkgs.gcc16Stdenv; }) {
  nativeBuildInputs = with pkgs; [
    cmake ninja pkg-config git python3 gnumake binutils mold coreutils bash
  ];
  buildInputs = with pkgs; [
    qt6.qtbase qt6.qtmultimedia qt6.qtsvg taglib zstd chromaprint glib
    gst.gstreamer gst.gst-plugins-base gst.gst-plugins-good
    gst.gst-plugins-bad gst.gst-libav pipewire pulseaudio wireplumber
    dbus ffmpeg fontconfig dejavu_fonts
  ];
  QT_QPA_PLATFORM = "offscreen";
  QT_PLUGIN_PATH = pkgs.lib.makeSearchPath "lib/qt-6/plugins" [
    pkgs.qt6.qtbase pkgs.qt6.qtmultimedia pkgs.qt6.qtsvg
  ];
  FONTCONFIG_FILE = "${pkgs.fontconfig.out}/etc/fonts/fonts.conf";
  FONTCONFIG_PATH = "${pkgs.fontconfig.out}/etc/fonts";
  GST_PLUGIN_SYSTEM_PATH_1_0 = pkgs.lib.makeSearchPath "lib/gstreamer-1.0" [
    gst.gst-plugins-base gst.gst-plugins-good gst.gst-plugins-bad
    gst.gst-libav pkgs.pipewire
  ];
  shellHook = ''
    export GST_REGISTRY="''${MUZAITEN_STATE_ROOT:-$PWD/agent-state/cloud}/cache/gstreamer-registry.bin"
    export CMAKE_BUILD_PARALLEL_LEVEL="''${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
  '';
}
SDK_2E47D058460F30A36EBB843A247410E7A973974F381C1B53B8AE4F76BF86280F
cat > "$setup_dir/preflight.sh" <<'SDK_D1A0DF63E45E9BFF6AE4F3B40630107A6E454ADAD6019F0DA7FD85B4A8899449'
#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
import json, subprocess

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
for tool in ['pipewire', 'pipewire-pulse', 'wireplumber', 'pactl', 'parec', 'pw-dump', 'pw-metadata', 'ffmpeg', 'git', 'ninja']:
    subprocess.run(['bash', '-c', 'command -v "$1" >/dev/null', 'preflight', tool], check=True)
for element in ['fakesink', 'pulsesink', 'pipewiresink', 'audiotestsrc', 'avdec_aac', 'mpg123audiodec']:
    subprocess.run(['gst-inspect-1.0', element], stdout=subprocess.DEVNULL, check=True)
print(json.dumps({'schema': 'muzaiten-cloud-toolchain/1', 'compiler': version(['g++', '-dumpfullversion']), 'cmake': cmake, 'libraries': versions, 'function_ref': 'verified', 'native_audio_tools': 'available', 'gstreamer_elements': 'available'}, indent=2))
PY
SDK_D1A0DF63E45E9BFF6AE4F3B40630107A6E454ADAD6019F0DA7FD85B4A8899449
cat > "$setup_dir/cloud-env" <<'SDK_2233C468584D5A91002C61F1F2D0E06648E9A73296426EEF732154089046C82C'
#!/usr/bin/env bash
set -euo pipefail
sdk_root=/opt/muzaiten-cloud
if [[ ! -f "$sdk_root/activate.sh" ]]; then
    printf '%s\n' 'Cloud toolchain is absent. Configure the environment Setup script before running this command.' >&2
    exit 1
fi
if [[ $# -eq 0 ]]; then
    printf '%s\n' 'Usage: cloud-env COMMAND [ARG...]' >&2
    exit 2
fi
source "$sdk_root/activate.sh"
exec "$@"
SDK_2233C468584D5A91002C61F1F2D0E06648E9A73296426EEF732154089046C82C
cat > "$setup_dir/capture-env.py" <<'SDK_F8B7F2CF2AFA11D9BA38E1C4FD5D58C9A5113F32B8D4104DDCFFB56AD6BEE22C'
#!/usr/bin/env python3
import os
import shlex
import sys
from pathlib import Path

keys = [
    'CC', 'CXX', 'LD', 'AR', 'AS', 'RANLIB', 'STRIP', 'NM', 'OBJCOPY', 'OBJDUMP',
    'CMAKE_PREFIX_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR',
    'NIX_CC', 'NIX_BINTOOLS', 'NIX_HARDENING_ENABLE', 'NIX_PURITY_PKG_CONFIG',
    'NIX_CFLAGS_COMPILE', 'NIX_CFLAGS_LINK', 'NIX_LDFLAGS', 'NIX_ENFORCE_PURITY',
    'NIX_SSL_CERT_FILE', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM',
    'FONTCONFIG_FILE', 'FONTCONFIG_PATH', 'GST_PLUGIN_SYSTEM_PATH_1_0',
    'GI_TYPELIB_PATH', 'XDG_DATA_DIRS', 'GLIBC_LOCALE_PATH',
]
paths = [p for p in os.environ['PATH'].split(':') if p.startswith('/nix/store/')]
paths += ['/usr/local/bin', '/usr/bin', '/bin', '/usr/sbin', '/sbin']
lines = ['#!/usr/bin/env bash', 'export PATH=' + shlex.quote(':'.join(dict.fromkeys(paths)))]
for key in keys:
    if key in os.environ:
        lines.append('export ' + key + '=' + shlex.quote(os.environ[key]))
lines.append('export GST_REGISTRY="${MUZAITEN_STATE_ROOT:-$PWD/agent-state/cloud}/cache/gstreamer-registry.bin"')
lines.append('export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"')
Path(sys.argv[1]).write_text('\n'.join(lines) + '\n')
SDK_F8B7F2CF2AFA11D9BA38E1C4FD5D58C9A5113F32B8D4104DDCFFB56AD6BEE22C
cat > "$setup_dir/setup.sh" <<'SDK_0342C1C45E48535DF54350FE21CE550A628D788F2CAAC6587628D8E71701390A'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} != --cloud-setup || $(id -u) != 0 ]]; then
    printf '%s\n' 'Run this script as root in the cloud environment Setup script phase, with --cloud-setup.' >&2
    exit 2
fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    printf '%s\n' 'This pinned toolchain requires x86_64 Linux.' >&2
    exit 2
fi
for tool in curl tar xz sha256sum python3; do
    command -v "$tool" >/dev/null || { printf 'Required bootstrap tool is absent: %s\n' "$tool" >&2; exit 1; }
done
sdk_root=/opt/muzaiten-cloud
sdk_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
for input in shell.nix preflight.sh cloud-env capture-env.py; do
    [[ -f "$sdk_source/$input" ]] || { printf 'Missing toolchain input: %s\n' "$input" >&2; exit 1; }
done
mkdir -p "$sdk_root/bin"
export NIX_CONFIG=$'build-users-group =\nmax-jobs = 0\nbuilders =\nrequire-sigs = true\nfallback = false'
export NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
if [[ ! -x "$sdk_root/bin/nix-shell" ]]; then
    nix_system_shell=$(command -v nix-shell || true)
    if [[ -n "$nix_system_shell" ]]; then
        "$nix_system_shell" --version | python3 -c '
import re, sys
match = re.fullmatch(r"nix-shell \(Nix\) (\d+)\.(\d+)\.\d+", sys.stdin.read().strip())
if not match or tuple(map(int, match.groups())) < (2, 34):
    sys.exit("The existing Nix client must be version 2.34 or newer.")
'
        for tool in nix-shell nix-instantiate; do
            nix_tool=$(command -v "$tool") || { printf 'Existing Nix installation lacks %s.\n' "$tool" >&2; exit 1; }
            ln -sfn "$(readlink -f "$nix_tool")" "$sdk_root/bin/$tool"
        done
        printf '%s\n' 'Using the existing Nix installation for signed SDK provisioning.'
    elif [[ -e /nix && ! -f "$sdk_root/owned-store" && -n $(find /nix -mindepth 1 -maxdepth 1 -print -quit) ]]; then
        printf '%s\n' 'An existing Nix store is not owned by this setup. Preserve it and configure a separate environment.' >&2
        exit 1
    else
        sdk_tmp=$(mktemp -d "$sdk_root/bootstrap.XXXXXX")
        trap 'rm -rf -- "$sdk_tmp"' EXIT
        curl -fsSL --retry 2 --max-time 120 \
            https://releases.nixos.org/nix/nix-2.34.8/nix-2.34.8-x86_64-linux.tar.xz \
            -o "$sdk_tmp/nix.tar.xz"
        printf '%s  %s\n' 2c2e146b80834fe0ca201b51deeb939405b4f18e8d2071bf80b10f8123c50464 \
            "$sdk_tmp/nix.tar.xz" | sha256sum -c -
        tar -xJf "$sdk_tmp/nix.tar.xz" -C "$sdk_tmp"
        mkdir -p /nix/store
        : > "$sdk_root/owned-store"
        cp -a "$sdk_tmp/nix-2.34.8-x86_64-linux/store/"* /nix/store/
        nix_candidates=()
        for candidate in /nix/store/*-nix-2.34.8; do
            [[ -x "$candidate/bin/nix-store" ]] && nix_candidates+=("$candidate")
        done
        [[ ${#nix_candidates[@]} -eq 1 ]] || { printf '%s\n' 'Cannot identify the verified Nix bootstrap executable.' >&2; exit 1; }
        nix_prefix=${nix_candidates[0]}
        "$nix_prefix/bin/nix-store" --load-db < "$sdk_tmp/nix-2.34.8-x86_64-linux/.reginfo"
        for tool in nix-shell nix-instantiate; do
            ln -sfn "$nix_prefix/bin/$tool" "$sdk_root/bin/$tool"
        done
    fi
fi
install -m 644 "$sdk_source/shell.nix" "$sdk_root/shell.nix"
install -m 755 "$sdk_source/preflight.sh" "$sdk_root/preflight.sh"
install -m 755 "$sdk_source/cloud-env" "$sdk_root/cloud-env"
install -m 755 "$sdk_source/capture-env.py" "$sdk_root/capture-env.py"
source_path=$(
    "$sdk_root/bin/nix-instantiate" --eval --json --expr '
        builtins.fetchTarball {
          url = "https://github.com/NixOS/nixpkgs/archive/e7439b6b14ad3cc35d05608ebca9bce01a25f5f8.tar.gz";
          sha256 = "19y4py973w62z4qb7gzn0chjs67ihxy8rbjb7g71iyk8yz7z5k8z";
        }' | python3 -c 'import json,sys; print(json.load(sys.stdin))'
)
ln -sfn "$source_path" "$sdk_root/nixpkgs"
export NIX_PATH="nixpkgs=$sdk_root/nixpkgs"
"$sdk_root/bin/nix-shell" "$sdk_root/shell.nix" --pure --run \
    "bash '$sdk_root/preflight.sh'" > "$sdk_root/toolchain.json"
python3 -m json.tool "$sdk_root/toolchain.json" >/dev/null
"$sdk_root/bin/nix-shell" "$sdk_root/shell.nix" --pure --run \
    "python3 '$sdk_root/capture-env.py' '$sdk_root/activate.sh'"
install -m 755 "$sdk_root/cloud-env" /usr/local/bin/cloud-env
printf '%s\n' 'Pinned muzaiten toolchain is ready. Run build and runtime commands through cloud-env.'
SDK_0342C1C45E48535DF54350FE21CE550A628D788F2CAAC6587628D8E71701390A
bash "$setup_dir/setup.sh" --cloud-setup
