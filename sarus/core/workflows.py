from __future__ import annotations

import json
import threading
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


class WorkflowScheduler:
    def __init__(self, db: Path, runner, event_bus=None):
        self.db = db
        self.runner = runner
        self.event_bus = event_bus
        self.stop_evt = threading.Event()
        self.thread: threading.Thread | None = None
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS automations("
                "id TEXT PRIMARY KEY,name TEXT,prompt TEXT,interval_seconds INTEGER,enabled INTEGER,"
                "last_run REAL,next_run REAL,metadata TEXT)"
            )

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is not None:
            try:
                self.event_bus.emit(kind, payload)
            except Exception:
                pass

    def add(self, name, prompt, interval_seconds, enabled=True, metadata=None):
        aid = str(uuid.uuid4())
        now = time.time()
        interval = max(60, int(interval_seconds))
        nxt = now + interval
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO automations VALUES(?,?,?,?,?,?,?,?)",
                (aid, name, prompt, interval, 1 if enabled else 0, 0, nxt, json.dumps(metadata or {})),
            )
        self._emit('AUTOMATION_CREATED', {'automation_id': aid, 'name': name, 'next_run': nxt})
        return {'id': aid, 'name': name, 'next_run': nxt}

    def list(self):
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,name,prompt,interval_seconds,enabled,last_run,next_run,metadata "
                "FROM automations ORDER BY name"
            ).fetchall()
        return [
            {
                'id': x[0],
                'name': x[1],
                'prompt': x[2],
                'interval_seconds': x[3],
                'enabled': bool(x[4]),
                'last_run': x[5],
                'next_run': x[6],
                'metadata': json.loads(x[7] or '{}'),
            }
            for x in rows
        ]

    def set_enabled(self, aid, enabled):
        with transaction(self.db) as c:
            cur = c.execute(
                "UPDATE automations SET enabled=? WHERE id=?",
                (1 if enabled else 0, aid),
            )
        if not cur.rowcount:
            raise KeyError('automation not found')
        self._emit('AUTOMATION_TOGGLED', {'automation_id': aid, 'enabled': bool(enabled)})

    def tick(self):
        now = time.time()
        for a in self.list():
            if not (a['enabled'] and a['next_run'] <= now):
                continue
            self._emit('AUTOMATION_STARTED', {'automation_id': a['id'], 'name': a['name']})
            ok = False
            error = None
            try:
                self.runner(a['prompt'], source='automation')
                ok = True
            except Exception as exc:
                error = str(exc)
                self._emit(
                    'AUTOMATION_FAILED',
                    {'automation_id': a['id'], 'name': a['name'], 'error': error[:1000]},
                )
            finally:
                finished = time.time()
                with transaction(self.db) as c:
                    c.execute(
                        "UPDATE automations SET last_run=?,next_run=? WHERE id=?",
                        (finished, finished + a['interval_seconds'], a['id']),
                    )
            if ok:
                self._emit('AUTOMATION_FINISHED', {'automation_id': a['id'], 'name': a['name']})
            # Preserve the old behavior: one scheduler tick may process all due
            # automations, but an individual failure cannot kill the thread.

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stop_evt.clear()

        def loop():
            while not self.stop_evt.wait(20):
                try:
                    self.tick()
                except Exception as exc:
                    self._emit('AUTOMATION_SCHEDULER_ERROR', {'error': str(exc)[:1000]})

        self.thread = threading.Thread(target=loop, name='jubi-scheduler', daemon=True)
        self.thread.start()

    def stop(self, timeout: float = 5.0):
        self.stop_evt.set()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=max(0.1, float(timeout)))
        self.thread = None
