from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


def _normalize_local_ollama_url(value: str | None) -> str | None:
    raw = str(value or '').strip()
    if not raw:
        return None
    if '://' not in raw:
        raw = 'http://' + raw
    try:
        parsed = urllib.parse.urlparse(raw)
    except Exception:
        return None
    if parsed.scheme.lower() != 'http':
        return None
    host = (parsed.hostname or '').lower()
    if host in {'localhost', '0.0.0.0', '::', '::1'}:
        host = '127.0.0.1'
    if host != '127.0.0.1':
        return None
    try:
        port = parsed.port or 11434
    except ValueError:
        return None
    if not 1 <= int(port) <= 65535:
        return None
    return f'http://127.0.0.1:{int(port)}'


def _runtime_ollama_url() -> str | None:
    candidates: list[str | None] = [os.environ.get('JUBI_OLLAMA_URL')]
    local_appdata = os.environ.get('LOCALAPPDATA')
    if local_appdata:
        runtime_path = Path(local_appdata) / 'Jubi' / 'runtime.json'
        try:
            runtime = json.loads(runtime_path.read_text(encoding='utf-8-sig'))
            if isinstance(runtime, dict):
                candidates.append(runtime.get('ollama_base_url'))
        except (OSError, ValueError, TypeError):
            pass
    candidates.extend([os.environ.get('OLLAMA_HOST'), 'http://127.0.0.1:11434'])
    for candidate in candidates:
        normalized = _normalize_local_ollama_url(candidate)
        if normalized:
            return normalized
    return None


class OllamaRouter:
    def __init__(self, config: Path, base_url=None):
        self.config_path = config
        self.cfg = json.loads(config.read_text(encoding='utf-8'))
        self.base = (base_url or _runtime_ollama_url() or 'http://127.0.0.1:11434').rstrip('/')

    def _json(self, path, body=None, timeout=10):
        data = None if body is None else json.dumps(body).encode('utf-8')
        headers = {'Content-Type': 'application/json'} if data else {}
        req = urllib.request.Request(self.base + path, data, headers)
        # Local Ollama requests must never be sent through a configured HTTP proxy.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=timeout) as r:
            return json.load(r)

    @staticmethod
    def _kind(name: str) -> str:
        low = str(name).lower()
        if ':cloud' in low or low.endswith('-cloud') or 'cloud' in low:
            return 'cloud-through-ollama'
        if any(x in low for x in ('embed', 'nomic-embed', 'bge-', 'e5-')):
            return 'embedding'
        if any(x in low for x in ('vl:', '-vl', 'vision')):
            return 'vision'
        if any(x in low for x in ('coder', 'code')):
            return 'coding'
        return 'general'

    def list_models(self):
        try:
            raw = self._json('/api/tags', timeout=3)
            items = []
            for model in raw.get('models', []):
                if not isinstance(model, dict) or not model.get('name'):
                    continue
                item = {
                    'name': model['name'],
                    'kind': self._kind(model['name']),
                    'size': model.get('size'),
                    'modified_at': model.get('modified_at'),
                    'details': model.get('details') or {},
                }
                items.append(item)
            return {
                'online': True,
                'models': [x['name'] for x in items],
                'items': items,
            }
        except Exception as e:
            return {'online': False, 'models': [], 'items': [], 'error': str(e)}

    def choose(self, task_type='general'):
        status = self.list_models()
        if not status.get('online'):
            return None
        installed = list(status.get('models', []))
        installed_set = set(installed)
        candidates = list(self.cfg.get(task_type, self.cfg.get('general', [])))
        for model in candidates:
            if model in installed_set:
                return model

        # Never return a configured-but-missing model. Fallback is restricted to
        # a model that Ollama actually reports as installed and role-compatible.
        desired_kind = {
            'coding': 'coding',
            'vision': 'vision',
            'embedding': 'embedding',
        }.get(task_type, 'general')
        for item in status.get('items', []):
            if item.get('kind') == desired_kind:
                return item['name']
        if task_type not in {'embedding', 'vision'}:
            for item in status.get('items', []):
                if item.get('kind') in {'general', 'coding'}:
                    return item['name']
        return None

    def generate(
        self,
        prompt: str,
        task_type='general',
        system='You are Jubi, a local AI orchestrator.',
        model=None,
        timeout=300,
        images=None,
    ):
        model = model or self.choose(task_type)
        if not model:
            status = self.list_models()
            if not status.get('online'):
                raise RuntimeError(
                    f'Ollama is not reachable at {self.base}. Start Ollama and try again.'
                )
            raise RuntimeError(
                f'No installed Ollama model is compatible with task type {task_type!r}. '
                'Refresh the model list or install/configure a suitable model.'
            )
        payload = {'model': model, 'prompt': prompt, 'system': system, 'stream': False}
        if images:
            if task_type != 'vision':
                raise ValueError('image inputs are only supported for vision tasks')
            if not isinstance(images, list) or len(images) > 4:
                raise ValueError('images must be a list with at most 4 items')
            payload['images'] = [str(x) for x in images]
        try:
            return self._json('/api/generate', payload, timeout)
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                raise RuntimeError(
                    f'The selected Ollama model {model!r} is not available. Refresh models and try again.'
                ) from exc
            raise

    def generate_text(self, *a, **kw):
        return str(self.generate(*a, **kw).get('response', '')).strip()

    def embed(self, text, model=None):
        model = model or self.choose('embedding')
        if not model:
            raise RuntimeError('No installed Ollama embedding model is available')
        d = self._json('/api/embed', {'model': model, 'input': text}, 120)
        return (d.get('embeddings') or [None])[0]
