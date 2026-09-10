"""Recoverable installation of a verified model directory."""

import os
from pathlib import Path
import shutil
import tempfile


def install_directory(staging: Path, target: Path) -> None:
    if not target.exists():
        os.replace(staging, target)
        return
    backup_root = Path(tempfile.mkdtemp(prefix=f".{target.name}-backup-", dir=target.parent))
    backup = backup_root / "previous"
    try:
        os.replace(target, backup)
        try:
            os.replace(staging, target)
        except BaseException:
            os.replace(backup, target)
            raise
    finally:
        # If restoration itself fails, retain the backup for recovery.
        if not backup.exists():
            backup_root.rmdir()
    if backup.exists():
        shutil.rmtree(backup_root)
