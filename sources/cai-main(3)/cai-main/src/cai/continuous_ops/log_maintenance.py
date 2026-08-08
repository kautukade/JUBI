"""Log retention for continuous ops (defaults: full ≤7d, gzip until ≤15d, then delete)."""

from __future__ import annotations

import gzip
import os
import shutil
import sys
import time
from pathlib import Path


def maintain_log_tree(log_root: Path, *, now: float | None = None) -> None:
    """Apply retention under *log_root* (expects ``full/`` subtree with iteration files).

    Tunable via ``CAI_COPS_LOG_FULL_DAYS`` and ``CAI_COPS_LOG_DELETE_AFTER_DAYS`` (set by the
    generated worker script). Files newer than *full* days stay uncompressed; between *full*
    and *delete* they are gzip-compressed; older than *delete* they are removed.
    """
    t0 = now if now is not None else time.time()
    try:
        full_days = max(1, int(os.getenv("CAI_COPS_LOG_FULL_DAYS", "7")))
    except ValueError:
        full_days = 7
    try:
        delete_after = max(full_days + 1, int(os.getenv("CAI_COPS_LOG_DELETE_AFTER_DAYS", "15")))
    except ValueError:
        delete_after = max(full_days + 1, 15)

    full_dir = log_root / "full"
    if not full_dir.is_dir():
        return
    sec = 86400.0
    for path in list(full_dir.rglob("*")):
        if not path.is_file():
            continue
        if path.name.endswith(".tmp"):
            continue
        age_days = (t0 - path.stat().st_mtime) / sec
        if age_days > delete_after:
            path.unlink(missing_ok=True)
        elif age_days > full_days and not path.name.endswith(".gz"):
            gz = path.with_name(path.name + ".gz")
            with path.open("rb") as fin, gzip.open(gz, "wb", compresslevel=6) as fout:
                shutil.copyfileobj(fin, fout)
            path.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    if not argv or len(argv) < 2:
        print("usage: python -m cai.continuous_ops.log_maintenance <LOG_ROOT>", file=sys.stderr)
        return 2
    maintain_log_tree(Path(argv[1]).expanduser().resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
