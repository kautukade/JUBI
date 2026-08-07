from __future__ import annotations
from contextlib import closing
import json, sqlite3, time, uuid
from pathlib import Path

class MemoryStore:
    def __init__(self, db: Path):
        self.db = db
        db.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(db)) as c:
            c.execute("CREATE TABLE IF NOT EXISTS memories(id TEXT PRIMARY KEY, ts REAL, namespace TEXT, title TEXT, content TEXT, metadata TEXT)")
            try:
                c.execute("CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(id UNINDEXED,title,content,namespace)")
                self.fts = True
            except sqlite3.OperationalError:
                self.fts = False

    def add(self, content: str, title: str = '', namespace: str = 'general', metadata: dict | None = None):
        mid = str(uuid.uuid4())
        ts = time.time()
        md = json.dumps(metadata or {}, ensure_ascii=False)
        with closing(sqlite3.connect(self.db)) as c:
            c.execute("INSERT INTO memories VALUES(?,?,?,?,?,?)", (mid, ts, namespace, title, content, md))
            if self.fts:
                c.execute("INSERT INTO memories_fts(id,title,content,namespace) VALUES(?,?,?,?)", (mid, title, content, namespace))
        return {'id': mid, 'ts': ts, 'namespace': namespace, 'title': title, 'content': content, 'metadata': metadata or {}}

    def search(self, q: str = '', namespace: str | None = None, limit: int = 25):
        rows = []
        with closing(sqlite3.connect(self.db)) as c:
            if q and self.fts:
                try:
                    sql = "SELECT m.id,m.ts,m.namespace,m.title,m.content,m.metadata FROM memories_fts f JOIN memories m ON m.id=f.id WHERE memories_fts MATCH ?"
                    args = [q]
                    if namespace:
                        sql += " AND m.namespace=?"
                        args.append(namespace)
                    sql += " ORDER BY rank LIMIT ?"
                    args.append(limit)
                    rows = c.execute(sql, args).fetchall()
                except sqlite3.OperationalError:
                    rows = []
            if not rows:
                sql = "SELECT id,ts,namespace,title,content,metadata FROM memories WHERE 1=1"
                args = []
                if namespace:
                    sql += " AND namespace=?"
                    args.append(namespace)
                if q:
                    sql += " AND (title LIKE ? OR content LIKE ?)"
                    args.extend([f'%{q}%', f'%{q}%'])
                sql += " ORDER BY ts DESC LIMIT ?"
                args.append(limit)
                rows = c.execute(sql, args).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'namespace': r[2], 'title': r[3], 'content': r[4], 'metadata': json.loads(r[5] or '{}')}
            for r in rows
        ]
