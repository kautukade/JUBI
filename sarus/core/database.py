from __future__ import annotations

from contextlib import contextmanager
import sqlite3
from pathlib import Path
from typing import Iterator


DEFAULT_BUSY_TIMEOUT_MS = 5000
DEFAULT_CONNECT_TIMEOUT_SECONDS = 15.0


def connect(db: Path | str) -> sqlite3.Connection:
    """Open a consistently configured SQLite connection for SARUS/Jubi state.

    Every caller gets its own connection. WAL + a busy timeout make the existing
    HTTP/scheduler threads much less likely to fail with ``database is locked``
    while still keeping transactions short and explicit.
    """
    path = Path(db)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(path, timeout=DEFAULT_CONNECT_TIMEOUT_SECONDS)
    conn.execute(f"PRAGMA busy_timeout={DEFAULT_BUSY_TIMEOUT_MS}")
    conn.execute("PRAGMA foreign_keys=ON")
    # journal_mode is persistent for the database. SQLite safely returns the
    # current mode when it is already WAL.
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


@contextmanager
def read_connection(db: Path | str) -> Iterator[sqlite3.Connection]:
    conn = connect(db)
    try:
        yield conn
    finally:
        conn.close()


@contextmanager
def transaction(db: Path | str) -> Iterator[sqlite3.Connection]:
    """Commit on success and roll back on failure."""
    conn = connect(db)
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()
