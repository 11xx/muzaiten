#!/usr/bin/env python3
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parent
files = ['shell.nix', 'preflight.sh', 'cloud-env', 'cloud-test', 'capture-env.py', 'setup.sh']
lines = ['#!/usr/bin/env bash', 'set -euo pipefail', 'setup_dir=/opt/muzaiten-cloud-setup', 'mkdir -p "$setup_dir"']
for name in files:
    body = (root / name).read_text()
    delimiter = 'SDK_' + hashlib.sha256(body.encode()).hexdigest().upper()
    assert delimiter not in body.splitlines()
    lines += [f'cat > "$setup_dir/{name}" <<\'{delimiter}\'', body.rstrip('\n'), delimiter]
lines.append('bash "$setup_dir/setup.sh" --cloud-setup "${MUZAITEN_CLOUD_CHECKOUT:-/workspace/muzaiten}"')
(root / 'cloud-init.sh').write_text('\n'.join(lines) + '\n')
