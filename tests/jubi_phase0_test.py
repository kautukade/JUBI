from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.events import EventBus
from sarus.core.execution import ExecutionEngine
from sarus.core.memory import MemoryStore
from sarus.core.models import OllamaRouter
from sarus.core.orchestrator import Step
from sarus.core.workflows import WorkflowScheduler


class PersistenceTests(unittest.TestCase):
    def test_memory_survives_reopen(self):
        with tempfile.TemporaryDirectory() as td:
            db = Path(td) / 'jubi.db'
            first = MemoryStore(db)
            saved = first.add('persistent memory token', 'Persistence Test', 'test')
            second = MemoryStore(db)
            rows = second.search('persistent', 'test', 10)
            self.assertTrue(any(x['id'] == saved['id'] for x in rows))

    def test_events_survive_reopen(self):
        with tempfile.TemporaryDirectory() as td:
            db = Path(td) / 'jubi.db'
            EventBus(db).emit('JUBI_TEST_EVENT', {'value': 7})
            rows = EventBus(db).recent(20)
            self.assertTrue(any(x['kind'] == 'JUBI_TEST_EVENT' and x['payload']['value'] == 7 for x in rows))

    def test_automation_survives_reopen(self):
        with tempfile.TemporaryDirectory() as td:
            db = Path(td) / 'jubi.db'
            first = WorkflowScheduler(db, lambda *_a, **_kw: None)
            created = first.add('Persisted automation', 'test prompt', 300)
            second = WorkflowScheduler(db, lambda *_a, **_kw: None)
            rows = second.list()
            self.assertTrue(any(x['id'] == created['id'] for x in rows))


class RouterTests(unittest.TestCase):
    def test_missing_configured_model_is_never_returned(self):
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / 'models.json'
            cfg.write_text(json.dumps({'general': ['missing:7b'], 'coding': [], 'embedding': []}), encoding='utf-8')
            router = OllamaRouter(cfg)
            router.list_models = lambda: {
                'online': True,
                'models': ['installed:7b'],
                'items': [{'name': 'installed:7b', 'kind': 'general'}],
            }
            self.assertEqual(router.choose('general'), 'installed:7b')
            self.assertNotEqual(router.choose('general'), 'missing:7b')

    def test_embedding_has_no_general_fallback(self):
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / 'models.json'
            cfg.write_text(json.dumps({'general': ['installed:7b'], 'embedding': ['missing-embed']}), encoding='utf-8')
            router = OllamaRouter(cfg)
            router.list_models = lambda: {
                'online': True,
                'models': ['installed:7b'],
                'items': [{'name': 'installed:7b', 'kind': 'general'}],
            }
            self.assertIsNone(router.choose('embedding'))


class _Bus:
    def __init__(self):
        self.items = []

    def emit(self, kind, payload):
        self.items.append((kind, payload))


class _Policy:
    def evaluate(self, action, level=0, source='core'):
        if level >= 4:
            return {'decision': 'approval', 'reason': 'high_risk'}
        return {'decision': 'allow', 'reason': 'policy_ok'}


class _Adapter:
    def __init__(self):
        self.calls = 0

    def execute(self, request, app, step=None, capability_id=None, context=None):
        self.calls += 1
        return {'ok': True, 'output': 'EXECUTED:' + request[:50]}


class _Adapters:
    def __init__(self, adapter):
        self.adapter = adapter

    def get(self, source):
        return self.adapter


class _Registry:
    def get(self, _cid):
        return None


class _Receipts:
    def __init__(self):
        self.n = 0

    def create(self, task_id, step_id, source, status, payload):
        self.n += 1
        return {'id': f'r{self.n}', 'hash': f'h{self.n}', 'status': status}


class _Memory:
    def add(self, *args, **kwargs):
        return {'id': 'memory'}


class _Orchestrator:
    def plan(self, text):
        return [Step('step-risky', 'tester', 'hermes', 'approved test action', 4)]


class _FakeApp:
    def __init__(self, root: Path, adapter: _Adapter):
        self.root = root
        self.db_path = root / 'data' / 'jubi-test.db'
        self.bus = _Bus()
        self.policy = _Policy()
        self.adapters = _Adapters(adapter)
        self.registry = _Registry()
        self.receipts = _Receipts()
        self.memory = _Memory()
        self.orchestrator = _Orchestrator()


class ApprovalResumeTests(unittest.TestCase):
    def test_approval_survives_restart_and_resumes_exact_step(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            adapter = _Adapter()
            app1 = _FakeApp(root, adapter)
            engine1 = ExecutionEngine(app1)
            waiting = engine1.run('needs approval')
            self.assertEqual(waiting['status'], 'waiting_approval')
            self.assertEqual(adapter.calls, 0)
            aid = waiting['approval_id']

            # Simulate a Jubi process restart by constructing a new app/engine on
            # the same SQLite database.
            app2 = _FakeApp(root, adapter)
            engine2 = ExecutionEngine(app2)
            pending = engine2.approvals('pending')
            self.assertEqual([x['id'] for x in pending], [aid])
            resolved = engine2.set_approval(aid, 'approved')
            self.assertEqual(resolved['task']['status'], 'completed')
            self.assertEqual(adapter.calls, 1)
            self.assertEqual(resolved['task']['id'], resolved['task']['task_id'])

            with self.assertRaises(RuntimeError):
                engine2.set_approval(aid, 'approved')
            self.assertEqual(adapter.calls, 1)

    def test_rejected_approval_never_executes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            adapter = _Adapter()
            app = _FakeApp(root, adapter)
            engine = ExecutionEngine(app)
            waiting = engine.run('reject this')
            result = engine.set_approval(waiting['approval_id'], 'rejected')
            self.assertEqual(result['task']['status'], 'rejected')
            self.assertEqual(adapter.calls, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
