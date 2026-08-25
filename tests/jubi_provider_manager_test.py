from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from sarus.core.brain import BrainRouter
from sarus.core.credentials import CredentialStore
from sarus.core.providers import OpenAICompatibleProvider, ProviderManager


class FakeModels:
    base = 'http://127.0.0.1:11434'
    cfg = {
        'general': ['qwen2.5:7b'],
        'coding': ['qwen2.5-coder:7b', 'qwen2.5:7b'],
        'vision': ['qwen2.5vl:3b'],
    }

    def __init__(self):
        self.calls = []
        self.fail = False

    def list_models(self):
        return {
            'online': True,
            'models': ['qwen2.5:7b', 'qwen2.5-coder:7b', 'qwen2.5vl:3b'],
            'items': [
                {'name': 'qwen2.5:7b', 'kind': 'general'},
                {'name': 'qwen2.5-coder:7b', 'kind': 'coding'},
                {'name': 'qwen2.5vl:3b', 'kind': 'vision'},
            ],
        }

    def generate(self, prompt, task_type='general', system='', model=None, timeout=300):
        if self.fail:
            raise RuntimeError('local model failed')
        chosen = model or ('qwen2.5-coder:7b' if task_type == 'coding' else 'qwen2.5:7b')
        self.calls.append((prompt, task_type, chosen))
        return {'response': 'LOCAL_OK', 'model': chosen}


class FakeCredentials:
    def __init__(self, configured=('openrouter', 'nvidia', 'huggingface')):
        self.configured = set(configured)
        self.path = Path('fake-credential-store')

    def get(self, provider):
        return (f'test-secret-{provider}', 'fake') if provider in self.configured else (None, 'none')

    def status(self, provider):
        return {
            'provider': provider,
            'configured': provider in self.configured,
            'source': 'fake' if provider in self.configured else 'none',
            'persistent_dashboard_storage': True,
        }

    def set(self, provider, secret):
        self.configured.add(provider)
        return {'provider': provider, 'configured': True, 'source': 'fake'}

    def delete(self, provider):
        self.configured.discard(provider)
        return {'provider': provider, 'configured': False, 'source': 'none'}


class FakeCloud:
    def __init__(self, provider, fail=False):
        self.provider_id = provider
        self.label = provider.title()
        self.fail = fail
        self.calls = []

    def list_models(self, timeout=20):
        model = {
            'openrouter': 'openrouter/free',
            'nvidia': 'nvidia/nemotron-3-super-120b-a12b',
            'huggingface': 'openai/gpt-oss-20b:fastest',
        }[self.provider_id]
        return {
            'provider': self.provider_id,
            'online': True,
            'count': 1,
            'models': [{'id': model, 'name': model, 'free': self.provider_id == 'openrouter'}],
        }

    def health(self, timeout=12):
        return {
            'provider': self.provider_id,
            'label': self.label,
            'configured': True,
            'online': not self.fail,
            'status': 'connected' if not self.fail else 'error',
            'credential_source': 'fake',
            'models': 1,
        }

    def chat(self, prompt, model, system, timeout=180):
        self.calls.append((prompt, model))
        if self.fail:
            raise RuntimeError(f'{self.provider_id} failed')
        return {'response': f'{self.provider_id.upper()}_OK', 'model': model, 'provider': self.provider_id, 'usage': {}}


class ProviderManagerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / 'jubi.db'
        self.models = FakeModels()
        self.brain = BrainRouter(self.db, self.models, ROOT / 'config/brain.json')
        self.credentials = FakeCredentials()
        self.manager = ProviderManager(
            self.db, self.brain, ROOT / 'config/providers.json', credentials=self.credentials
        )
        self.cloud = {p: FakeCloud(p) for p in ('openrouter', 'nvidia', 'huggingface')}
        self.manager.providers = self.cloud

    def tearDown(self):
        self.tmp.cleanup()

    def test_local_only_never_calls_cloud(self):
        self.assertEqual(self.manager.mode(), 'local_only')
        result = self.manager.generate('Explain this in simple language')
        self.assertEqual(result['response'], 'LOCAL_OK')
        self.assertEqual(result['jubi_provider_route']['provider'], 'ollama')
        self.assertFalse(result['jubi_provider_route']['cloud'])
        self.assertTrue(all(not provider.calls for provider in self.cloud.values()))

    def test_mode_persists_in_sqlite(self):
        self.manager.set_mode('hybrid_auto')
        other = ProviderManager(
            self.db, self.brain, ROOT / 'config/providers.json', credentials=self.credentials
        )
        self.assertEqual(other.mode(), 'hybrid_auto')

    def test_hybrid_complex_request_can_prefer_cloud(self):
        self.manager.set_mode('hybrid_auto')
        text = (
            'Design a complete production architecture and detailed migration strategy for this entire '
            'application with security performance testing rollback monitoring and implementation steps '
            'for every service database interface and deployment component.'
        )
        preview = self.manager.route_preview(text)
        self.assertGreaterEqual(preview['complexity'], 4)
        self.assertEqual(preview['provider_order'][0], 'nvidia')
        result = self.manager.generate(text)
        self.assertEqual(result['jubi_provider_route']['provider'], 'nvidia')
        self.assertTrue(result['jubi_provider_route']['cloud'])

    def test_high_privacy_request_stays_local_even_in_cloud_boost(self):
        self.manager.set_mode('cloud_boost')
        result = self.manager.generate('My API key is SECRET-123456789. Explain how to store this credential securely.')
        self.assertEqual(result['jubi_provider_route']['provider'], 'ollama')
        self.assertTrue(all(not provider.calls for provider in self.cloud.values()))

    def test_explicit_cloud_is_blocked_for_high_privacy(self):
        self.manager.set_mode('cloud_boost')
        with self.assertRaises(PermissionError):
            self.manager.generate('Use this password secret-123456 to answer.', provider='openrouter')

    def test_cloud_failure_falls_back_to_local(self):
        self.manager.set_mode('cloud_boost')
        for provider in self.cloud.values():
            provider.fail = True
        result = self.manager.generate('Write a short public project plan for a demo website.')
        self.assertEqual(result['response'], 'LOCAL_OK')
        self.assertEqual(result['jubi_provider_route']['provider'], 'ollama')
        self.assertTrue(result['jubi_provider_route']['fallback_errors'])

    def test_provider_outcomes_persist_without_prompt_text(self):
        self.manager.set_mode('cloud_boost')
        secret_prompt = 'Create a public architecture overview with enough detail for a technical presentation.'
        result = self.manager.generate(secret_prompt)
        self.assertTrue(result['jubi_provider_route']['cloud'])
        rows = self.manager.recent_requests()
        self.assertEqual(len(rows), 1)
        self.assertNotIn(secret_prompt, json.dumps(rows))
        self.assertEqual(len(rows[0]['prompt_hash']), 64)
        perf = self.manager.performance()
        self.assertTrue(any(x['provider'] == 'nvidia' and x['successes'] == 1 for x in perf))

    def test_explicit_provider_routes_only_to_that_provider(self):
        result = self.manager.generate('Write a friendly greeting.', provider='huggingface')
        self.assertEqual(result['jubi_provider_route']['provider'], 'huggingface')
        self.assertEqual(len(self.cloud['huggingface'].calls), 1)
        self.assertEqual(len(self.cloud['nvidia'].calls), 0)
        self.assertEqual(len(self.cloud['openrouter'].calls), 0)

    def test_openai_compatible_model_normalization(self):
        provider = OpenAICompatibleProvider(
            'openrouter', 'OpenRouter', 'https://example.invalid/v1', self.credentials
        )
        provider._json = lambda *args, **kwargs: {
            'data': [
                {'id': 'demo/free', 'name': 'Demo', 'context_length': 1000, 'pricing': {'prompt': '0', 'completion': '0'}},
                {'id': 'demo/paid', 'name': 'Paid', 'pricing': {'prompt': '0.1', 'completion': '0.2'}},
            ]
        }
        result = provider.list_models()
        self.assertEqual(result['count'], 2)
        self.assertTrue(result['models'][0]['free'])
        self.assertFalse(result['models'][1]['free'])

    def test_credential_status_never_exposes_environment_secret(self):
        store = CredentialStore(Path(self.tmp.name) / 'credentials.json')
        with patch.dict(os.environ, {'OPENROUTER_API_KEY': 'super-secret-openrouter-value'}, clear=False):
            status = store.status('openrouter')
            self.assertTrue(status['configured'])
            self.assertNotIn('super-secret-openrouter-value', json.dumps(status))
            value, source = store.get('openrouter')
            self.assertEqual(value, 'super-secret-openrouter-value')
            self.assertIn('environment:', source)

    @unittest.skipUnless(os.name == 'nt', 'Windows DPAPI test')
    def test_windows_dpapi_round_trip_does_not_write_plaintext(self):
        path = Path(self.tmp.name) / 'credentials.json'
        store = CredentialStore(path)
        secret = 'windows-dpapi-test-secret-123456789'
        with patch.dict(os.environ, {'OPENROUTER_API_KEY': ''}, clear=False):
            store.set('openrouter', secret)
            raw = path.read_text(encoding='utf-8')
            self.assertNotIn(secret, raw)
            value, source = store.get('openrouter')
            self.assertEqual(value, secret)
            self.assertEqual(source, 'windows-dpapi')


if __name__ == '__main__':
    unittest.main(verbosity=2)
