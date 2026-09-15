"""The inference transport boundary, shared by every enabled Jubi provider.

M0 deliberately has no network-inference grant. Legacy hybrid settings cannot
weaken this boundary. Optional free services require a separate reviewed grant
implementation; an API key or a model name is never a grant.
"""
from __future__ import annotations

import json
import urllib.parse
import urllib.request
from dataclasses import dataclass


class ProviderPolicyError(RuntimeError):
    pass


@dataclass(frozen=True)
class ProviderPolicy:
    revision: str = 'local-only-v1'
    mode: str = 'local_only'

    def authorize(self, provider: str, url: str) -> None:
        parsed = urllib.parse.urlsplit(url)
        try:
            port = parsed.port
        except ValueError as exc:
            raise ProviderPolicyError('Invalid inference endpoint port') from exc
        if (self.mode != 'local_only' or provider != 'ollama'
                or parsed.scheme != 'http'
                or parsed.hostname not in {'127.0.0.1', '::1'}
                or parsed.path not in {'/api/tags', '/api/show', '/api/version', '/api/ps',
                                       '/api/generate', '/api/chat', '/api/embed',
                                       '/api/embeddings', '/v1/chat/completions'}
                or parsed.username or parsed.password or parsed.query or parsed.fragment
                or port is None or not 1 <= port <= 65535):
            raise ProviderPolicyError('Local Only permits inference only through local Ollama')

    @staticmethod
    def check_model_name(model: str) -> None:
        if not isinstance(model, str) or not model.strip() or 'cloud' in model.lower():
            raise ProviderPolicyError('Local Only rejects missing models and cloud aliases')

    def check_model(self, model: str, metadata: dict) -> None:
        self.check_model_name(model)
        # A renamed remote alias must not become local merely by losing "cloud".
        if (not isinstance(metadata, dict)
                or metadata.get('remote_host') or metadata.get('remote_model')
                or not isinstance(metadata.get('details'), dict)
                or metadata['details'].get('format') != 'gguf'
                or not isinstance(metadata.get('model_info'), dict)
                or not metadata['model_info']):
            raise ProviderPolicyError('Model has no verified local GGUF metadata or is remote')


LOCAL_ONLY = ProviderPolicy()


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ProviderPolicyError('Inference redirects are forbidden')


class InferenceTransport:
    """Validate before opening a socket or releasing any prompt/credential.

    Ollama's metadata endpoints are the only unauthenticated preflight. Inference
    checks fresh installed-model metadata on every call, including embeddings and
    OpenAI-compatible calls. Proxy environment variables and redirects are ignored.
    This is an application boundary, not a sandbox for arbitrary Python source.
    """
    _inference_paths = frozenset({'/api/generate', '/api/chat', '/api/embed',
                                  '/api/embeddings', '/v1/chat/completions'})
    _metadata_paths = frozenset({'/api/tags', '/api/show', '/api/version', '/api/ps'})

    def __init__(self, base_url: str, provider: str = 'ollama'):
        self.base_url = str(base_url).rstrip('/')
        self.provider = provider

    def _send(self, path, body, timeout, headers=None):
        # Recheck immediately before I/O, including private preflight sends.
        LOCAL_ONLY.authorize(self.provider, self.base_url + path)
        data = None if body is None else json.dumps(body, allow_nan=False).encode('utf-8')
        req = urllib.request.Request(self.base_url + path, data,
                                     {'Content-Type': 'application/json', **(headers or {})})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
        with opener.open(req, timeout=timeout) as response:
            return json.load(response)

    def json(self, path: str, body=None, timeout: int = 30, headers=None):
        LOCAL_ONLY.authorize(self.provider, self.base_url + path)
        if path in self._inference_paths:
            if not isinstance(body, dict):
                raise ProviderPolicyError('Inference requires an explicit model')
            model = body.get('model')
            LOCAL_ONLY.check_model_name(model)
            tags = self._send('/api/tags', None, min(timeout, 10))
            if model not in {m.get('name') for m in tags.get('models', []) if isinstance(m, dict)}:
                raise ProviderPolicyError('Selected model is not installed locally')
            metadata = self._send('/api/show', {'model': model}, min(timeout, 10))
            LOCAL_ONLY.check_model(model, metadata)
            if body.get('stream'):
                raise ProviderPolicyError('Use non-streaming inference through this transport')
        elif path not in self._metadata_paths:
            raise ProviderPolicyError('Endpoint is outside the inference allowlist')
        return self._send(path, body, timeout, headers)
