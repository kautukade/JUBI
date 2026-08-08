"""Log retention maintenance for continuous ops."""

import time
from pathlib import Path

from cai.continuous_ops.log_maintenance import maintain_log_tree


def test_maintain_log_tree_deletes_very_old(tmp_path: Path) -> None:
    full = tmp_path / "full"
    full.mkdir(parents=True)
    old = full / "old.log"
    old.write_text("x", encoding="utf-8")
    ts = time.time() - 20 * 86400
    import os

    os.utime(old, (ts, ts))
    maintain_log_tree(tmp_path, now=time.time())
    assert not old.exists()


def test_maintain_log_tree_compresses_week_two(tmp_path: Path) -> None:
    full = tmp_path / "full"
    full.mkdir(parents=True)
    mid = full / "mid.log"
    mid.write_text("hello", encoding="utf-8")
    ts = time.time() - 10 * 86400
    import os

    os.utime(mid, (ts, ts))
    maintain_log_tree(tmp_path, now=time.time())
    assert not mid.exists()
    gz = full / "mid.log.gz"
    assert gz.is_file()
