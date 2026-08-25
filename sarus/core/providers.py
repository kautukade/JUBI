from __future__ import annotations

import hashlib
import json
import math
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

from .credentials import CredentialStore
from .database import read_connection, transaction


PROVIDER_NAMES = ('openrouter', 'nvidia', 'huggingface')
PROVIDER_MODES = ('local_only', 'hybrid_auto', 'cloud_boost')


class OpenAICompatibleProvider:
    """Small dependency-free client for an OpenAI-compatible chat provider."""

    def __init__(self, provider_id: str, label: str, base_url: str, credentials: CredentialStore, headers=None):
        self.provider_id = provider_id
        self.label = label
        self.base_url = str(base_url).rstrip('/')
        self.credentials = credentials
        self.extra_headers = dict(headers or {})

    def _credential(self) -> tuple[str, str]:
        value, source = self.credentials.get(self.provider_id)
        if not value:
            raise RuntimeError(f'{self.label} is not configured with an API credential')
        return value, source

    def _json(self, path: str, body=None, timeout: int = 20) -> dict:
        token, _ = self._credential()
        data = None if body is None else json.dumps(body).encode('utf-8')
        headers = {
            'Accept': 'application/json',
            'Authorization': f'Bearer {token}',
            **self.extra_headers,
        }
        if data is not None:
            headers['Content-Type'] = 'application/json'
        req = urllib.request.Request(self.base_url + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as response:
                raw = response.read()
                return json.loads(raw.decode('utf-8')) if raw else {}
        except urllib.error.HTTPError as exc:
            detail = ''
            try:
                raw = exc.read(8192).decode('utf-8', errors='replace')
                parsed = json.loads(raw)
                if isinstance(parsed, dict):
                    error = parsed.get('error')
                    if isinstance(error, dict):
                        detail = str(error.get('message') or error.get('code') or '')
                    else:
                        detail = str(error or parsed.get('message') or '')
                else:
                    detail = raw
            except Exception:
                detail = ''
            detail = detail.strip().replace('\n', ' ')[:500]
            suffix = f': {detail}' if detail else ''
            if exc.code in (401, 403):
                raise RuntimeError(f'{self.label} rejected the configured credential{suffix}') from exc
            if exc.code == 429:
                raise RuntimeError(f'{self.label} rate limit or quota reached{suffix}') from exc
            raise RuntimeError(f'{self.label} API returned HTTP {exc.code}{suffix}') from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f'{self.label} is unreachable: {exc.reason}') from exc

    @staticmethod
    def _pricing_zero(pricing) -> bool:
        if not isinstance(pricing, dict):
            return False
        values = []
        for key in ('prompt', 'completion', 'input', 'output'):
            if key not in pricing:
                continue
            try:
                values.append(float(pricing[key]))
            except (TypeError, ValueError):
                return False
        return bool(values) and all(v == 0 for v in values)

    def list_models(self, timeout: int = 20) -> dict:
        raw = self._json('/models', timeout=timeout)
        items = []
        for model in raw.get('data', []) if isinstance(raw, dict) else []:
            if not isinstance(model, dict) or not model.get('id'):
                continue
            providers = model.get('providers') or []
            pricing = model.get('pricing') or {}
            if not pricing and providers and isinstance(providers[0], dict):
                pricing = providers[0].get('pricing') or {}
            architecture = model.get('architecture') or {}
            items.append(
                {
                    'id': str(model['id']),
                    'name': str(model.get('name') or model['id']),
                    'context_length': model.get('context_length'),
                    'pricing': pricing,
                    'free': self._pricing_zero(pricing) or str(model['id']).endswith(':free'),
                    'architecture': architecture,
                    'providers': providers[:12] if isinstance(providers, list) else [],
                }
            )
        return {'provider': self.provider_id, 'online': True, 'models': items, 'count': len(items)}

    def health(self, timeout: int = 12) -> dict:
        credential = self.credentials.status(self.provider_id)
        if not credential['configured']:
            return {
                'provider': self.provider_id,
                'label': self.label,
                'configured': False,
                'online': False,
                'status': 'unconfigured',
                'credential_source': credential['source'],
            }
        started = time.perf_counter()
        try:
            models = self.list_models(timeout=timeout)
            return {
                'provider': self.provider_id,
                'label': self.label,
                'configured': True,
                'online': True,
                'status': 'connected',
                'credential_source': credential['source'],
                'models': models['count'],
                'latency_ms': round((time.perf_counter() - started) * 1000.0, 2),
            }
        except Exception as exc:
            return {
                'provider': self.provider_id,
                'label': self.label,
                'configured': True,
                'online': False,
                'status': 'error',
                'credential_source': credential['source'],
                'error': str(exc)[:700],
                'latency_ms': round((time.perf_counter() - started) * 1000.0, 2),
            }

    def chat(self, prompt: str, model: str, system: str, timeout: int = 180) -> dict:
        payload = {
            'model': model,
            'messages': [
                {'role': 'system', 'content': system},
                {'role': 'user', 'content': prompt},
            ],
            'stream': False,
        }
        raw = self._json('/chat/completions', payload, timeout=timeout)
        choices = raw.get('choices') or [] if isinstance(raw, dict) else []
        if not choices or not isinstance(choices[0], dict):
            raise RuntimeError(f'{self.label} returned no chat completion choice')
        message = choices[0].get('message') or {}
        content = message.get('content') if isinstance(message, dict) else None
        if isinstance(content, list):
            parts = []
            for item in content:
                if isinstance(item, dict) and item.get('text'):
                    parts.append(str(item['text']))
            content = '\n'.join(parts)
        text = str(content or '').strip()
        if not text:
            raise RuntimeError(f'{self.label} returned an empty chat response')
        return {
            'response': text,
            'model': str(raw.get('model') or model),
            'provider': self.provider_id,
            'usage': raw.get('usage') or {},
        }


class ProviderManager:
    """Jubi's optional cloud provider layer wrapped around the Local Brain.

    Modes are intentionally explicit:
    - ``local_only``: never send prompts to cloud providers.
    - ``hybrid_auto``: simple work starts local; complex work may start cloud;
      either side can fall back to the other when available.
    - ``cloud_boost``: non-sensitive prompts prefer configured cloud providers,
      then fall back to local Ollama.

    High-privacy requests never leave the machine automatically. Credentials are
    handled by ``CredentialStore`` and never written to the Jubi SQLite DB.
    """

    def __init__(self, db: Path, brain, config_path: Path, event_bus=None, credentials=None):
        self.db = db
        self.brain = brain
        self.event_bus = event_bus
        self.config_path = Path(config_path)
        self.cfg = self._load_config(self.config_path)
        self.credentials = credentials or CredentialStore()
        self.providers = {
            'openrouter': OpenAICompatibleProvider(
                'openrouter',
                'OpenRouter',
                self.cfg['providers']['openrouter']['base_url'],
                self.credentials,
                headers={'HTTP-Referer': 'http://127.0.0.1', 'X-OpenRouter-Title': 'Jubi'},
            ),
            'nvidia': OpenAICompatibleProvider(
                'nvidia', 'NVIDIA NIM', self.cfg['providers']['nvidia']['base_url'], self.credentials
            ),
            'huggingface': OpenAICompatibleProvider(
                'huggingface',
                'Hugging Face Inference Providers',
                self.cfg['providers']['huggingface']['base_url'],
                self.credentials,
            ),
        }
        self._model_cache: dict[str, tuple[float, dict]] = {}
        self._init_db()

    @staticmethod
    def _load_config(path: Path) -> dict:
        defaults = {
            'mode': 'local_only',
            'provider_order': ['nvidia', 'openrouter', 'huggingface'],
            'hybrid_cloud_complexity_threshold': 4,
            'max_cloud_provider_attempts': 3,
            'max_models_per_provider_attempt': 2,
            'model_cache_seconds': 300,
            'allow_high_privacy_cloud': False,
            'providers': {
                'openrouter': {
                    'enabled': True,
                    'base_url': 'https://openrouter.ai/api/v1',
                    'models': {
                        'general': ['openrouter/free'],
                        'coding': ['openrouter/free'],
                        'research': ['openrouter/free'],
                        'planning': ['openrouter/free'],
                        'document': ['openrouter/free'],
                        'system': ['openrouter/free'],
                        'vision': ['openrouter/free'],
                    },
                },
                'nvidia': {
                    'enabled': True,
                    'base_url': 'https://integrate.api.nvidia.com/v1',
                    'models': {
                        'general': ['nvidia/nemotron-3-super-120b-a12b', 'meta/llama-3.3-70b-instruct'],
                        'coding': ['qwen/qwen3-coder-480b-a35b-instruct', 'nvidia/nemotron-3-super-120b-a12b'],
                        'research': ['nvidia/nemotron-3-super-120b-a12b'],
                        'planning': ['nvidia/nemotron-3-super-120b-a12b'],
                        'document': ['meta/llama-3.3-70b-instruct', 'nvidia/nemotron-3-super-120b-a12b'],
                        'system': ['nvidia/nemotron-3-super-120b-a12b'],
                        'vision': [],
                    },
                },
                'huggingface': {
                    'enabled': True,
                    'base_url': 'https://router.huggingface.co/v1',
                    'models': {
                        'general': ['openai/gpt-oss-20b:fastest'],
                        'coding': ['Qwen/Qwen3-Coder-480B-A35B-Instruct:fastest', 'openai/gpt-oss-20b:fastest'],
                        'research': ['openai/gpt-oss-20b:fastest'],
                        'planning': ['openai/gpt-oss-20b:fastest'],
                        'document': ['openai/gpt-oss-20b:fastest'],
                        'system': ['openai/gpt-oss-20b:fastest'],
                        'vision': [],
                    },
                },
            },
        }
        try:
            loaded = json.loads(path.read_text(encoding='utf-8'))
            if isinstance(loaded, dict):
                for key in (
                    'mode', 'provider_order', 'hybrid_cloud_complexity_threshold',
                    'max_cloud_provider_attempts', 'max_models_per_provider_attempt',
                    'model_cache_seconds', 'allow_high_privacy_cloud',
                ):
                    if key in loaded:
                        defaults[key] = loaded[key]
                for provider, value in (loaded.get('providers') or {}).items():
                    if provider in defaults['providers'] and isinstance(value, dict):
                        defaults['providers'][provider].update(value)
        except Exception:
            pass
        return defaults

    def _init_db(self):
        with transaction(self.db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS provider_settings("
                "key TEXT PRIMARY KEY,value TEXT NOT NULL,updated REAL NOT NULL)"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS provider_performance("
                "provider TEXT,model TEXT,task_type TEXT,successes INTEGER DEFAULT 0,"
                "failures INTEGER DEFAULT 0,total_latency_ms REAL DEFAULT 0,"
                "last_latency_ms REAL DEFAULT 0,last_status TEXT DEFAULT '',updated REAL DEFAULT 0,"
                "PRIMARY KEY(provider,model,task_type))"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS provider_requests("
                "id TEXT PRIMARY KEY,ts REAL,prompt_hash TEXT,prompt_length INTEGER,"
                "provider TEXT,model TEXT,task_type TEXT,complexity INTEGER,privacy TEXT,mode TEXT,"
                "status TEXT,latency_ms REAL,error TEXT)"
            )

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is None:
            return
        try:
            self.event_bus.emit(kind, payload)
        except Exception:
            pass

    def _get_setting(self, key: str, default=None):
        with read_connection(self.db) as c:
            row = c.execute('SELECT value FROM provider_settings WHERE key=?', (key,)).fetchone()
        if not row:
            return default
        try:
            return json.loads(row[0])
        except Exception:
            return row[0]

    def _set_setting(self, key: str, value):
        encoded = json.dumps(value, ensure_ascii=False)
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO provider_settings(key,value,updated) VALUES(?,?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated=excluded.updated",
                (key, encoded, time.time()),
            )

    def mode(self) -> str:
        value = str(self._get_setting('mode', self.cfg.get('mode', 'local_only')))
        return value if value in PROVIDER_MODES else 'local_only'

    def set_mode(self, mode: str) -> dict:
        value = str(mode or '').strip().lower()
        if value not in PROVIDER_MODES:
            raise ValueError(f'unsupported provider mode: {mode!r}')
        self._set_setting('mode', value)
        self._emit('PROVIDER_MODE_CHANGED', {'mode': value})
        return {'mode': value}

    def set_default_model(self, provider: str, task_type: str, model: str) -> dict:
        provider = self._provider_id(provider)
        task = str(task_type or 'general').strip().lower() or 'general'
        value = str(model or '').strip()
        if not value:
            raise ValueError('model is required')
        self._set_setting(f'default_model:{provider}:{task}', value)
        return {'provider': provider, 'task_type': task, 'model': value}

    def _default_model(self, provider: str, task_type: str) -> str | None:
        value = self._get_setting(f'default_model:{provider}:{task_type}')
        return str(value) if value else None

    @staticmethod
    def _provider_id(provider: str) -> str:
        value = str(provider or '').strip().lower()
        if value not in PROVIDER_NAMES:
            raise ValueError(f'unsupported cloud provider: {provider!r}')
        return value

    def save_credential(self, provider: str, secret: str) -> dict:
        provider = self._provider_id(provider)
        result = self.credentials.set(provider, secret)
        self._model_cache.pop(provider, None)
        self._emit('PROVIDER_CREDENTIAL_SAVED', {'provider': provider, 'source': result['source']})
        return result

    def delete_credential(self, provider: str) -> dict:
        provider = self._provider_id(provider)
        result = self.credentials.delete(provider)
        self._model_cache.pop(provider, None)
        self._emit('PROVIDER_CREDENTIAL_REMOVED', {'provider': provider, 'effective_source': result['source']})
        return result

    def _enabled(self, provider: str) -> bool:
        return bool((self.cfg.get('providers') or {}).get(provider, {}).get('enabled', True))

    def models(self, provider: str, force: bool = False) -> dict:
        provider = self._provider_id(provider)
        status = self.credentials.status(provider)
        if not status['configured']:
            return {
                'provider': provider,
                'configured': False,
                'online': False,
                'models': [],
                'count': 0,
                'error': 'provider credential is not configured',
            }
        now = time.monotonic()
        cached = self._model_cache.get(provider)
        ttl = max(10, int(self.cfg.get('model_cache_seconds', 300)))
        if cached and not force and now - cached[0] < ttl:
            return dict(cached[1])
        try:
            result = self.providers[provider].list_models()
            result['configured'] = True
            self._model_cache[provider] = (now, dict(result))
            return result
        except Exception as exc:
            return {
                'provider': provider,
                'configured': True,
                'online': False,
                'models': [],
                'count': 0,
                'error': str(exc)[:700],
            }

    def validate(self, provider: str) -> dict:
        provider = self._provider_id(provider)
        result = self.providers[provider].health()
        self._model_cache.pop(provider, None)
        self._emit(
            'PROVIDER_VALIDATED',
            {'provider': provider, 'status': result.get('status'), 'online': bool(result.get('online'))},
        )
        return result

    def _performance_rows(self) -> list[dict]:
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT provider,model,task_type,successes,failures,total_latency_ms,"
                "last_latency_ms,last_status,updated FROM provider_performance"
            ).fetchall()
        out = []
        for r in rows:
            successes = int(r[3] or 0)
            failures = int(r[4] or 0)
            attempts = successes + failures
            out.append(
                {
                    'provider': r[0], 'model': r[1], 'task_type': r[2],
                    'successes': successes, 'failures': failures, 'attempts': attempts,
                    'success_rate': successes / attempts if attempts else None,
                    'avg_success_latency_ms': float(r[5] or 0) / successes if successes else None,
                    'last_latency_ms': float(r[6] or 0), 'last_status': r[7] or '',
                    'updated': float(r[8] or 0),
                }
            )
        out.sort(key=lambda x: (-x['successes'], x['failures'], x['provider'], x['model'], x['task_type']))
        return out

    def performance(self) -> list[dict]:
        return self._performance_rows()

    def recent_requests(self, limit: int = 50) -> list[dict]:
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,prompt_hash,prompt_length,provider,model,task_type,complexity,privacy,"
                "mode,status,latency_ms,error FROM provider_requests ORDER BY ts DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'prompt_hash': r[2], 'prompt_length': r[3],
                'provider': r[4], 'model': r[5], 'task_type': r[6], 'complexity': r[7],
                'privacy': r[8], 'mode': r[9], 'status': r[10], 'latency_ms': r[11], 'error': r[12],
            }
            for r in rows
        ]

    def _record_outcome(self, provider: str, model: str, task_type: str, ok: bool, latency_ms: float):
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO provider_performance(provider,model,task_type,successes,failures,total_latency_ms,"
                "last_latency_ms,last_status,updated) VALUES(?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(provider,model,task_type) DO UPDATE SET "
                "successes=successes+excluded.successes,failures=failures+excluded.failures,"
                "total_latency_ms=total_latency_ms+excluded.total_latency_ms,"
                "last_latency_ms=excluded.last_latency_ms,last_status=excluded.last_status,updated=excluded.updated",
                (
                    provider, model, task_type, 1 if ok else 0, 0 if ok else 1,
                    float(latency_ms) if ok else 0.0, float(latency_ms),
                    'success' if ok else 'failure', time.time(),
                ),
            )

    def _record_request(self, request_id: str, prompt: str, classification: dict, mode: str,
                        provider: str, model: str, status: str, latency_ms: float, error: str = ''):
        with transaction(self.db) as c:
            c.execute(
                "INSERT OR REPLACE INTO provider_requests("
                "id,ts,prompt_hash,prompt_length,provider,model,task_type,complexity,privacy,mode,status,latency_ms,error) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    request_id, time.time(), hashlib.sha256(prompt.encode('utf-8', errors='replace')).hexdigest(),
                    len(prompt), provider, model, classification['task_type'], int(classification['complexity']),
                    classification['privacy'], mode, status, float(latency_ms), str(error or '')[:2000],
                ),
            )

    def _provider_score(self, provider: str, task_type: str) -> float:
        order = [p for p in self.cfg.get('provider_order', []) if p in PROVIDER_NAMES]
        base = 100.0 - (order.index(provider) * 8.0 if provider in order else 30.0)
        rows = [x for x in self._performance_rows() if x['provider'] == provider and x['task_type'] == task_type]
        attempts = sum(x['attempts'] for x in rows)
        successes = sum(x['successes'] for x in rows)
        if attempts:
            rate = successes / attempts
            confidence = min(1.0, math.log2(attempts + 1) / 4.0)
            base += (rate - 0.5) * 30.0 * confidence
        return base

    def _ordered_cloud_providers(self, task_type: str, explicit: str | None = None) -> list[str]:
        if explicit:
            provider = self._provider_id(explicit)
            return [provider] if self._enabled(provider) and self.credentials.status(provider)['configured'] else []
        available = [
            p for p in PROVIDER_NAMES
            if self._enabled(p) and self.credentials.status(p)['configured']
        ]
        available.sort(key=lambda p: (-self._provider_score(p, task_type), p))
        return available

    @staticmethod
    def _looks_coding(model: dict) -> bool:
        text = f"{model.get('id', '')} {model.get('name', '')}".lower()
        return any(x in text for x in ('coder', 'code', 'devstral', 'codestral'))

    @staticmethod
    def _looks_vision(model: dict) -> bool:
        architecture = model.get('architecture') or {}
        inputs = architecture.get('input_modalities') or []
        text = f"{model.get('id', '')} {model.get('name', '')}".lower()
        return 'image' in inputs or any(x in text for x in ('vision', '-vl', 'vl-', 'multimodal'))

    def _model_candidates(self, provider: str, task_type: str, requested_model: str | None = None) -> list[str]:
        if requested_model:
            return [str(requested_model).strip()]
        configured = []
        saved = self._default_model(provider, task_type)
        if saved:
            configured.append(saved)
        provider_cfg = (self.cfg.get('providers') or {}).get(provider, {})
        model_cfg = provider_cfg.get('models') or {}
        configured.extend(model_cfg.get(task_type) or model_cfg.get('general') or [])

        seen = set()
        configured = [m for m in configured if m and not (m in seen or seen.add(m))]
        # OpenRouter's free router is a supported alias and need not appear in the
        # ordinary model catalog, so preserve it even when discovery succeeds.
        if provider == 'openrouter' and 'openrouter/free' not in configured:
            configured.append('openrouter/free')

        discovered = self.models(provider, force=False)
        live = discovered.get('models') or [] if discovered.get('online') else []
        live_ids = {str(x.get('id')) for x in live if isinstance(x, dict) and x.get('id')}
        candidates = [m for m in configured if not live_ids or m in live_ids or m == 'openrouter/free']

        if live:
            ranked = list(live)
            if task_type == 'coding':
                ranked.sort(key=lambda x: (not self._looks_coding(x), not x.get('free', False), str(x.get('id'))))
            elif task_type == 'vision':
                ranked.sort(key=lambda x: (not self._looks_vision(x), not x.get('free', False), str(x.get('id'))))
            else:
                ranked.sort(key=lambda x: (not x.get('free', False), str(x.get('id'))))
            for item in ranked[:30]:
                mid = str(item.get('id') or '')
                if not mid:
                    continue
                if task_type == 'vision' and not self._looks_vision(item):
                    continue
                if mid not in candidates:
                    candidates.append(mid)
                if len(candidates) >= 8:
                    break

        return candidates

    def route_preview(self, prompt: str, task_type: str = 'auto', provider: str = 'auto') -> dict:
        classification = self.brain.classify(prompt, task_type)
        mode = self.mode()
        explicit = str(provider or 'auto').strip().lower()
        if explicit in ('ollama', 'local'):
            order = ['ollama']
        elif explicit not in ('', 'auto', 'smart'):
            if classification['privacy'] == 'high' and not self.cfg.get('allow_high_privacy_cloud', False):
                order = ['ollama']
            else:
                order = self._ordered_cloud_providers(classification['task_type'], explicit) + ['ollama']
        elif mode == 'local_only' or classification['privacy'] == 'high':
            order = ['ollama']
        elif mode == 'cloud_boost' or classification['complexity'] >= int(self.cfg.get('hybrid_cloud_complexity_threshold', 4)):
            order = self._ordered_cloud_providers(classification['task_type']) + ['ollama']
        else:
            order = ['ollama'] + self._ordered_cloud_providers(classification['task_type'])
        return {
            **classification,
            'mode': mode,
            'provider_order': order,
            'cloud_configured': [p for p in PROVIDER_NAMES if self.credentials.status(p)['configured']],
            'high_privacy_cloud_blocked': classification['privacy'] == 'high' and not self.cfg.get('allow_high_privacy_cloud', False),
        }

    def _cloud_attempts(self, prompt: str, classification: dict, mode: str, providers: list[str],
                        requested_model: str | None, system: str, timeout: int) -> tuple[dict | None, list[dict]]:
        errors = []
        max_providers = max(1, min(int(self.cfg.get('max_cloud_provider_attempts', 3)), len(PROVIDER_NAMES)))
        models_per_provider = max(1, min(int(self.cfg.get('max_models_per_provider_attempt', 2)), 4))
        for provider in providers[:max_providers]:
            models = self._model_candidates(provider, classification['task_type'], requested_model)
            if not models:
                errors.append({'provider': provider, 'model': '', 'error': 'no compatible provider model found'})
                continue
            for model in models[:models_per_provider]:
                request_id = str(uuid.uuid4())
                started = time.perf_counter()
                self._emit('PROVIDER_MODEL_ATTEMPT', {
                    'request_id': request_id, 'provider': provider, 'model': model,
                    'task_type': classification['task_type'], 'mode': mode,
                })
                try:
                    result = self.providers[provider].chat(prompt, model, system, timeout)
                    elapsed = (time.perf_counter() - started) * 1000.0
                    self._record_outcome(provider, model, classification['task_type'], True, elapsed)
                    self._record_request(request_id, prompt, classification, mode, provider, model, 'success', elapsed)
                    route = {
                        'request_id': request_id, 'provider': provider,
                        'selected_model': result.get('model') or model, 'mode': mode,
                        'task_type': classification['task_type'], 'intent': classification['intent'],
                        'complexity': classification['complexity'], 'privacy': classification['privacy'],
                        'latency_ms': round(elapsed, 2), 'cloud': True,
                    }
                    result['jubi_provider_route'] = route
                    self._emit('PROVIDER_ROUTE_SUCCEEDED', route)
                    return result, errors
                except Exception as exc:
                    elapsed = (time.perf_counter() - started) * 1000.0
                    self._record_outcome(provider, model, classification['task_type'], False, elapsed)
                    self._record_request(request_id, prompt, classification, mode, provider, model, 'failed', elapsed, str(exc))
                    item = {'provider': provider, 'model': model, 'error': str(exc), 'latency_ms': round(elapsed, 2)}
                    errors.append(item)
                    self._emit('PROVIDER_MODEL_FAILED', {**item, 'request_id': request_id})
        return None, errors

    def _local(self, prompt: str, task_type: str, model: str | None, system: str, timeout: int,
               mode: str, classification: dict, prior_errors: list[dict] | None = None) -> dict:
        result = self.brain.generate(prompt, task_type, model=model, system=system, timeout=timeout)
        local_route = result.get('jubi_route') or {}
        result['jubi_provider_route'] = {
            'provider': 'ollama',
            'selected_model': local_route.get('selected_model') or result.get('model'),
            'mode': mode,
            'task_type': classification['task_type'],
            'intent': classification['intent'],
            'complexity': classification['complexity'],
            'privacy': classification['privacy'],
            'cloud': False,
            'fallback_errors': prior_errors or [],
        }
        return result

    def generate(self, prompt: str, task_type: str = 'auto', model: str | None = None,
                 provider: str = 'auto', system: str = 'You are Jubi, a privacy-first AI orchestrator.',
                 timeout: int = 300) -> dict:
        prompt = str(prompt or '').strip()
        if not prompt:
            raise ValueError('chat prompt is required')
        classification = self.brain.classify(prompt, task_type)
        mode = self.mode()
        requested_provider = str(provider or 'auto').strip().lower()

        if requested_provider in ('ollama', 'local'):
            return self._local(prompt, task_type, model, system, timeout, mode, classification)

        explicit_cloud = requested_provider not in ('', 'auto', 'smart')
        if explicit_cloud:
            requested_provider = self._provider_id(requested_provider)
            if classification['privacy'] == 'high' and not bool(self.cfg.get('allow_high_privacy_cloud', False)):
                raise PermissionError(
                    'This request is classified as high privacy. Jubi blocks cloud transmission by default. '
                    'Use Local/Ollama or remove sensitive credential/private-data content.'
                )
            cloud, errors = self._cloud_attempts(
                prompt, classification, mode, self._ordered_cloud_providers(classification['task_type'], requested_provider),
                model, system, timeout,
            )
            if cloud:
                return cloud
            raise RuntimeError('The selected cloud provider failed. ' + '; '.join(x['error'] for x in errors[-3:]))

        if mode == 'local_only' or (
            classification['privacy'] == 'high' and not bool(self.cfg.get('allow_high_privacy_cloud', False))
        ):
            return self._local(prompt, task_type, model, system, timeout, mode, classification)

        cloud_providers = self._ordered_cloud_providers(classification['task_type'])
        threshold = int(self.cfg.get('hybrid_cloud_complexity_threshold', 4))
        cloud_first = mode == 'cloud_boost' or (mode == 'hybrid_auto' and classification['complexity'] >= threshold)

        if cloud_first and cloud_providers:
            cloud, errors = self._cloud_attempts(
                prompt, classification, mode, cloud_providers, model, system, timeout
            )
            if cloud:
                return cloud
            try:
                return self._local(prompt, task_type, None, system, timeout, mode, classification, errors)
            except Exception as local_exc:
                joined = '; '.join(x['error'] for x in errors[-3:])
                raise RuntimeError(f'Cloud providers failed ({joined}); local fallback failed: {local_exc}') from local_exc

        # Hybrid simple request: local first. If it fails, cloud may recover.
        try:
            return self._local(prompt, task_type, model, system, timeout, mode, classification)
        except Exception as local_exc:
            if not cloud_providers:
                raise
            cloud, errors = self._cloud_attempts(
                prompt, classification, mode, cloud_providers, None, system, timeout
            )
            if cloud:
                cloud['jubi_provider_route']['local_failure'] = str(local_exc)[:700]
                return cloud
            joined = '; '.join(x['error'] for x in errors[-3:])
            raise RuntimeError(f'Local route failed: {local_exc}; cloud fallback failed: {joined}') from local_exc

    def status(self, validate: bool = False) -> dict:
        ollama = self.brain.models.list_models()
        cloud = []
        for provider in PROVIDER_NAMES:
            cred = self.credentials.status(provider)
            item = {
                'provider': provider,
                'label': self.providers[provider].label,
                'configured': cred['configured'],
                'credential_source': cred['source'],
                'enabled': self._enabled(provider),
                'online': None,
                'status': 'configured' if cred['configured'] else 'unconfigured',
            }
            if validate and cred['configured']:
                item.update(self.providers[provider].health())
            cloud.append(item)
        return {
            'mode': self.mode(),
            'modes': list(PROVIDER_MODES),
            'local': {
                'provider': 'ollama', 'label': 'Ollama Local', 'configured': True,
                'online': bool(ollama.get('online')), 'models': len(ollama.get('items', [])),
                'status': 'connected' if ollama.get('online') else 'offline',
            },
            'cloud': cloud,
            'hybrid_cloud_complexity_threshold': int(self.cfg.get('hybrid_cloud_complexity_threshold', 4)),
            'high_privacy_cloud_allowed': bool(self.cfg.get('allow_high_privacy_cloud', False)),
            'credential_storage': {
                'windows_dpapi': self.credentials.status('openrouter')['persistent_dashboard_storage'],
                'path': str(self.credentials.path),
                'secrets_returned_by_api': False,
            },
            'performance': self.performance(),
            'recent_requests': self.recent_requests(30),
        }
