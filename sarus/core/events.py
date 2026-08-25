from __future__ import annotations

import json
import threading
import time
from pathlib import Path

from .database import read_connection, transaction


class EventBus:
    def __init__(self, db: Path):
        self.db = db
        self.lock = threading.Lock()
        self.subscribers = []
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS events("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, kind TEXT, payload TEXT)"
            )

    def emit(self, kind: str, payload: dict):
        row = {'ts': time.time(), 'kind': str(kind), 'payload': payload}
        try:
            encoded = json.dumps(payload, ensure_ascii=False, default=str)
        except Exception:
            encoded = json.dumps({'serialization_error': True, 'repr': repr(payload)[:4000]})
            row['payload'] = json.loads(encoded)

        with self.lock:
            with transaction(self.db) as c:
                c.execute(
                    "INSERT INTO events(ts,kind,payload) VALUES(?,?,?)",
                    (row['ts'], row['kind'], encoded),
                )

        for fn in list(self.subscribers):
            try:
                fn(row)
            except Exception:
                # Event subscribers are optional observers and must not be able
                # to break the primary application flow.
                continue
        return row

    def recent(self, limit=100):
        limit = max(1, min(int(limit), 5000))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,kind,payload FROM events ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'kind': r[2], 'payload': json.loads(r[3])}
            for r in reversed(rows)
        ]
