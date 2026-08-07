from __future__ import annotations
from contextlib import closing
import hashlib, json, sqlite3, time, uuid, threading
from pathlib import Path

class ReceiptStore:
    """Append-only hash-chained execution evidence store."""
    def __init__(self, db: Path):
        self.db = db
        self.lock = threading.Lock()
        db.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(db)) as c:
            c.execute("CREATE TABLE IF NOT EXISTS receipts(id TEXT PRIMARY KEY,ts REAL,task_id TEXT,step_id TEXT,source TEXT,status TEXT,payload TEXT,prev_hash TEXT,hash TEXT)")

    @staticmethod
    def _digest(rid, ts, task_id, step_id, source, status, prev, blob):
        return hashlib.sha256(f'{rid}|{ts}|{task_id}|{step_id}|{source}|{status}|{prev}|{blob}'.encode()).hexdigest()

    def create(self, task_id: str, step_id: str, source: str, status: str, payload: dict):
        with self.lock, closing(sqlite3.connect(self.db)) as c:
            row = c.execute("SELECT hash FROM receipts ORDER BY ts DESC,id DESC LIMIT 1").fetchone()
            prev = row[0] if row else ''
            rid = str(uuid.uuid4()); ts = time.time()
            blob = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            digest = self._digest(rid, ts, task_id, step_id, source, status, prev, blob)
            c.execute("INSERT INTO receipts VALUES(?,?,?,?,?,?,?,?,?)", (rid, ts, task_id, step_id, source, status, blob, prev, digest))
        return {'id': rid, 'ts': ts, 'task_id': task_id, 'step_id': step_id, 'source': source, 'status': status, 'payload': payload, 'prev_hash': prev, 'hash': digest}

    def recent(self, limit=100):
        with closing(sqlite3.connect(self.db)) as c:
            rows = c.execute("SELECT id,ts,task_id,step_id,source,status,payload,prev_hash,hash FROM receipts ORDER BY ts DESC,id DESC LIMIT ?", (limit,)).fetchall()
        return [{'id': r[0], 'ts': r[1], 'task_id': r[2], 'step_id': r[3], 'source': r[4], 'status': r[5], 'payload': json.loads(r[6]), 'payload_json':r[6], 'prev_hash': r[7], 'hash': r[8]} for r in rows]

    def verify_chain(self):
        rows = list(reversed(self.recent(100000))); prev = ''; errors = []
        for r in rows:
            link_ok = r['prev_hash'] == prev
            digest = self._digest(r['id'], r['ts'], r['task_id'], r['step_id'], r['source'], r['status'], r['prev_hash'], r['payload_json'])
            hash_ok = digest == r['hash']
            if not (link_ok and hash_ok): errors.append({'id':r['id'],'link_ok':link_ok,'hash_ok':hash_ok})
            prev = r['hash']
        return {'ok': not errors, 'count': len(rows), 'errors': errors[:20]}
