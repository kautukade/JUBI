from __future__ import annotations
from contextlib import closing
import hashlib, hmac, json, os, secrets, sqlite3, time, uuid, threading
from pathlib import Path


class ReceiptStore:
    """Append-only hash-chained execution evidence store with cryptographic MACs.

    New receipts are authenticated with HMAC-SHA256 using a local key that is
    generated on first run and kept outside the database. Existing legacy rows
    remain readable and are reported as unsigned by verify_chain().
    """

    SIGNATURE_ALGORITHM = 'HMAC-SHA256'

    def __init__(self, db: Path):
        self.db = db
        self.lock = threading.Lock()
        db.parent.mkdir(parents=True, exist_ok=True)
        self.key_path = db.parent / 'receipt-signing.key'
        self._signing_key = self._load_or_create_key()
        self.key_id = hashlib.sha256(self._signing_key).hexdigest()[:16]
        with closing(sqlite3.connect(db)) as c:
            c.execute("CREATE TABLE IF NOT EXISTS receipts(id TEXT PRIMARY KEY,ts REAL,task_id TEXT,step_id TEXT,source TEXT,status TEXT,payload TEXT,prev_hash TEXT,hash TEXT)")
            cols = {r[1] for r in c.execute('PRAGMA table_info(receipts)').fetchall()}
            for name in ('signature_alg', 'key_id', 'signature'):
                if name not in cols:
                    c.execute(f"ALTER TABLE receipts ADD COLUMN {name} TEXT DEFAULT ''")

    def _load_or_create_key(self) -> bytes:
        if self.key_path.exists():
            raw = self.key_path.read_bytes()
            if len(raw) < 32:
                raise RuntimeError('SARUS receipt signing key is invalid or truncated')
            return raw
        key = secrets.token_bytes(32)
        # Exclusive creation prevents two first-run processes from silently
        # replacing each other's signing key.
        try:
            with self.key_path.open('xb') as f:
                f.write(key)
        except FileExistsError:
            key = self.key_path.read_bytes()
        try:
            os.chmod(self.key_path, 0o600)
        except OSError:
            pass
        return key

    @staticmethod
    def _digest(rid, ts, task_id, step_id, source, status, prev, blob):
        return hashlib.sha256(f'{rid}|{ts}|{task_id}|{step_id}|{source}|{status}|{prev}|{blob}'.encode()).hexdigest()

    def _sign(self, digest: str) -> str:
        return hmac.new(self._signing_key, digest.encode('ascii'), hashlib.sha256).hexdigest()

    def create(self, task_id: str, step_id: str, source: str, status: str, payload: dict):
        with self.lock, closing(sqlite3.connect(self.db)) as c:
            row = c.execute("SELECT hash FROM receipts ORDER BY ts DESC,id DESC LIMIT 1").fetchone()
            prev = row[0] if row else ''
            rid = str(uuid.uuid4())
            ts = time.time()
            blob = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            digest = self._digest(rid, ts, task_id, step_id, source, status, prev, blob)
            signature = self._sign(digest)
            c.execute(
                "INSERT INTO receipts(id,ts,task_id,step_id,source,status,payload,prev_hash,hash,signature_alg,key_id,signature) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                (rid, ts, task_id, step_id, source, status, blob, prev, digest, self.SIGNATURE_ALGORITHM, self.key_id, signature),
            )
        return {
            'id': rid,
            'ts': ts,
            'task_id': task_id,
            'step_id': step_id,
            'source': source,
            'status': status,
            'payload': payload,
            'prev_hash': prev,
            'hash': digest,
            'signature': {
                'algorithm': self.SIGNATURE_ALGORITHM,
                'key_id': self.key_id,
                'value': signature,
            },
        }

    def recent(self, limit=100):
        with closing(sqlite3.connect(self.db)) as c:
            rows = c.execute(
                "SELECT id,ts,task_id,step_id,source,status,payload,prev_hash,hash,signature_alg,key_id,signature FROM receipts ORDER BY ts DESC,id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'task_id': r[2], 'step_id': r[3],
                'source': r[4], 'status': r[5], 'payload': json.loads(r[6]),
                'payload_json': r[6], 'prev_hash': r[7], 'hash': r[8],
                'signature': {'algorithm': r[9], 'key_id': r[10], 'value': r[11]} if r[11] else None,
            }
            for r in rows
        ]

    def verify_chain(self):
        rows = list(reversed(self.recent(100000)))
        prev = ''
        errors = []
        unsigned_legacy = 0
        signed_count = 0
        for r in rows:
            link_ok = r['prev_hash'] == prev
            digest = self._digest(r['id'], r['ts'], r['task_id'], r['step_id'], r['source'], r['status'], r['prev_hash'], r['payload_json'])
            hash_ok = hmac.compare_digest(digest, r['hash'])
            signature = r.get('signature')
            if signature and signature.get('value'):
                signed_count += 1
                alg_ok = signature.get('algorithm') == self.SIGNATURE_ALGORITHM
                key_ok = signature.get('key_id') == self.key_id
                sig_ok = alg_ok and key_ok and hmac.compare_digest(signature['value'], self._sign(r['hash']))
            else:
                unsigned_legacy += 1
                sig_ok = True
            if not (link_ok and hash_ok and sig_ok):
                errors.append({'id': r['id'], 'link_ok': link_ok, 'hash_ok': hash_ok, 'signature_ok': sig_ok})
            prev = r['hash']
        return {
            'ok': not errors,
            'count': len(rows),
            'signed_count': signed_count,
            'unsigned_legacy': unsigned_legacy,
            'all_new_receipts_signed': signed_count + unsigned_legacy == len(rows),
            'key_id': self.key_id,
            'algorithm': self.SIGNATURE_ALGORITHM,
            'errors': errors[:20],
        }
