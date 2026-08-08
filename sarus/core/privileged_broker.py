from __future__ import annotations

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
            'receipt_signing': self.receipts.SIGNATURE_ALGORITHM,
            'kernel_direct_access': False,
            'arbitrary_shell': False,
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
        return datetime.fromisoformat(text).astimezone(timezone.utc).timestamp()

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

    def _approval_ok(self, proof: str | None) -> bool:
        if len(self._approval_secret) < 24 or not proof:
            return False
        return secrets.compare_digest(self._approval_secret, str(proof))

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
            resolved = self._resolve_resource(spec, parameters)
            risk = int(spec.get('risk', 0))
            decision = self.policy.evaluate('privileged_system_action' if risk >= 4 else action_id, risk, 'core')
            if decision.get('decision') in {'deny', 'isolated'}:
                receipt = self._receipt(request_id, action_id, 'denied', {
                    'status': 'denied', 'reason': decision.get('reason'), 'risk': risk,
                    'parameters': parameters, 'resource_id': resolved.get('resource_id'),
                })
                return {'ok': False, 'status': 'denied', 'policy': decision, 'receipt': receipt}

            requires_approval = bool(spec.get('requires_approval')) or decision.get('decision') == 'approval'
            if requires_approval and not self._approval_ok(approval_proof):
                receipt = self._receipt(request_id, action_id, 'approval_required', {
                    'status': 'approval_required', 'risk': risk, 'parameters': parameters,
                    'resource_id': resolved.get('resource_id'),
                    'approval_secret_configured': len(self._approval_secret) >= 24,
                })
                return {
                    'ok': False,
                    'status': 'approval_required',
                    'request_id': request_id,
                    'action_id': action_id,
                    'receipt': receipt,
                }

            # Mark only immediately before execution. Approval-required requests
            # can therefore be resubmitted once with an out-of-band proof.
            self._mark_once(request_id, nonce)
            result = self.windows.execute_typed(action_id, parameters, resolved)
            status = 'completed' if result.get('ok') else 'failed'
            receipt = self._receipt(request_id, action_id, status, {
                'status': status,
                'risk': risk,
                'parameters': parameters,
                'resource_id': resolved.get('resource_id'),
                'result': result,
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
