from __future__ import annotations

import hashlib
import json
import math
import re
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


class ExperienceEngine:
    """Bounded local experience memory for adaptive Jubi behavior.

    This is deliberately not uncontrolled continual weight training. Jubi stores
    task/outcome episodes, embeds them locally when Ollama embeddings are
    available, and retrieves similar successful/failed experiences to inform
    future routing and planning. Users can inspect and delete the records.
    """

    def __init__(self, db: Path, models, event_bus=None):
        self.db = db
        self.models = models
        self.event_bus = event_bus
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS experiences("
                "id TEXT PRIMARY KEY,ts REAL,kind TEXT,task_type TEXT,request TEXT,request_hash TEXT,"
                "outcome TEXT,success INTEGER,provider TEXT,model TEXT,tool TEXT,latency_ms REAL,"
                "lesson TEXT,embedding TEXT,embedding_model TEXT,metadata TEXT)"
            )
            c.execute("CREATE INDEX IF NOT EXISTS idx_experience_task ON experiences(task_type,success,ts)")

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is not None:
            try:
                self.event_bus.emit(kind, payload)
            except Exception:
                pass

    def _embed_best_effort(self, text: str) -> tuple[list[float] | None, str]:
        try:
            model = self.models.choose('embedding')
            if not model:
                return None, ''
            vector = self.models.embed(text, model=model)
            if isinstance(vector, list) and vector:
                return [float(x) for x in vector], model
        except Exception:
            pass
        return None, ''

    @staticmethod
    def _cosine(a: list[float], b: list[float]) -> float:
        if not a or not b or len(a) != len(b):
            return 0.0
        dot = sum(x * y for x, y in zip(a, b))
        na = math.sqrt(sum(x * x for x in a))
        nb = math.sqrt(sum(y * y for y in b))
        return dot / (na * nb) if na and nb else 0.0

    def record(self, request: str, outcome: str, success: bool, task_type: str = 'general',
               kind: str = 'task', provider: str = '', model: str = '', tool: str = '',
               latency_ms: float = 0.0, lesson: str = '', metadata=None) -> dict:
        request = str(request or '').strip()
        if not request:
            raise ValueError('experience request is required')
        outcome = str(outcome or '').strip()
        eid = str(uuid.uuid4())
        ts = time.time()
        embedding_text = f'Task: {request}\nOutcome: {outcome}\nLesson: {lesson}'.strip()
        vector, embedding_model = self._embed_best_effort(embedding_text[:12000])
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO experiences(id,ts,kind,task_type,request,request_hash,outcome,success,provider,model,tool,"
                "latency_ms,lesson,embedding,embedding_model,metadata) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    eid, ts, str(kind or 'task'), str(task_type or 'general'), request[:12000],
                    hashlib.sha256(request.encode('utf-8', errors='replace')).hexdigest(), outcome[:12000],
                    1 if success else 0, str(provider or ''), str(model or ''), str(tool or ''),
                    float(latency_ms or 0), str(lesson or '')[:4000],
                    json.dumps(vector, separators=(',', ':')) if vector else '', embedding_model,
                    json.dumps(metadata or {}, ensure_ascii=False),
                ),
            )
        item = {
            'id': eid, 'ts': ts, 'kind': kind or 'task', 'task_type': task_type or 'general',
            'success': bool(success), 'provider': provider or '', 'model': model or '', 'tool': tool or '',
            'latency_ms': float(latency_ms or 0), 'lesson': lesson or '', 'embedded': bool(vector),
        }
        self._emit('EXPERIENCE_RECORDED', item)
        return item

    def record_chat(self, request: str, result: dict, success: bool = True, error: str = '') -> dict:
        provider_route = (result or {}).get('jubi_provider_route') or {}
        brain_route = (result or {}).get('jubi_route') or {}
        provider = str(provider_route.get('provider') or ('ollama' if brain_route else ''))
        model = str(provider_route.get('selected_model') or brain_route.get('selected_model') or (result or {}).get('model') or '')
        task_type = str(provider_route.get('task_type') or brain_route.get('task_type') or 'general')
        latency = float(provider_route.get('latency_ms') or brain_route.get('latency_ms') or 0)
        outcome = str((result or {}).get('response') or (result or {}).get('output') or error or '')[:4000]
        lesson = 'Successful route can be preferred for similar work.' if success else 'Previous route failed; prefer a proven alternative for similar work.'
        return self.record(
            request, outcome, success, task_type=task_type, kind='chat', provider=provider,
            model=model, latency_ms=latency, lesson=lesson,
            metadata={'mode': provider_route.get('mode'), 'cloud': provider_route.get('cloud')},
        )

    def recent(self, limit: int = 100, success: bool | None = None) -> list[dict]:
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            sql = (
                "SELECT id,ts,kind,task_type,request,outcome,success,provider,model,tool,latency_ms,lesson,"
                "embedding_model,metadata FROM experiences"
            )
            args: list[object] = []
            if success is not None:
                sql += " WHERE success=?"
                args.append(1 if success else 0)
            sql += " ORDER BY ts DESC LIMIT ?"
            args.append(limit)
            rows = c.execute(sql, args).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'kind': r[2], 'task_type': r[3], 'request': r[4], 'outcome': r[5],
             'success': bool(r[6]), 'provider': r[7], 'model': r[8], 'tool': r[9], 'latency_ms': r[10],
             'lesson': r[11], 'embedding_model': r[12], 'metadata': json.loads(r[13] or '{}')}
            for r in rows
        ]

    def similar(self, query: str, task_type: str | None = None, limit: int = 6) -> list[dict]:
        query = str(query or '').strip()
        if not query:
            raise ValueError('experience query is required')
        limit = max(1, min(int(limit), 20))
        qvec, _ = self._embed_best_effort(query)
        qterms = {x for x in re.findall(r'\w+', query.lower()) if len(x) > 2}
        with read_connection(self.db) as c:
            sql = (
                "SELECT id,ts,kind,task_type,request,outcome,success,provider,model,tool,latency_ms,lesson,embedding,metadata "
                "FROM experiences"
            )
            args: list[object] = []
            if task_type:
                sql += " WHERE task_type=?"
                args.append(task_type)
            sql += " ORDER BY ts DESC LIMIT 1000"
            rows = c.execute(sql, args).fetchall()
        scored = []
        for r in rows:
            semantic = 0.0
            if qvec and r[12]:
                try:
                    semantic = self._cosine(qvec, [float(x) for x in json.loads(r[12])])
                except Exception:
                    semantic = 0.0
            terms = set(re.findall(r'\w+', (str(r[4]) + ' ' + str(r[5])).lower()))
            lexical = len(qterms & terms) / max(1, len(qterms)) if qterms else 0.0
            recency = max(0.0, 1.0 - (time.time() - float(r[1])) / (90 * 86400))
            success_bonus = 0.08 if r[6] else -0.02
            score = semantic * 0.78 + lexical * 0.14 + recency * 0.08 + success_bonus
            scored.append({
                'id': r[0], 'ts': r[1], 'kind': r[2], 'task_type': r[3], 'request': r[4], 'outcome': r[5],
                'success': bool(r[6]), 'provider': r[7], 'model': r[8], 'tool': r[9], 'latency_ms': r[10],
                'lesson': r[11], 'metadata': json.loads(r[13] or '{}'), 'score': round(score, 6),
            })
        scored.sort(key=lambda x: (-x['score'], -x['ts']))
        return scored[:limit]

    def delete(self, experience_id: str) -> dict:
        with transaction(self.db) as c:
            row = c.execute('SELECT id FROM experiences WHERE id=?', (experience_id,)).fetchone()
            if not row:
                raise KeyError('experience not found')
            c.execute('DELETE FROM experiences WHERE id=?', (experience_id,))
        self._emit('EXPERIENCE_DELETED', {'id': experience_id})
        return {'ok': True, 'id': experience_id}

    def stats(self) -> dict:
        with read_connection(self.db) as c:
            total = int(c.execute('SELECT COUNT(*) FROM experiences').fetchone()[0])
            successes = int(c.execute('SELECT COUNT(*) FROM experiences WHERE success=1').fetchone()[0])
            failures = total - successes
            embedded = int(c.execute("SELECT COUNT(*) FROM experiences WHERE embedding<>''").fetchone()[0])
            rows = c.execute(
                "SELECT provider,model,task_type,COUNT(*),SUM(success),AVG(latency_ms) FROM experiences "
                "GROUP BY provider,model,task_type ORDER BY COUNT(*) DESC LIMIT 50"
            ).fetchall()
        return {
            'total': total, 'successes': successes, 'failures': failures,
            'success_rate': successes / total if total else None, 'embedded': embedded,
            'groups': [
                {'provider': r[0], 'model': r[1], 'task_type': r[2], 'runs': int(r[3]),
                 'successes': int(r[4] or 0), 'avg_latency_ms': float(r[5] or 0)}
                for r in rows
            ],
        }
