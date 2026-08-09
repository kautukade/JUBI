from __future__ import annotations
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from pathlib import Path
import json, os, sys, traceback, secrets

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sarus.core.app import Sarus

APP = Sarus(ROOT)
SESSION_TOKEN = secrets.token_urlsafe(32)
MAX_HTTP_BODY = 65536


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT / 'sarus/web'), **kw)

    def log_message(self, fmt, *args):
        if os.environ.get('SARUS_HTTP_LOG', '0') == '1':
            super().log_message(fmt, *args)

    def _json(self, obj, code=200):
        b = json.dumps(obj, ensure_ascii=False, default=str).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('X-Frame-Options', 'DENY')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'")
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _body(self):
        n = int(self.headers.get('Content-Length', '0'))
        if n < 0 or n > MAX_HTTP_BODY:
            raise ValueError('request body exceeds 64 KiB limit')
        try:
            return json.loads(self.rfile.read(n) or b'{}')
        except json.JSONDecodeError as exc:
            raise ValueError('invalid JSON body') from exc

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        q = parse_qs(u.query, keep_blank_values=True)
        try:
            if p == '/api/session':
                return self._json({'token': SESSION_TOKEN})
            if p == '/api/status':
                return self._json(APP.status())
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

            # Fable Intelligence / Research Lab API.
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
        except Exception as e:
            return self._json({'error': str(e), 'trace': traceback.format_exc(limit=2)}, 500)

    def do_POST(self):
        p = urlparse(self.path).path
        if self.headers.get('X-SARUS-Token', '') != SESSION_TOKEN:
            return self._json({'error': 'invalid SARUS session token'}, 403)
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
            if p == '/api/chat':
                return self._json(APP.models.generate(str(data.get('text', '')), str(data.get('task_type', 'general')), model=data.get('model')))
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
                # Keep only old read-only dashboard aliases for compatibility.
                # Privileged legacy names (powershell/service_control/etc.) are
                # intentionally not translated and will fail schema validation.
                if 'action_id' not in data:
                    safe_legacy = {
                        'list_processes': 'system.processes.list',
                        'list_services': 'system.services.list',
                        'read_file': 'workspace.file.read',
                        'write_file': 'workspace.file.write',
                        'open_url': 'url.open',
                    }
                    old_name = str(data.get('name', ''))
                    if old_name in safe_legacy:
                        data = {'action_id': safe_legacy[old_name], 'parameters': data.get('args') or {}}
                # Privileged approval is deliberately out-of-band. A JSON
                # "approved": true flag is not accepted as authorization.
                proof = self.headers.get('X-SARUS-Approval')
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
                cap = APP.fable.capabilities.save(
                    str(data.get('name', '')),
                    str(data.get('description', '')),
                    str(data.get('prompt', '')),
                    data.get('permissions') or [],
                )
                trace = APP.fable.traces.verified(
                    'capability.save',
                    {'capability_id': cap['id'], 'definition_hash': cap['definition_hash']},
                )
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
                item = APP.fable.agenda.add(
                    str(data.get('name', cap['name'])),
                    str(data.get('when', 'once')),
                    cid,
                    int(data.get('period_seconds', 3600)),
                    int(data.get('max_runs', 1)),
                )
                return self._json({'ok': True, 'agenda': item})
            if p == '/api/fable/agenda/toggle':
                item = APP.fable.agenda.set_enabled(str(data.get('id', '')), bool(data.get('enabled')))
                return self._json({'ok': True, 'agenda': item})
            return self._json({'error': 'not found'}, 404)
        except ValueError as e:
            return self._json({'error': str(e)}, 400)
        except KeyError as e:
            return self._json({'error': str(e)}, 404)
        except PermissionError as e:
            return self._json({'error': str(e)}, 403)
        except RuntimeError as e:
            return self._json({'error': str(e)}, 409)
        except Exception as e:
            return self._json({'error': str(e), 'trace': traceback.format_exc(limit=3)}, 500)


def run(port=None):
    port = int(port or os.environ.get('SARUS_PORT', '8877'))
    host = os.environ.get('SARUS_HOST', '127.0.0.1')
    print(f'SARUS v1.3 dashboard: http://{host}:{port}')
    ThreadingHTTPServer((host, port), H).serve_forever()


if __name__ == '__main__':
    run()
