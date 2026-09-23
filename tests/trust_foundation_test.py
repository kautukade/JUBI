import json
import math
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sarus.core.privileged_broker import PrivilegedBroker
from sarus.core.experience import ExperienceEngine
from sarus.core.capabilities import CapabilityRegistry, CapabilitySpec, AVAILABILITY


class TrustFoundationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.config = self.root / 'config.json'
        self.config.write_text('{}')

    def tearDown(self):
        self.tmp.cleanup()

    def broker(self):
        with patch.object(PrivilegedBroker, '_load_approval_secret', return_value=('', 'test')):
            return PrivilegedBroker(self.root, self.config, Mock(), Mock(), Mock())

    def test_broker_replay_is_rejected_after_recreation(self):
        self.broker()._mark_once('request1', 'nonce1')
        for request, nonce in [('request1', 'different'), ('different', 'nonce1')]:
            with self.assertRaises(PermissionError):
                self.broker()._mark_once(request, nonce)

    def test_two_broker_instances_cannot_consume_same_proof_concurrently(self):
        first, second = self.broker(), self.broker()
        def consume(broker):
            try:
                broker._mark_once('shared-request', 'shared-nonce')
                return True
            except PermissionError:
                return False
        with ThreadPoolExecutor(2) as pool:
            self.assertEqual(sorted(pool.map(consume, [first, second])), [False, True])

    def test_nonfinite_embeddings_fall_back_without_poisoning_memory(self):
        models = Mock()
        models.choose.return_value = 'embedding-A'
        engine = ExperienceEngine(self.root / 'memory.db', models)
        for value in (float('nan'), float('inf'), -float('inf')):
            models.embed.return_value = [1.0, value]
            self.assertFalse(engine.record('task', 'outcome', True)['embedded'])
        self.assertTrue(all(math.isfinite(row['score']) for row in engine.similar('task')))

    def test_semantic_search_never_compares_different_embedding_spaces(self):
        models = Mock()
        models.choose.return_value = 'embedding-A'
        models.embed.return_value = [1.0, 0.0]
        engine = ExperienceEngine(self.root / 'memory.db', models)
        engine.record('fix a parser', 'fixed parser', True)
        models.choose.return_value = 'embedding-B'
        with patch.object(engine, '_cosine', wraps=engine._cosine) as cosine:
            self.assertEqual(len(engine.similar('fix parser')), 1)  # lexical fallback remains usable
            cosine.assert_not_called()

    def test_indexed_source_id_is_not_execution_authority(self):
        registry = CapabilityRegistry(self.root, self.config, self.root / 'index.json')
        with self.assertRaises(PermissionError):
            registry.execute('indexed-python-file', {'executor': 'os.system'})

    def test_registry_denies_unknown_inputs_and_cyber_registration(self):
        registry = CapabilityRegistry(self.root, self.config, self.root / 'index.json')
        values = dict(id='read', name='Read', source='jubi', version='1', category='read',
                      description='test', platforms=('Windows',), dependencies=(), permissions=(),
                      privacy='local_only', risk=0, input_schema={'properties': {'text': {'type': 'string'}},
                                                               'required': ['text']},
                      output_schema={'type': 'object'}, health_check='test', executor='test', timeout_seconds=2,
                      resource_requirements={}, isolation='none', availability='AVAILABLE', verification='test')
        handler = Mock(return_value={'ok': True})
        registry.register_executor(CapabilitySpec(**values), handler)
        for params in ({}, {'text': 2}, {'text': 'hi', 'command': 'powershell'}):
            with self.assertRaises(ValueError):
                registry.execute('read', params)
        handler.assert_not_called()
        self.assertEqual(registry.execute('read', {'text': 'hello'}), {'ok': True})
        self.assertEqual(AVAILABILITY, {'AVAILABLE', 'PARTIAL', 'DEPENDENCY_MISSING', 'DISABLED', 'EXPERIMENTAL', 'FAILED'})
        for state in AVAILABILITY:
            spec = replace(CapabilitySpec(**values), id=state, availability=state)
            guarded = Mock(return_value={'ok': True})
            registry.register_executor(spec, guarded)
            if state in {'AVAILABLE', 'EXPERIMENTAL'}:
                self.assertEqual(registry.execute(state, {'text': 'hello'}), {'ok': True})
                guarded.assert_called_once()
            else:
                with self.assertRaises(PermissionError):
                    registry.execute(state, {'text': 'hello'})
                guarded.assert_not_called()
        with self.assertRaises(ValueError):
            registry.register_executor(replace(CapabilitySpec(**values), id='invalid', availability='READY'), handler)
        values.update(id='cyber-test', source='cybergym')
        with self.assertRaises(PermissionError):
            registry.register_executor(CapabilitySpec(**values), handler)


if __name__ == '__main__':
    unittest.main(verbosity=2)
