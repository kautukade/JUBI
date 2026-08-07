from __future__ import annotations
from contextlib import closing
import sqlite3, threading, time, json
from pathlib import Path

class EventBus:
    def __init__(self, db: Path):
        self.db=db; self.lock=threading.Lock(); self.subscribers=[]
        db.parent.mkdir(parents=True,exist_ok=True)
        with closing(sqlite3.connect(db)) as c:
            c.execute("CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, kind TEXT, payload TEXT)")
    def emit(self, kind:str, payload:dict):
        row={'ts':time.time(),'kind':kind,'payload':payload}
        with self.lock, closing(sqlite3.connect(self.db)) as c:
            c.execute("INSERT INTO events(ts,kind,payload) VALUES(?,?,?)",(row['ts'],kind,json.dumps(payload,ensure_ascii=False)))
        for fn in list(self.subscribers):
            try: fn(row)
            except Exception: pass
        return row
    def recent(self, limit=100):
        with closing(sqlite3.connect(self.db)) as c:
            rows=c.execute("SELECT id,ts,kind,payload FROM events ORDER BY id DESC LIMIT ?",(limit,)).fetchall()
        return [{'id':r[0],'ts':r[1],'kind':r[2],'payload':json.loads(r[3])} for r in reversed(rows)]
