from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from sarus.core.brain import BrainRouter


class FakeModels:
    base = 'http://127.0.0.1:11434'
    cfg = {
        'general': ['qwen2.5:7b', 'glm4:latest'],
        'coding': ['qwen2.5-coder:7b', 'qwen2.5:7b'],
        'vision': ['qwen2.5vl:3b'],
    }

    def __init__(self):
        self.fail = set()
        self.calls = []

    def list_models(self):
        return {
            'online': True,
            'models': [
                'qwen2.5:7b', 'glm4:latest', 'qwen2.5-coder:7b',
                'qwen2.5vl:3b', 'nomic-embed-text-v2-moe:latest',
                'qwen3-coder:480b-cloud',
            ],
            'items': [
                {'name': 'qwen2.5:7b', 'kind': 'general'},
                {'name': 'glm4:latest', 'kind': 'general'},
                {'name': 'qwen2.5-coder:7b', 'kind': 'coding'},
                {'name': 'qwen2.5vl:3b', 'kind': 'vision'},
                {'name': 'nomic-embed-text-v2-moe:latest', 'kind': 'embedding'},
                {'name': 'qwen3-coder:480b-cloud', 'kind': 'cloud-through-ollama'},
            ],
        }

    def generate(self, prompt, task_type='general', system='', model=None, timeout=300):
        self.calls.append((prompt, task_type, model))
        if model in self.fail:
            raise RuntimeError('simulated model failure')
        return {'response': f'ok:{model}', 'model': model}


class BrainRouterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.models = FakeModels()
        self.brain = BrainRouter(root / 'jubi.db', self.models, root / 'missing-brain.json')

    def tearDown(self):
        self.tmp.cleanup()

    def test_classifies_common_task_types(self):
        self.assertEqual(self.brain.classify('Fix this React login bug')['task_type'], 'coding')
        self.assertEqual(self.brain.classify('Analyze this screenshot and UI error')['task_type'], 'vision')
        self.assertEqual(self.brain.classify('Deep research and compare sources')['task_type'], 'research')
        self.assertEqual(self.brain.classify('Write a detailed project report')['task_type'], 'document')

    def test_automatic_route_uses_installed_compatible_local_model(self):
        route = self.brain.route('Fix Python API error')
        self.assertEqual(route['task_type'], 'coding')
        self.assertEqual(route['selected_model'], 'qwen2.5-coder:7b')
        names = [x['model'] for x in route['candidates']]
        self.assertNotIn('qwen3-coder:480b-cloud', names)
        self.assertNotIn('nomic-embed-text-v2-moe:latest', names)

    def test_explicit_model_must_exist_and_cannot_be_embedding(self):
        route = self.brain.route('hello', requested_model='glm4:latest')
        self.assertEqual(route['selected_model'], 'glm4:latest')
        with self.assertRaises(RuntimeError):
            self.brain.route('hello', requested_model='not-installed:latest')
        with self.assertRaises(RuntimeError):
            self.brain.route('hello', requested_model='nomic-embed-text-v2-moe:latest')

    def test_generate_falls_back_and_records_outcomes(self):
        self.models.fail.add('qwen2.5-coder:7b')
        result = self.brain.generate('Fix this Python bug')
        self.assertEqual(result['response'], 'ok:qwen2.5:7b')
        self.assertEqual(result['jubi_route']['attempt'], 2)
        perf = {(x['model'], x['task_type']): x for x in self.brain.performance()}
        self.assertEqual(perf[('qwen2.5-coder:7b', 'coding')]['failures'], 1)
        self.assertEqual(perf[('qwen2.5:7b', 'coding')]['successes'], 1)
        decisions = self.brain.recent_decisions()
        self.assertEqual(decisions[0]['status'], 'success')
        self.assertEqual(decisions[0]['selected_model'], 'qwen2.5:7b')

    def test_performance_history_influences_ranking(self):
        # glm4 starts behind qwen2.5 because of configured preference.
        initial = self.brain.route('Explain this business idea')
        self.assertEqual(initial['selected_model'], 'qwen2.5:7b')
        for _ in range(8):
            self.brain._record_model_outcome('qwen2.5:7b', 'general', False, 20000)
            self.brain._record_model_outcome('glm4:latest', 'general', True, 2000)
        learned = self.brain.route('Explain this business idea')
        self.assertEqual(learned['selected_model'], 'glm4:latest')

    def test_high_privacy_request_remains_local(self):
        route = self.brain.route('Review this confidential API key handling design')
        self.assertEqual(route['privacy'], 'high')
        self.assertTrue(all(x['kind'] != 'cloud-through-ollama' for x in route['candidates']))


if __name__ == '__main__':
    unittest.main(verbosity=2)
