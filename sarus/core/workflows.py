from __future__ import annotations
from contextlib import closing
import json, sqlite3, threading, time, uuid
from pathlib import Path

class WorkflowScheduler:
    def __init__(self, db: Path, runner):
        self.db = db; self.runner = runner; self.stop_evt = threading.Event(); self.thread = None
        with closing(sqlite3.connect(db)) as c:
            c.execute("CREATE TABLE IF NOT EXISTS automations(id TEXT PRIMARY KEY,name TEXT,prompt TEXT,interval_seconds INTEGER,enabled INTEGER,last_run REAL,next_run REAL,metadata TEXT)")

    def add(self, name, prompt, interval_seconds, enabled=True, metadata=None):
        aid = str(uuid.uuid4()); now = time.time(); interval = max(60, int(interval_seconds)); nxt = now + interval
        with closing(sqlite3.connect(self.db)) as c:
            c.execute("INSERT INTO automations VALUES(?,?,?,?,?,?,?,?)", (aid, name, prompt, interval, 1 if enabled else 0, 0, nxt, json.dumps(metadata or {})))
        return {'id': aid, 'name': name, 'next_run': nxt}

    def list(self):
        with closing(sqlite3.connect(self.db)) as c:
            rows = c.execute("SELECT id,name,prompt,interval_seconds,enabled,last_run,next_run,metadata FROM automations ORDER BY name").fetchall()
        return [{'id': x[0], 'name': x[1], 'prompt': x[2], 'interval_seconds': x[3], 'enabled': bool(x[4]), 'last_run': x[5], 'next_run': x[6], 'metadata': json.loads(x[7] or '{}')} for x in rows]

    def set_enabled(self, aid, enabled):
        with closing(sqlite3.connect(self.db)) as c:
            c.execute("UPDATE automations SET enabled=? WHERE id=?", (1 if enabled else 0, aid))

    def tick(self):
        now = time.time()
        for a in self.list():
            if a['enabled'] and a['next_run'] <= now:
                try: self.runner(a['prompt'], source='automation')
                finally:
                    with closing(sqlite3.connect(self.db)) as c:
                        c.execute("UPDATE automations SET last_run=?,next_run=? WHERE id=?", (now, now + a['interval_seconds'], a['id']))

    def start(self):
        if self.thread and self.thread.is_alive(): return
        def loop():
            while not self.stop_evt.wait(20):
                try: self.tick()
                except Exception: pass
        self.thread = threading.Thread(target=loop, name='sarus-scheduler', daemon=True); self.thread.start()
