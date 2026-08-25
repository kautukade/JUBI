from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from sarus.core.council import AICouncil, MultiAgentSupervisor


class FakeBrain:
    def classify(self, text, task_type='auto'):
        low = str(text).lower()
        privacy = 'high' if 'api key' in low or 'secret' in low else 'local'
        task = 'coding' if 'code' in low else ('planning' if task_type in ('auto', None) else str(task_type))
        return {'intent': task, 'task_type': task, 'complexity': 4, 'privacy': privacy, 'tool_hint': 'none', 'prompt_length': len(str(text))}

    def route(self, text, task_type='auto', requested_model=None):
        return {
            'task_type': self.classify(text, task_type)['task_type'],
            'candidates': [
                {'model': 'local-a', 'kind': 'general', 'score': 100},
                {'model': 'local-b', 'kind': 'general', 'score': 90},
            ],
        }


class FakeProviders:
    def __init__(self, mode='hybrid_auto', cloud=True):
        self.mode = mode
        self.cloud = cloud
        self.calls = []

    def route_preview(self, prompt, task_type='auto', provider='auto'):
        privacy = 'high' if 'api key' in str(prompt).lower() else 'local'
        return {
            'intent': 'planning', 'task_type': 'planning', 'complexity': 4, 'privacy': privacy,
            'tool_hint': 'none', 'prompt_length': len(str(prompt)), 'mode': self.mode,
            'provider_order': ['ollama', 'nvidia'] if self.cloud else ['ollama'],
            'cloud_configured': ['nvidia'] if self.cloud else [],
            'high_privacy_cloud_blocked': privacy == 'high',
        }

    def generate(self, prompt, task_type='auto', model=None, provider='auto', system='', timeout=300):
        self.calls.append({'prompt': prompt, 'task_type': task_type, 'provider': provider, 'model': model, 'system': system})
        if 'Return ONLY valid JSON' in prompt:
            response = json.dumps({
                'goal': 'Ship feature',
                'steps': [
                    {'id': 'S1', 'role': 'research', 'task': 'Inspect requirements', 'deliverable': 'Requirements', 'depends_on': [], 'risk': 'low'},
                    {'id': 'S2', 'role': 'coding', 'task': 'Design implementation', 'deliverable': 'Implementation', 'depends_on': ['S1'], 'risk': 'medium'},
                ],
                'verification': ['Review requirements and implementation'],
            })
        elif 'Act as the Council Judge' in prompt:
            response = 'Council synthesis: choose the evidence-backed option.'
        elif 'Review the work.' in prompt:
            response = 'Supervisor final: reviewed integrated result.'
        else:
            response = f'Independent answer from {provider}/{model or "auto"}'
        actual_provider = 'ollama' if provider in ('auto', 'ollama') else provider
        return {
            'response': response,
            'model': model or f'{actual_provider}-model',
            'jubi_provider_route': {
                'provider': actual_provider, 'selected_model': model or f'{actual_provider}-model',
                'task_type': task_type, 'cloud': actual_provider != 'ollama', 'mode': self.mode,
            },
        }


class FakeKnowledge:
    def search(self, query, namespace=None, limit=4, min_score=0.0):
        return [{'title': 'Local note', 'source': 'test', 'text': 'Use tested incremental changes.', 'score': 0.9}]


class FakeExperience:
    def __init__(self):
        self.recorded = []

    def similar(self, query, task_type=None, limit=4):
        return [{'request': 'Prior similar task', 'success': True, 'lesson': 'Test before merge', 'provider': 'ollama', 'model': 'local-a'}]

    def record(self, *args, **kwargs):
        self.recorded.append((args, kwargs))
        return {'id': 'exp-1'}


class CouncilSupervisorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / 'jubi.db'
        self.brain = FakeBrain()
        self.providers = FakeProviders()

    def tearDown(self):
        self.tmp.cleanup()

    def test_council_uses_multiple_members_and_judge(self):
        council = AICouncil(self.db, self.brain, self.providers)
        result = council.run('Choose a production architecture', max_members=3)
        self.assertGreaterEqual(len(result['members']), 2)
        self.assertIn('Council synthesis', result['final'])
        self.assertEqual(len(council.recent()), 1)
        self.assertTrue(any('Act as the Council Judge' in x['prompt'] for x in self.providers.calls))

    def test_council_keeps_high_privacy_prompt_local(self):
        council = AICouncil(self.db, self.brain, self.providers)
        result = council.run('Analyze this secret api key handling design', max_members=4)
        self.assertEqual(result['classification']['privacy'], 'high')
        member_providers = {x['provider'] for x in result['members']}
        self.assertEqual(member_providers, {'ollama'})

    def test_supervisor_plan_uses_local_context_and_json_plan(self):
        experience = FakeExperience()
        supervisor = MultiAgentSupervisor(self.db, self.brain, self.providers, FakeKnowledge(), experience)
        result = supervisor.plan('Plan code delivery')
        self.assertEqual(result['plan']['goal'], 'Ship feature')
        self.assertEqual(len(result['plan']['steps']), 2)
        planner_prompt = self.providers.calls[-1]['prompt']
        self.assertIn('Prior similar task', planner_prompt)
        self.assertIn('Local note', planner_prompt)

    def test_supervisor_run_specialists_reviewer_and_history(self):
        experience = FakeExperience()
        supervisor = MultiAgentSupervisor(self.db, self.brain, self.providers, FakeKnowledge(), experience)
        result = supervisor.run('Plan code delivery')
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(len(result['results']), 2)
        self.assertIn('Supervisor final', result['final'])
        self.assertEqual(len(supervisor.recent()), 1)
        self.assertEqual(len(experience.recorded), 1)
        self.assertTrue(any('Review the work.' in x['prompt'] for x in self.providers.calls))

    def test_supervisor_is_reasoning_only_in_source(self):
        source = (ROOT / 'sarus/core/council.py').read_text(encoding='utf-8')
        self.assertNotIn('subprocess.', source)
        self.assertNotIn('os.system', source)
        self.assertNotIn('privileged.handle', source)
        self.assertNotIn('windows.', source)


if __name__ == '__main__':
    unittest.main(verbosity=2)
