from __future__ import annotations

import hashlib
import hmac
import json
import os
import secrets
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse


class PrivilegedBroker:
    """Zero-trust gateway for typed Windows actions.

    The broker accepts only action IDs present in config/broker_allowlist.json.
    It never accepts shell text, executable paths, raw driver handles, IOCTLs,
    or kernel-memory parameters from the caller.
    """

    APPROVAL_VERSION = 'v1'
    MAX_APPROVAL_TTL = 300

    def __init__(self, root: Path, config_path: Path, policy, windows, receipts):
        self.root = root.resolve()
        self.cfg = json.loads(config_path.read_text(encoding='utf-8'))
        self.policy = policy
        self.windows = windows
        self.receipts = receipts
        self.max_request_bytes = int(self.cfg.get('max_request_bytes', 65536))
        self.replay_window = int(self.cfg.get('replay_window_seconds', 300))
        self._seen: dict[str, float] = {}
        self._lock = threading.Lock()
        self._approval_secret = os.environ.get('SARUS_BROKER_APPROVAL_SECRET', '')

    def status(self):
        actions = self.cfg.get('actions', {})
        return {
            'schema': self.cfg.get('schema'),
            'default': 'deny',
            'configured_actions': sorted(k for k, v in actions.items() if v.get('enabled', False)),
            'forbidden_actions': sorted(self.cfg.get('forbidden_actions', [])),
            'approval_secret_configured': len(self._approval_secret) >= 24,
            'approval_proof': 'request-bound-hmac-sha256',
            'approval_max_ttl_seconds': self.MAX_APPROVAL_TTL,
            'receipt_signing': self.receipts.SIGNATURE_ALGORITHM,
            'kernel_direct_access': False,
            'arbitrary_shell': False,
            'audit_raw_content': False,
        }

    @staticmethod
    def _timestamp(value) -> float:
        if value is None or value == '':
            return time.time()
        if isinstance(value, (int, float)):
            return float(value)
        text = str(value).strip()
        if text.endswith('Z'):
            text = text[:-1] + '+00:00'
        parsed = datetime.fromisoformat(text)
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=timezone.utc)
        return parsed.astimezone(timezone.utc).timestamp()

    def _check_freshness(self, ts: float):
        if abs(time.time() - ts) > self.replay_window:
            raise PermissionError('request timestamp is outside the replay window')

    def _mark_once(self, request_id: str, nonce: str):
        now = time.time()
        with self._lock:
            cutoff = now - self.replay_window
            self._seen = {k: v for k, v in self._seen.items() if v >= cutoff}
            keys = (f'id:{request_id}', f'nonce:{nonce}')
            if any(k in self._seen for k in keys):
                raise PermissionError('replayed broker request')
            for k in keys:
                self._seen[k] = now

    def _validate_parameters(self, spec: dict, parameters: dict) -> dict:
        if not isinstance(parameters, dict):
            raise ValueError('parameters must be an object')
        schema = spec.get('parameters', {})
        unknown = sorted(set(parameters) - set(schema))
        if unknown:
            raise ValueError('unknown parameters: ' + ', '.join(unknown))
        out = {}
        for name, rule in schema.items():
            required = bool(rule.get('required'))
            if name not in parameters:
                if required:
                    raise ValueError(f'missing required parameter: {name}')
                continue
            value = parameters[name]
            typ = rule.get('type')
            if typ in {'string', 'resource_id', 'url'}:
                if not isinstance(value, str):
                    raise ValueError(f'{name} must be a string')
                value = value.strip() if typ != 'string' else value
                if len(value) > int(rule.get('max_length', 4096)):
                    raise ValueError(f'{name} exceeds maximum length')
                if typ == 'resource_id' and (not value or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in value)):
                    raise ValueError(f'invalid resource id: {name}')
                if typ == 'url':
                    parsed = urlparse(value)
                    if parsed.scheme not in {'http', 'https'} or not parsed.netloc:
                        raise ValueError('only http/https URLs are allowed')
            elif typ == 'boolean':
                if type(value) is not bool:
                    raise ValueError(f'{name} must be a boolean')
            elif typ == 'integer':
                if type(value) is not int:
                    raise ValueError(f'{name} must be an integer')
            else:
                raise ValueError(f'unsupported parameter schema type: {typ}')
            out[name] = value
        return out

    def _resolve_resource(self, spec: dict, parameters: dict) -> dict:
        group = spec.get('resource_group')
        if not group:
            return {}
        resource_id = parameters.get('resource_id')
        resources = self.cfg.get('resources', {}).get(group, {})
        if resource_id not in resources:
            raise PermissionError(f'resource is not allowlisted in {group}')
        return {'resource_id': resource_id, **resources[resource_id]}

    @staticmethod
    def _parameters_hash(parameters: dict) -> str:
        raw = json.dumps(parameters, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
        return hashlib.sha256(raw).hexdigest()

    def _approval_message(self, request_id: str, action_id: str, parameters: dict, expires: int) -> bytes:
        phash = self._parameters_hash(parameters)
        return f'{self.APPROVAL_VERSION}|{request_id}|{action_id}|{phash}|{expires}'.encode('utf-8')

    def create_approval_proof(self, request_id: str, action_id: str, parameters: dict, ttl_seconds: int = 120) -> str:
        """Create a short-lived proof for an external trusted approval tool.

        This method is intentionally not exposed by the HTTP API. In the native
        service phase the equivalent operation belongs in a separate elevated
        approval UI/service identity.
        """
        if len(self._approval_secret) < 24:
            raise RuntimeError('SARUS_BROKER_APPROVAL_SECRET is not configured securely')
        ttl = max(1, min(int(ttl_seconds), self.MAX_APPROVAL_TTL))
        expires = int(time.time()) + ttl
        mac = hmac.new(
            self._approval_secret.encode('utf-8'),
            self._approval_message(request_id, action_id, parameters, expires),
            hashlib.sha256,
        ).hexdigest()
        return f'{self.APPROVAL_VERSION}:{expires}:{mac}'

    def _approval_ok(self, proof: str | None, request_id: str, action_id: str, parameters: dict) -> bool:
        if len(self._approval_secret) < 24 or not proof:
            return False
        try:
            version, expires_text, supplied = str(proof).split(':', 2)
            expires = int(expires_text)
        except (ValueError, TypeError):
            return False
        now = int(time.time())
        if version != self.APPROVAL_VERSION or expires < now or expires > now + self.MAX_APPROVAL_TTL:
            return False
        expected = hmac.new(
            self._approval_secret.encode('utf-8'),
            self._approval_message(request_id, action_id, parameters, expires),
            hashlib.sha256,
        ).hexdigest()
        return secrets.compare_digest(expected, supplied)

    @staticmethod
    def _redacted_value(value):
        if isinstance(value, str):
            data = value.encode('utf-8', errors='replace')
            return {
                'redacted': True,
                'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
            }
        raw = json.dumps(value, ensure_ascii=False, sort_keys=True, default=str).encode('utf-8')
        return {
            'redacted': True,
            'bytes': len(raw),
            'sha256': hashlib.sha256(raw).hexdigest(),
        }

    def _audit_parameters(self, parameters: dict) -> dict:
        safe = {}
        sensitive_names = {'content', 'password', 'secret', 'token', 'api_key', 'authorization'}
        for key, value in parameters.items():
            if key.lower() in sensitive_names:
                safe[key] = self._redacted_value(value)
            else:
                safe[key] = value
        return safe

    def _audit_result(self, result: dict) -> dict:
        safe = {}
        content_names = {'content', 'stdout', 'stderr', 'password', 'secret', 'token', 'api_key', 'authorization'}
        for key, value in result.items():
            if key.lower() in content_names:
                safe[key] = self._redacted_value(value)
            else:
                safe[key] = value
        return safe

    def _receipt(self, request_id: str, action_id: str, status: str, payload: dict):
        safe_payload = {
            'schema': 'sarus.controlbridge.receipt.v1',
            'request_id': request_id,
            'action_id': action_id,
            **payload,
        }
        return self.receipts.create('privileged-broker', request_id, 'windows-broker', status, safe_payload)

    def handle(self, request: dict, source: str = 'user', approval_proof: str | None = None):
        request_id = str(uuid.uuid4())
        action_id = ''
        try:
            if not isinstance(request, dict):
                raise ValueError('request must be a JSON object')
            raw = json.dumps(request, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
            if len(raw) > self.max_request_bytes:
                raise ValueError('broker request is too large')

            allowed_top = {'schema', 'request_id', 'timestamp', 'nonce', 'action_id', 'parameters', 'reason', 'source'}
            unknown_top = sorted(set(request) - allowed_top)
            if unknown_top:
                raise ValueError('unknown request fields: ' + ', '.join(unknown_top))

            request_id = str(request.get('request_id') or uuid.uuid4())
            nonce = str(request.get('nonce') or secrets.token_urlsafe(16))
            action_id = str(request.get('action_id') or '').strip()
            if not action_id:
                raise ValueError('action_id is required')
            if len(request_id) > 128 or len(nonce) > 256:
                raise ValueError('request_id or nonce is too long')

            ts = self._timestamp(request.get('timestamp'))
            self._check_freshness(ts)

            if action_id in set(self.cfg.get('forbidden_actions', [])):
                raise PermissionError('action is permanently forbidden by broker policy')
            if action_id.startswith(('kernel.', 'driver.raw_', 'shell.', 'powershell.')):
                raise PermissionError('direct kernel/driver/shell actions are forbidden')

            spec = self.cfg.get('actions', {}).get(action_id)
            if not spec or not spec.get('enabled', False):
                raise PermissionError('action is not allowlisted')

            parameters = self._validate_parameters(spec, request.get('parameters') or {})
            audit_parameters = self._audit_parameters(parameters)
            resolved = self._resolve_resource(spec, parameters)
            risk = int(spec.get('risk', 0))
            decision = self.policy.evaluate('privileged_system_action' if risk >= 4 else action_id, risk, 'core')
            if decision.get('decision') in {'deny', 'isolated'}:
                receipt = self._receipt(request_id, action_id, 'denied', {
                    'status': 'denied', 'reason': decision.get('reason'), 'risk': risk,
                    'parameters': audit_parameters, 'resource_id': resolved.get('resource_id'),
                })
                return {'ok': False, 'status': 'denied', 'policy': decision, 'receipt': receipt}

            requires_approval = bool(spec.get('requires_approval')) or decision.get('decision') == 'approval'
            if requires_approval and not self._approval_ok(approval_proof, request_id, action_id, parameters):
                receipt = self._receipt(request_id, action_id, 'approval_required', {
                    'status': 'approval_required', 'risk': risk, 'parameters': audit_parameters,
                    'resource_id': resolved.get('resource_id'),
                    'approval_secret_configured': len(self._approval_secret) >= 24,
                    'approval_binding': 'request_id+action_id+parameters_hash+expiry',
                })
                return {
                    'ok': False,
                    'status': 'approval_required',
                    'request_id': request_id,
                    'action_id': action_id,
                    'receipt': receipt,
                }

            # Mark only immediately before execution. Approval-required requests
            # can therefore be resubmitted once with a valid request-bound proof.
            self._mark_once(request_id, nonce)
            result = self.windows.execute_typed(action_id, parameters, resolved)
            status = 'completed' if result.get('ok') else 'failed'
            receipt = self._receipt(request_id, action_id, status, {
                'status': status,
                'risk': risk,
                'parameters': audit_parameters,
                'resource_id': resolved.get('resource_id'),
                'result': self._audit_result(result),
            })
            return {
                'ok': bool(result.get('ok')),
                'status': status,
                'request_id': request_id,
                'action_id': action_id,
                'result': result,
                'receipt': receipt,
            }
        except PermissionError as exc:
            receipt = self._receipt(request_id, action_id or 'unknown', 'denied', {'status': 'denied', 'reason': str(exc)})
            return {'ok': False, 'status': 'denied', 'request_id': request_id, 'action_id': action_id, 'error': str(exc), 'receipt': receipt}
        except (ValueError, KeyError, TypeError) as exc:
            receipt = self._receipt(request_id, action_id or 'unknown', 'invalid', {'status': 'invalid', 'reason': str(exc)})
            return {'ok': False, 'status': 'invalid', 'request_id': request_id, 'action_id': action_id, 'error': str(exc), 'receipt': receipt}
