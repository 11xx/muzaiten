#!/usr/bin/env python3
import os
import shlex
import sys
from pathlib import Path

keys = [
    'CC', 'CXX', 'LD', 'AR', 'AS', 'RANLIB', 'STRIP', 'NM', 'OBJCOPY', 'OBJDUMP',
    'CMAKE_PREFIX_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR',
    'NIX_CC', 'NIX_BINTOOLS', 'NIX_HARDENING_ENABLE', 'NIX_PURITY_PKG_CONFIG',
    'NIX_CC_WRAPPER_TARGET_HOST_x86_64_unknown_linux_gnu',
    'NIX_BINTOOLS_WRAPPER_TARGET_HOST_x86_64_unknown_linux_gnu',
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
