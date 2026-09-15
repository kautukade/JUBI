"""Real loopback HTTP tests: rejected prompts must never reach a socket."""
from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sarus.core.provider_policy import InferenceTransport, ProviderPolicyError, LOCAL_ONLY
from sarus.core.models import OllamaRouter
from sarus.core.providers import OpenAICompatibleProvider, ProviderManager
from sarus.core.brain import BrainRouter


class ProviderPolicyTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.metadata = {'details': {'format': 'gguf'}, 'model_info': {'test.architecture': 'test'}}
        self.redirect = False
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.do_POST()

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
                owner.calls.append((self.path, body))
                if owner.redirect:
                    self.send_response(307)
                    self.send_header('Location', 'https://example.com/inference')
                    self.end_headers()
                    return
                payload = {
                    '/api/tags': {'models': [{'name': 'local:latest'}, {'name': 'renamed:latest'}]},
                    '/api/show': owner.metadata,
                    '/api/generate': {'response': 'local answer'},
                    '/api/embed': {'embeddings': [[1.0, 0.0]]},
                }.get(self.path, {'choices': [{'message': {'content': 'local answer'}}]})
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps(payload).encode())

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f'http://127.0.0.1:{self.server.server_port}'
        self.transport = InferenceTransport(self.base)
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        config = self.root / 'models.json'
        config.write_text('{"general": ["local:latest"]}')
        self.models = OllamaRouter(config, self.base)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def test_real_local_generation_and_embedding(self):
        with patch.dict('os.environ', {'HTTP_PROXY': 'http://127.0.0.1:1'}):
            self.assertEqual(self.models.generate('hello', model='local:latest')['response'], 'local answer')
            self.assertEqual(self.models.embed('hello', model='local:latest'), [1.0, 0.0])

    def test_cloud_alias_rejected_before_any_request(self):
        for path in ('/api/generate', '/api/embed', '/v1/chat/completions'):
            with self.subTest(path=path), self.assertRaises(ProviderPolicyError):
                self.transport.json(path, {'model': 'qwen3-coder:480b-cloud', 'prompt': 'private'})
        self.assertEqual(self.calls, [])

    def test_renamed_remote_alias_never_receives_prompt(self):
        for metadata in ({'remote_host': 'https://ollama.com', **self.metadata},
                         {'remote_model': 'remote', **self.metadata}, {}, {'details': {}}):
            self.metadata = metadata
            with self.subTest(metadata=metadata), self.assertRaises(ProviderPolicyError):
                self.models.generate('private', model='renamed:latest')
        self.assertTrue(all(path in {'/api/tags', '/api/show'} for path, _ in self.calls))
        self.assertNotIn('private', json.dumps(self.calls))

    def test_uninstalled_model_cannot_trigger_remote_or_download(self):
        with self.assertRaises(ProviderPolicyError):
            self.models.generate('private', model='missing:latest')
        self.assertEqual([p for p, _ in self.calls], ['/api/tags'])
        for path in ('/api/pull', '/api/create', '/api/delete', '/api/tags?override=1', '//evil/generate'):
            with self.subTest(path=path), self.assertRaises(ProviderPolicyError):
                self.transport.json(path, {'model': 'local:latest'})

    def test_remote_endpoints_and_direct_source_provider_calls_denied(self):
        for base in ('https://ollama.com', 'http://192.168.1.2:11434',
                     'http://localhost.evil:11434', 'http://user@127.0.0.1:11434',
                     'http://127.0.0.1:11434/proxy'):
            with self.subTest(base=base), self.assertRaises(ProviderPolicyError):
                InferenceTransport(base).json('/api/generate', {'model': 'local:latest'})
        credentials = Mock()
        for provider in ('openrouter', 'nvidia', 'huggingface'):
            client = OpenAICompatibleProvider(provider, provider, self.base, credentials)
            with self.assertRaises(ProviderPolicyError):
                client.chat('private', 'anything', '')
            with self.assertRaises(ProviderPolicyError):
                client.list_models()
        credentials.get.assert_not_called()
        self.assertEqual(self.calls, [])

    def test_redirects_never_followed(self):
        self.redirect = True
        with self.assertRaises(ProviderPolicyError):
            self.transport.json('/api/tags')
        self.assertEqual(len(self.calls), 1)

    def test_new_threads_and_recreated_transports_keep_policy(self):
        errors = []
        def worker():
            try:
                InferenceTransport(self.base).json('/api/chat', {'model': 'renamed-cloud'})
            except ProviderPolicyError:
                errors.append(True)
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        self.assertEqual(errors, [True])
        self.assertEqual(self.calls, [])

    def test_saved_hybrid_setting_cannot_enable_fallback_after_restart(self):
        brain = BrainRouter(self.root / 'state.db', self.models, self.root / 'missing.json')
        manager = ProviderManager(self.root / 'state.db', brain, self.root / 'providers.json', credentials=Mock())
        manager._set_setting('mode', 'hybrid_auto')
        resumed = ProviderManager(self.root / 'state.db', brain, self.root / 'providers.json', credentials=Mock())
        self.assertEqual(resumed.mode(), 'local_only')
        with self.assertRaises(ValueError):
            resumed.set_mode('cloud_boost')
        with self.assertRaises(PermissionError):
            resumed.generate('private', provider='openrouter')
        with patch.object(brain, 'generate', side_effect=RuntimeError('local failed')), \
                patch.object(resumed, '_cloud_attempts') as cloud:
            with self.assertRaises(RuntimeError):
                resumed.generate('private')
            cloud.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
