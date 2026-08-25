from __future__ import annotations

import hashlib
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from sarus.core.knowledge import SemanticKnowledge
from sarus.core.experience import ExperienceEngine


class FakeModels:
    def choose(self, task_type='general'):
        return 'fake-embed' if task_type == 'embedding' else 'fake-general'

    @staticmethod
    def _vector(text: str):
        low = text.lower()
        dims = [
            low.count('deployment') + low.count('netlify'),
            low.count('database') + low.count('sqlite'),
            low.count('client') + low.count('sales'),
            low.count('python') + low.count('code'),
            low.count('memory') + low.count('knowledge'),
        ]
        if not any(dims):
            digest = hashlib.sha256(text.encode()).digest()
            dims = [digest[i] / 255 for i in range(5)]
        norm = math.sqrt(sum(x*x for x in dims)) or 1
        return [x / norm for x in dims]

    def embed(self, text, model=None):
        return self._vector(str(text))


class FakeProviders:
    def __init__(self):
        self.calls = []

    def generate(self, prompt, task_type='auto', model=None, provider='auto', system='', timeout=300):
        self.calls.append({'prompt': prompt, 'provider': provider, 'task_type': task_type})
        self.assert_grounded = '[K1]' in prompt
        return {
            'response': 'The deployment uses Netlify [K1].',
            'model': 'fake-answer-model',
            'jubi_provider_route': {'provider': 'ollama', 'selected_model': 'fake-answer-model', 'cloud': False},
        }


class KnowledgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / 'jubi.db'
        self.models = FakeModels()
        self.providers = FakeProviders()
        self.knowledge = SemanticKnowledge(self.db, self.models, self.providers)
        self.experience = ExperienceEngine(self.db, self.models)

    def tearDown(self):
        self.tmp.cleanup()

    def test_chunk_ingest_search_and_delete(self):
        content = ('Deployment uses Netlify for the frontend.\n\n' * 30) + 'The database is SQLite locally.'
        result = self.knowledge.ingest(content, 'Deployment Notes', 'project', 'test')
        self.assertGreaterEqual(result['chunks'], 1)
        status = self.knowledge.status()
        self.assertEqual(status['documents'], 1)
        self.assertGreaterEqual(status['chunks'], 1)
        rows = self.knowledge.search('Where is deployment hosted?', namespace='project', limit=4)
        self.assertTrue(rows)
        self.assertEqual(rows[0]['title'], 'Deployment Notes')
        self.assertGreater(rows[0]['score'], 0)
        self.knowledge.delete_document(result['id'])
        self.assertEqual(self.knowledge.status()['documents'], 0)

    def test_rag_answer_returns_visible_sources(self):
        self.knowledge.ingest('Production deployment is hosted on Netlify.', 'Hosting', 'project', 'manual')
        result = self.knowledge.answer('Where is production deployed?', namespace='project', provider='ollama')
        self.assertIn('[K1]', result['answer'])
        self.assertEqual(result['sources'][0]['ref'], 'K1')
        self.assertEqual(self.providers.calls[0]['provider'], 'ollama')
        self.assertIn('[K1]', self.providers.calls[0]['prompt'])

    def test_experience_record_stats_and_similarity(self):
        one = self.experience.record(
            'Fix Python deployment code', 'Succeeded after updating code', True,
            task_type='coding', provider='ollama', model='coder', lesson='Use coder for Python fixes.'
        )
        self.experience.record(
            'Write client sales email', 'Succeeded', True,
            task_type='document', provider='nvidia', model='writer'
        )
        stats = self.experience.stats()
        self.assertEqual(stats['total'], 2)
        self.assertEqual(stats['successes'], 2)
        rows = self.experience.similar('Python code bug', task_type='coding', limit=3)
        self.assertTrue(rows)
        self.assertEqual(rows[0]['id'], one['id'])
        self.experience.delete(one['id'])
        self.assertEqual(self.experience.stats()['total'], 1)

    def test_record_chat_keeps_provider_route(self):
        result = {
            'response': 'Done', 'model': 'm',
            'jubi_provider_route': {
                'provider': 'openrouter', 'selected_model': 'm', 'task_type': 'coding',
                'latency_ms': 123, 'mode': 'hybrid_auto', 'cloud': True,
            },
        }
        item = self.experience.record_chat('Fix code', result, True)
        self.assertEqual(item['provider'], 'openrouter')
        self.assertEqual(item['model'], 'm')
        recent = self.experience.recent(1)
        self.assertEqual(recent[0]['metadata']['cloud'], True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
