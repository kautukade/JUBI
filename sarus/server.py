from __future__ import annotations

from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from pathlib import Path
import json
import os
import sys
import traceback
import secrets

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sarus.core.app import Jubi

APP = Jubi(ROOT)
SESSION_TOKEN = secrets.token_urlsafe(32)
MAX_HTTP_BODY = 2 * 1024 * 1024


def _env(primary: str, legacy: str, default: str) -> str:
    value = os.environ.get(primary, '').strip()
    if value:
        return value
    legacy_value = os.environ.get(legacy, '').strip()
    if legacy_value:
        try:
            APP.bus.emit('LEGACY_ENV_USED', {'legacy': legacy, 'replacement': primary})
        except Exception:
            pass
        return legacy_value
    return default


DEBUG = _env('JUBI_DEBUG', 'SARUS_DEBUG', '0').lower() in {'1', 'true', 'yes', 'on'}


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT / 'sarus/web'), **kw)

    def log_message(self, fmt, *args):
        if _env('JUBI_HTTP_LOG', 'SARUS_HTTP_LOG', '0') == '1':
            super().log_message(fmt, *args)

    def _json(self, obj, code=200):
        b = json.dumps(obj, ensure_ascii=False, default=str).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header(
            'Content-Security-Policy',
            "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'",
        )
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _body(self):
        n = int(self.headers.get('Content-Length', '0'))
        if n < 0 or n > MAX_HTTP_BODY:
            raise ValueError('request body exceeds 2 MiB limit')
        try:
            return json.loads(self.rfile.read(n) or b'{}')
        except json.JSONDecodeError as exc:
            raise ValueError('invalid JSON body') from exc

    @staticmethod
    def _safe_error(exc: Exception, *, conflict=False):
        payload = {'error': str(exc)}
        if DEBUG:
            payload['trace'] = traceback.format_exc(limit=4)
        return payload

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        q = parse_qs(u.query, keep_blank_values=True)
        try:
            if p == '/api/session':
                return self._json({'token': SESSION_TOKEN, 'product': 'Jubi'})
            if p == '/api/status':
                return self._json(APP.status())
            if p == '/api/brain':
                return self._json(APP.brain.status())
            if p == '/api/brain/decisions':
                return self._json(APP.brain.recent_decisions(int(q.get('limit', ['50'])[0])))
            if p == '/api/brain/performance':
                return self._json(APP.brain.performance())
            if p == '/api/council':
                return self._json(APP.council.recent(int(q.get('limit', ['50'])[0])))
            if p == '/api/supervisor':
                return self._json(APP.supervisor.recent(int(q.get('limit', ['30'])[0])))
            if p == '/api/providers':
                validate = str(q.get('validate', ['0'])[0]).lower() in {'1', 'true', 'yes', 'on'}
                return self._json(APP.providers.status(validate=validate))
            if p == '/api/providers/models':
                provider = q.get('provider', [''])[0]
                force = str(q.get('force', ['0'])[0]).lower() in {'1', 'true', 'yes', 'on'}
                return self._json(APP.providers.models(provider, force=force))
            if p == '/api/providers/performance':
                return self._json(APP.providers.performance())
            if p == '/api/providers/requests':
                return self._json(APP.providers.recent_requests(int(q.get('limit', ['50'])[0])))
            if p == '/api/knowledge/status':
                return self._json(APP.knowledge.status())
            if p == '/api/knowledge/documents':
                return self._json(APP.knowledge.documents(q.get('namespace', [None])[0] or None, int(q.get('limit', ['100'])[0])))
            if p == '/api/knowledge/search':
                return self._json(APP.knowledge.search(q.get('q', [''])[0], q.get('namespace', [None])[0] or None, int(q.get('limit', ['8'])[0])))
            if p == '/api/experience':
                success_raw = str(q.get('success', [''])[0]).lower()
                success = True if success_raw in {'1', 'true', 'yes'} else (False if success_raw in {'0', 'false', 'no'} else None)
                return self._json(APP.experience.recent(int(q.get('limit', ['100'])[0]), success=success))
            if p == '/api/experience/stats':
                return self._json(APP.experience.stats())
            if p == '/api/experience/similar':
                return self._json(APP.experience.similar(q.get('q', [''])[0], q.get('task_type', [None])[0] or None, int(q.get('limit', ['6'])[0])))
            if p == '/api/broker':
                return self._json(APP.privileged.status())
            if p == '/api/doctor':
                return self._json(APP.doctor.run())
            if p == '/api/events':
                return self._json(APP.bus.recent(int(q.get('limit', ['100'])[0])))
            if p == '/api/models':
                return self._json(APP.models.list_models())
            if p == '/api/capabilities':
                if 'limit' in q or q.get('q', [''])[0] or q.get('source', [''])[0] or q.get('kind', [''])[0]:
                    kinds = [x for x in q.get('kind', []) if x] or None
                    return self._json(APP.registry.search(q.get('q', [''])[0], q.get('source', [None])[0], kinds, int(q.get('limit', ['50'])[0])))
                return self._json(APP.registry.summary())
            if p == '/api/capability':
                cid = q.get('id', [''])[0]
                return self._json(APP.registry.read(cid) or {'error': 'not found'}, 200 if APP.registry.get(cid) else 404)
            if p == '/api/tasks':
                return self._json(APP.execution.recent_tasks(int(q.get('limit', ['50'])[0])))
            if p == '/api/approvals':
                return self._json(APP.execution.approvals(q.get('status', ['pending'])[0]))
            if p == '/api/receipts':
                return self._json({'chain': APP.receipts.verify_chain(), 'items': APP.receipts.recent(int(q.get('limit', ['100'])[0]))})
            if p == '/api/memory':
                return self._json(APP.memory.search(q.get('q', [''])[0], q.get('namespace', [None])[0], int(q.get('limit', ['25'])[0])))
            if p == '/api/automations':
                return self._json(APP.scheduler.list())
            if p == '/api/fable':
                return self._json(APP.fable.status())
            if p == '/api/fable/traces':
                return self._json(APP.fable.traces.recent(int(q.get('limit', ['100'])[0]), q.get('kind', [None])[0] or None))
            if p == '/api/fable/capabilities':
                return self._json(APP.fable.capabilities.list(int(q.get('limit', ['100'])[0])))
            if p == '/api/fable/agenda':
                return self._json(APP.fable.agenda.list())
            if p == '/api/fable/lab/tail':
                return self._json(APP.fable.lab.tail(int(q.get('limit', ['200'])[0])))
            return super().do_GET()
        except ValueError as exc:
            return self._json(self._safe_error(exc), 400)
        except KeyError as exc:
            return self._json(self._safe_error(exc), 404)
        except Exception as exc:
            APP.bus.emit('HTTP_ERROR', {'method': 'GET', 'path': p, 'error': str(exc)[:1000]})
            return self._json(self._safe_error(exc), 500)

    def do_POST(self):
        p = urlparse(self.path).path
        token = self.headers.get('X-JUBI-Token', '') or self.headers.get('X-SARUS-Token', '')
        if token != SESSION_TOKEN:
            return self._json({'error': 'invalid Jubi session token'}, 403)
        origin = self.headers.get('Origin', '')
        host = self.headers.get('Host', '')
        if origin and origin not in {f'http://{host}', f'https://{host}'}:
            return self._json({'error': 'cross-origin request blocked'}, 403)
        try:
            data = self._body()
            if p == '/api/plan':
                return self._json({'steps': APP.orchestrator.execute_dry(str(data.get('text', '')))})
            if p == '/api/task':
                return self._json(APP.execution.run(str(data.get('text', '')), str(data.get('source', 'user')), data.get('capability_id')))
            if p == '/api/brain/route':
                return self._json(APP.brain.route(str(data.get('text', '')), str(data.get('task_type', 'auto')), data.get('model')))
            if p == '/api/council/run':
                return self._json(APP.council.run(str(data.get('text', '')), str(data.get('task_type', 'auto')), int(data.get('max_members', 4)), str(data.get('judge_provider', 'auto'))))
            if p == '/api/supervisor/plan':
                return self._json(APP.supervisor.plan(str(data.get('text', '')), str(data.get('task_type', 'auto')), str(data.get('provider', 'auto'))))
            if p == '/api/supervisor/run':
                return self._json(APP.supervisor.run(str(data.get('text', '')), str(data.get('task_type', 'auto')), str(data.get('provider', 'auto'))))
            if p == '/api/provider/route':
                return self._json(APP.providers.route_preview(str(data.get('text', '')), str(data.get('task_type', 'auto')), str(data.get('provider', 'auto'))))
            if p == '/api/providers/mode':
                return self._json(APP.providers.set_mode(str(data.get('mode', 'local_only'))))
            if p == '/api/provider/credential':
                return self._json(APP.providers.save_credential(str(data.get('provider', '')), str(data.get('api_key', ''))))
            if p == '/api/provider/credential/delete':
                return self._json(APP.providers.delete_credential(str(data.get('provider', ''))))
            if p == '/api/provider/validate':
                return self._json(APP.providers.validate(str(data.get('provider', ''))))
            if p == '/api/provider/default-model':
                return self._json(APP.providers.set_default_model(str(data.get('provider', '')), str(data.get('task_type', 'general')), str(data.get('model', ''))))
            if p == '/api/chat':
                text = str(data.get('text', ''))
                try:
                    result = APP.providers.generate(text, str(data.get('task_type', 'auto')), model=data.get('model'), provider=str(data.get('provider', 'auto')))
                    try:
                        APP.experience.record_chat(text, result, True)
                    except Exception as learn_exc:
                        APP.bus.emit('EXPERIENCE_RECORD_WARNING', {'error': str(learn_exc)[:1000]})
                    return self._json(result)
                except Exception as exc:
                    try:
                        classification = APP.brain.classify(text, str(data.get('task_type', 'auto')))
                        APP.experience.record(text, str(exc), False, task_type=classification['task_type'], kind='chat', lesson='This route failed; use successful alternatives for similar work.')
                    except Exception:
                        pass
                    raise
            if p == '/api/knowledge/ingest':
                return self._json(APP.knowledge.ingest(str(data.get('content', '')), str(data.get('title', '')), str(data.get('namespace', 'general')), str(data.get('source', 'manual')), data.get('metadata') or {}))
            if p == '/api/knowledge/search':
                return self._json(APP.knowledge.search(str(data.get('query', data.get('q', ''))), str(data.get('namespace', '')).strip() or None, int(data.get('limit', 8)), float(data.get('min_score', 0.0))))
            if p == '/api/knowledge/ask':
                return self._json(APP.knowledge.answer(str(data.get('question', '')), str(data.get('namespace', '')).strip() or None, int(data.get('limit', 6)), str(data.get('provider', 'auto')), data.get('model')))
            if p == '/api/knowledge/delete':
                return self._json(APP.knowledge.delete_document(str(data.get('id', ''))))
            if p == '/api/experience':
                return self._json(APP.experience.record(str(data.get('request', '')), str(data.get('outcome', '')), bool(data.get('success', True)), task_type=str(data.get('task_type', 'general')), kind=str(data.get('kind', 'manual')), provider=str(data.get('provider', '')), model=str(data.get('model', '')), tool=str(data.get('tool', '')), latency_ms=float(data.get('latency_ms', 0)), lesson=str(data.get('lesson', '')), metadata=data.get('metadata') or {}))
            if p == '/api/experience/delete':
                return self._json(APP.experience.delete(str(data.get('id', ''))))
            if p == '/api/capability/run':
                cid = str(data.get('id', ''))
                cap = APP.registry.get(cid)
                if not cap:
                    return self._json({'error': 'capability not found'}, 404)
                adapter = APP.adapters.get(cap['source'])
                out = adapter.execute(str(data.get('text', 'Use this capability for its intended purpose.')), APP, capability_id=cid)
                receipt = APP.receipts.create('direct-capability', cid, cap['source'], 'completed' if out.get('ok') else 'failed', out)
                return self._json({'capability': cap, 'result': out, 'receipt': receipt})
            if p == '/api/memory':
                return self._json(APP.memory.add(str(data.get('content', '')), str(data.get('title', '')), str(data.get('namespace', 'general')), data.get('metadata') or {}))
            if p == '/api/approval':
                return self._json(APP.execution.set_approval(str(data.get('id', '')), str(data.get('status', 'rejected'))))
            if p == '/api/system/action':
                if 'action_id' not in data:
                    safe_legacy = {'list_processes': 'system.processes.list', 'list_services': 'system.services.list', 'read_file': 'workspace.file.read', 'write_file': 'workspace.file.write', 'open_url': 'url.open'}
                    old_name = str(data.get('name', ''))
                    if old_name in safe_legacy:
                        data = {'action_id': safe_legacy[old_name], 'parameters': data.get('args') or {}}
                proof = self.headers.get('X-JUBI-Approval') or self.headers.get('X-SARUS-Approval')
                out = APP.privileged.handle(data, source='local-api', approval_proof=proof)
                code = 423 if out.get('status') == 'approval_required' else (403 if out.get('status') == 'denied' else (400 if out.get('status') == 'invalid' else 200))
                return self._json(out, code)
            if p == '/api/automation':
                return self._json(APP.scheduler.add(str(data.get('name', 'Automation')), str(data.get('prompt', '')), int(data.get('interval_seconds', 3600)), bool(data.get('enabled', True))))
            if p == '/api/automation/toggle':
                APP.scheduler.set_enabled(str(data.get('id', '')), bool(data.get('enabled')))
                return self._json({'ok': True})
            if p == '/api/fable/lab':
                action = str(data.get('action', 'status'))
                if action == 'status':
                    return self._json(APP.fable.lab.status())
                if action == 'start':
                    return self._json(APP.fable.lab.start())
                if action == 'stop':
                    return self._json(APP.fable.lab.stop())
                if action in APP.fable.lab.ACTION_TARGETS:
                    return self._json(APP.fable.lab.run_action(action, int(data.get('timeout', 1800))))
                return self._json({'error': 'unsupported Fable lab action'}, 400)
            if p == '/api/fable/capability/save':
                cap = APP.fable.capabilities.save(str(data.get('name', '')), str(data.get('description', '')), str(data.get('prompt', '')), data.get('permissions') or [])
                trace = APP.fable.traces.verified('capability.save', {'capability_id': cap['id'], 'definition_hash': cap['definition_hash']})
                return self._json({'ok': True, 'capability': cap, 'trace': trace})
            if p == '/api/fable/capability/run':
                return self._json(APP.fable.run_capability(str(data.get('id', ''))))
            if p == '/api/fable/capability/toggle':
                cap = APP.fable.capabilities.set_enabled(str(data.get('id', '')), bool(data.get('enabled')))
                return self._json({'ok': True, 'capability': cap})
            if p == '/api/fable/agenda/add':
                cid = str(data.get('capability_id', ''))
                cap = APP.fable.capabilities.get(cid)
                if not cap:
                    return self._json({'error': 'Fable capability not found'}, 404)
                if not cap['enabled']:
                    return self._json({'error': 'Fable capability is disabled'}, 403)
                item = APP.fable.agenda.add(str(data.get('name', cap['name'])), str(data.get('when', 'once')), cid, int(data.get('period_seconds', 3600)), int(data.get('max_runs', 1)))
                return self._json({'ok': True, 'agenda': item})
            if p == '/api/fable/agenda/toggle':
                item = APP.fable.agenda.set_enabled(str(data.get('id', '')), bool(data.get('enabled')))
                return self._json({'ok': True, 'agenda': item})
            return self._json({'error': 'not found'}, 404)
        except ValueError as exc:
            return self._json(self._safe_error(exc), 400)
        except KeyError as exc:
            return self._json(self._safe_error(exc), 404)
        except PermissionError as exc:
            return self._json(self._safe_error(exc), 403)
        except RuntimeError as exc:
            return self._json(self._safe_error(exc), 409)
        except Exception as exc:
            APP.bus.emit('HTTP_ERROR', {'method': 'POST', 'path': p, 'error': str(exc)[:1000]})
            return self._json(self._safe_error(exc), 500)


def run(port=None):
    port = int(port or _env('JUBI_PORT', 'SARUS_PORT', '8877'))
    host = _env('JUBI_HOST', 'SARUS_HOST', '127.0.0.1')
    if host == '0.0.0.0':
        raise RuntimeError('Jubi is localhost-only; JUBI_HOST=0.0.0.0 is not permitted')
    print(f'Jubi v{APP.VERSION} dashboard: http://{host}:{port}')
    httpd = ThreadingHTTPServer((host, port), H)
    try:
        httpd.serve_forever()
    finally:
        httpd.server_close()
        APP.shutdown()


if __name__ == '__main__':
    run()
