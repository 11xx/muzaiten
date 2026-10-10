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
for input in shell.nix preflight.sh cloud-env cloud-test capture-env.py; do
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
install -m 755 "$sdk_source/cloud-test" "$sdk_root/cloud-test"
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
install -m 755 "$sdk_root/cloud-test" /usr/local/bin/cloud-test
if [[ -d .git && -f CMakeLists.txt && -f tools/version.py ]]; then
    test_uid=$(id -u ubuntu)
    test_gid=$(id -g ubuntu)
    [[ "$test_uid" != 0 ]] || { printf '%s\n' 'A normal ubuntu account is required for filesystem permission tests.' >&2; exit 1; }
    for directory in build build/tests build/Testing agent-state agent-state/cloud-tests agent-state/cloud-tests/runtime; do
        [[ ! -L "$directory" ]] || { printf 'Test directory must not be a symlink: %s\n' "$directory" >&2; exit 1; }
        install -d -o "$test_uid" -g "$test_gid" "$directory"
    done
    chmod 700 agent-state/cloud-tests/runtime
fi
printf '%s\n' 'Pinned muzaiten toolchain is ready. Run build and runtime commands through cloud-env.'
