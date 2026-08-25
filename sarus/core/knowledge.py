from __future__ import annotations

import json
import math
import re
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


class SemanticKnowledge:
    """Local semantic knowledge store backed by Ollama embeddings + SQLite.

    Embeddings stay on-device. Jubi stores vectors as compact JSON in the
    existing SQLite database to keep deployment dependency-free. Generation is
    delegated to ProviderManager so the current Local Only / Hybrid Auto /
    Cloud Boost policy remains the single cloud boundary.
    """

    def __init__(self, db: Path, models, providers, event_bus=None):
        self.db = db
        self.models = models
        self.providers = providers
        self.event_bus = event_bus
        self.embedding_model = None
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS knowledge_documents("
                "id TEXT PRIMARY KEY,ts REAL,namespace TEXT,title TEXT,source TEXT,metadata TEXT,chunk_count INTEGER)"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS knowledge_chunks("
                "id TEXT PRIMARY KEY,document_id TEXT,ordinal INTEGER,text TEXT,embedding TEXT,"
                "embedding_model TEXT,chars INTEGER,FOREIGN KEY(document_id) REFERENCES knowledge_documents(id) ON DELETE CASCADE)"
            )
            c.execute("CREATE INDEX IF NOT EXISTS idx_knowledge_chunks_doc ON knowledge_chunks(document_id,ordinal)")
            c.execute("CREATE INDEX IF NOT EXISTS idx_knowledge_docs_ns ON knowledge_documents(namespace,ts)")

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is not None:
            try:
                self.event_bus.emit(kind, payload)
            except Exception:
                pass

    @staticmethod
    def _clean(text: str) -> str:
        text = str(text or '').replace('\x00', ' ')
        text = re.sub(r'\r\n?', '\n', text)
        text = re.sub(r'[ \t]+', ' ', text)
        text = re.sub(r'\n{3,}', '\n\n', text)
        return text.strip()

    @classmethod
    def chunk_text(cls, text: str, target_chars: int = 1400, overlap_chars: int = 180) -> list[str]:
        text = cls._clean(text)
        if not text:
            return []
        target_chars = max(400, min(int(target_chars), 4000))
        overlap_chars = max(0, min(int(overlap_chars), target_chars // 3))
        paragraphs = [p.strip() for p in text.split('\n\n') if p.strip()]
        chunks: list[str] = []
        current = ''
        for para in paragraphs:
            if len(para) > target_chars * 2:
                pieces = [para[i:i + target_chars] for i in range(0, len(para), target_chars - overlap_chars or target_chars)]
            else:
                pieces = [para]
            for piece in pieces:
                candidate = f'{current}\n\n{piece}'.strip() if current else piece
                if current and len(candidate) > target_chars:
                    chunks.append(current.strip())
                    tail = current[-overlap_chars:] if overlap_chars else ''
                    current = f'{tail}\n{piece}'.strip()
                else:
                    current = candidate
        if current.strip():
            chunks.append(current.strip())
        return [c for c in chunks if c]

    def _embed(self, text: str) -> tuple[list[float], str]:
        model = self.models.choose('embedding')
        if not model:
            raise RuntimeError('Semantic Knowledge requires an installed local Ollama embedding model')
        vector = self.models.embed(text, model=model)
        if not isinstance(vector, list) or not vector:
            raise RuntimeError('Ollama embedding model returned an empty vector')
        self.embedding_model = model
        return [float(x) for x in vector], model

    @staticmethod
    def _cosine(a: list[float], b: list[float]) -> float:
        if not a or not b or len(a) != len(b):
            return 0.0
        dot = sum(x * y for x, y in zip(a, b))
        na = math.sqrt(sum(x * x for x in a))
        nb = math.sqrt(sum(y * y for y in b))
        return dot / (na * nb) if na and nb else 0.0

    def ingest(self, content: str, title: str = '', namespace: str = 'general', source: str = 'manual', metadata=None) -> dict:
        clean = self._clean(content)
        if not clean:
            raise ValueError('knowledge content is required')
        chunks = self.chunk_text(clean)
        if not chunks:
            raise ValueError('knowledge content produced no chunks')
        doc_id = str(uuid.uuid4())
        created = time.time()
        embedded = []
        model_name = None
        for ordinal, chunk in enumerate(chunks):
            vector, model_name = self._embed(chunk)
            embedded.append((str(uuid.uuid4()), ordinal, chunk, json.dumps(vector, separators=(',', ':'))))
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO knowledge_documents(id,ts,namespace,title,source,metadata,chunk_count) VALUES(?,?,?,?,?,?,?)",
                (doc_id, created, namespace or 'general', title or 'Untitled knowledge', source or 'manual',
                 json.dumps(metadata or {}, ensure_ascii=False), len(embedded)),
            )
            c.executemany(
                "INSERT INTO knowledge_chunks(id,document_id,ordinal,text,embedding,embedding_model,chars) VALUES(?,?,?,?,?,?,?)",
                [(cid, doc_id, ordinal, text, vector, model_name or '', len(text)) for cid, ordinal, text, vector in embedded],
            )
        result = {
            'id': doc_id,
            'ts': created,
            'namespace': namespace or 'general',
            'title': title or 'Untitled knowledge',
            'source': source or 'manual',
            'chunks': len(embedded),
            'embedding_model': model_name,
        }
        self._emit('KNOWLEDGE_INGESTED', {k: result[k] for k in ('id', 'namespace', 'title', 'source', 'chunks', 'embedding_model')})
        return result

    def documents(self, namespace: str | None = None, limit: int = 100) -> list[dict]:
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            sql = "SELECT id,ts,namespace,title,source,metadata,chunk_count FROM knowledge_documents"
            args: list[object] = []
            if namespace:
                sql += " WHERE namespace=?"
                args.append(namespace)
            sql += " ORDER BY ts DESC LIMIT ?"
            args.append(limit)
            rows = c.execute(sql, args).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'namespace': r[2], 'title': r[3], 'source': r[4],
             'metadata': json.loads(r[5] or '{}'), 'chunk_count': int(r[6] or 0)}
            for r in rows
        ]

    def delete_document(self, document_id: str) -> dict:
        with transaction(self.db) as c:
            row = c.execute("SELECT title FROM knowledge_documents WHERE id=?", (document_id,)).fetchone()
            if not row:
                raise KeyError('knowledge document not found')
            c.execute("DELETE FROM knowledge_chunks WHERE document_id=?", (document_id,))
            c.execute("DELETE FROM knowledge_documents WHERE id=?", (document_id,))
        self._emit('KNOWLEDGE_DELETED', {'id': document_id, 'title': row[0]})
        return {'ok': True, 'id': document_id}

    def search(self, query: str, namespace: str | None = None, limit: int = 8, min_score: float = 0.0) -> list[dict]:
        query = self._clean(query)
        if not query:
            raise ValueError('semantic query is required')
        limit = max(1, min(int(limit), 30))
        qvec, model = self._embed(query)
        with read_connection(self.db) as c:
            sql = (
                "SELECT k.id,k.document_id,k.ordinal,k.text,k.embedding,k.embedding_model,"
                "d.namespace,d.title,d.source,d.metadata FROM knowledge_chunks k "
                "JOIN knowledge_documents d ON d.id=k.document_id"
            )
            args: list[object] = []
            if namespace:
                sql += " WHERE d.namespace=?"
                args.append(namespace)
            rows = c.execute(sql, args).fetchall()
        qterms = {x for x in re.findall(r'\w+', query.lower()) if len(x) > 2}
        scored = []
        for r in rows:
            try:
                vector = [float(x) for x in json.loads(r[4])]
            except Exception:
                continue
            semantic = self._cosine(qvec, vector)
            text_terms = set(re.findall(r'\w+', str(r[3]).lower()))
            lexical = len(qterms & text_terms) / max(1, len(qterms)) if qterms else 0.0
            score = semantic * 0.9 + lexical * 0.1
            if score < float(min_score):
                continue
            scored.append({
                'chunk_id': r[0], 'document_id': r[1], 'ordinal': int(r[2]), 'text': r[3],
                'score': round(score, 6), 'semantic_score': round(semantic, 6),
                'namespace': r[6], 'title': r[7], 'source': r[8], 'metadata': json.loads(r[9] or '{}'),
                'embedding_model': r[5] or model,
            })
        scored.sort(key=lambda x: (-x['score'], x['document_id'], x['ordinal']))
        self._emit('KNOWLEDGE_SEARCHED', {'query_length': len(query), 'namespace': namespace or '', 'matches': min(limit, len(scored))})
        return scored[:limit]

    def answer(self, question: str, namespace: str | None = None, limit: int = 6,
               provider: str = 'auto', model: str | None = None) -> dict:
        matches = self.search(question, namespace=namespace, limit=limit)
        if not matches:
            return {'answer': 'No relevant local knowledge was found.', 'sources': [], 'provider': None, 'model': None}
        context_parts = []
        sources = []
        for index, item in enumerate(matches, start=1):
            ref = f'K{index}'
            context_parts.append(f'[{ref}] {item["title"]} | {item["source"]}\n{item["text"]}')
            sources.append({
                'ref': ref, 'document_id': item['document_id'], 'title': item['title'],
                'source': item['source'], 'namespace': item['namespace'], 'score': item['score'],
                'chunk_id': item['chunk_id'], 'ordinal': item['ordinal'],
            })
        prompt = (
            'Question:\n' + self._clean(question) + '\n\nLocal knowledge context:\n' + '\n\n'.join(context_parts) +
            '\n\nAnswer using the supplied context. Cite supporting context inline as [K1], [K2], etc. '
            'If the context is insufficient, say what is missing instead of inventing facts.'
        )
        result = self.providers.generate(
            prompt,
            task_type='research',
            model=model,
            provider=provider,
            system='You are Jubi Knowledge. Ground answers in the supplied local context and preserve source markers.',
        )
        answer = str(result.get('response') or result.get('output') or '').strip()
        return {
            'answer': answer,
            'sources': sources,
            'provider_route': result.get('jubi_provider_route') or {},
            'model': result.get('model'),
        }

    def status(self) -> dict:
        with read_connection(self.db) as c:
            docs = int(c.execute('SELECT COUNT(*) FROM knowledge_documents').fetchone()[0])
            chunks = int(c.execute('SELECT COUNT(*) FROM knowledge_chunks').fetchone()[0])
            namespaces = int(c.execute('SELECT COUNT(DISTINCT namespace) FROM knowledge_documents').fetchone()[0])
        return {
            'name': 'Jubi Semantic Knowledge',
            'documents': docs,
            'chunks': chunks,
            'namespaces': namespaces,
            'embedding_model': self.models.choose('embedding'),
            'storage': 'SQLite local vectors',
            'cloud_embedding': False,
        }
