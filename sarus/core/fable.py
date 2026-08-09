from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import sqlite3
import subprocess
import threading
import time
import uuid


@dataclass(frozen=True)
class FableCommandResult:
    ok: bool
    action: str
    returncode: int
    duration_ms: int
    output: str

    def as_dict(self):
        return {
            'ok': self.ok,
            'action': self.action,
            'returncode': self.returncode,
            'duration_ms': self.duration_ms,
            'output': self.output,
        }


class FableTraceStore:
    """Stores Fable-inspired ground-truth evidence separately from model prose.

    A SARUS verified event is created by trusted Python execution and is also
    committed to the existing signed/hash-chained ReceiptStore. Imported Fable
    serial text is classified, never promoted to a SARUS verified event merely
    because it contains brackets.
    """

    def __init__(self, db: Path, receipts):
        self.db = db
        self.receipts = receipts
        self.lock = threading.Lock()
        db.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(db)) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS fable_traces("
                "id TEXT PRIMARY KEY,ts REAL,channel TEXT,kind TEXT,event TEXT,"
                "payload TEXT,receipt_id TEXT)"
            )
            c.commit()

    def _insert(self, channel: str, kind: str, event: str, payload: dict, receipt_id: str = ''):
        row = {
            'id': str(uuid.uuid4()),
            'ts': time.time(),
            'channel': channel,
            'kind': kind,
            'event': event,
            'payload': payload,
            'receipt_id': receipt_id,
        }
        with self.lock, closing(sqlite3.connect(self.db)) as c:
            c.execute(
                "INSERT INTO fable_traces(id,ts,channel,kind,event,payload,receipt_id) VALUES(?,?,?,?,?,?,?)",
                (row['id'], row['ts'], channel, kind, event,
                 json.dumps(payload, ensure_ascii=False, sort_keys=True), receipt_id),
            )
            c.commit()
        return row

    def verified(self, event: str, payload: dict, status: str = 'completed', task_id: str = 'fable'):
        receipt = self.receipts.create(task_id, event, 'fable_integration', status, payload)
        return self._insert('sarus', 'verified', event, payload, receipt['id']) | {'receipt': receipt}

    def import_serial(self, text: str, source: str = 'fable-serial'):
        """Classify serial text without trusting text-controlled formatting.

        `kernel_candidate` means the line has Fable's column-zero bracket shape.
        It is still imported evidence, not a SARUS signed verified event. The
        distinction prevents pasted/model-generated text from becoming proof.
        """
        rows = []
        for raw in str(text).splitlines():
            if not raw:
                continue
            kind = 'kernel_candidate' if raw.startswith('[') else 'prose'
            rows.append(self._insert(source, kind, 'serial.line', {'line': raw[:4096]}))
        return rows

    def recent(self, limit: int = 100, kind: str | None = None):
        limit = max(1, min(int(limit), 1000))
        sql = "SELECT id,ts,channel,kind,event,payload,receipt_id FROM fable_traces"
        args: list[object] = []
        if kind:
            sql += " WHERE kind=?"
            args.append(kind)
        sql += " ORDER BY ts DESC,id DESC LIMIT ?"
        args.append(limit)
        with closing(sqlite3.connect(self.db)) as c:
            rows = c.execute(sql, tuple(args)).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'channel': r[2], 'kind': r[3],
                'event': r[4], 'payload': json.loads(r[5] or '{}'), 'receipt_id': r[6],
            }
            for r in rows
        ]


class LearnedCapabilityStore:
    """Versioned, testable SARUS-native form of Fable's persistent capabilities.

    Capabilities are declarative prompts routed back through SARUS execution.
    This store never persists raw executable kernel code.
    """

    MAX_CAPABILITIES = 256

    def __init__(self, db: Path):
        self.db = db
        self.lock = threading.Lock()
        with closing(sqlite3.connect(db)) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS fable_capabilities("
                "id TEXT PRIMARY KEY,name TEXT,version INTEGER,description TEXT,prompt TEXT,"
                "permissions TEXT,enabled INTEGER,created REAL,updated REAL,definition_hash TEXT,"
                "success_count INTEGER,failure_count INTEGER,last_status TEXT,UNIQUE(name,version))"
            )
            c.commit()

    @staticmethod
    def _norm_name(name: str) -> str:
        clean = ''.join(ch.lower() if ch.isalnum() else '_' for ch in str(name).strip())
        clean = '_'.join(x for x in clean.split('_') if x)
        if not clean or len(clean) > 64:
            raise ValueError('capability name must contain 1-64 alphanumeric characters')
        return clean

    @staticmethod
    def _digest(name: str, version: int, description: str, prompt: str, permissions: list[str]):
        blob = json.dumps(
            {'name': name, 'version': version, 'description': description,
             'prompt': prompt, 'permissions': sorted(permissions)},
            sort_keys=True, separators=(',', ':'), ensure_ascii=False,
        )
        return hashlib.sha256(blob.encode('utf-8')).hexdigest()

    def save(self, name: str, description: str, prompt: str, permissions=None):
        name = self._norm_name(name)
        description = str(description).strip()[:1000]
        prompt = str(prompt).strip()
        if not prompt or len(prompt) > 12000:
            raise ValueError('capability prompt must contain 1-12000 characters')
        permissions = sorted({str(x).strip() for x in (permissions or []) if str(x).strip()})[:32]
        with self.lock, closing(sqlite3.connect(self.db)) as c:
            total = c.execute("SELECT COUNT(*) FROM fable_capabilities").fetchone()[0]
            latest = c.execute("SELECT MAX(version) FROM fable_capabilities WHERE name=?", (name,)).fetchone()[0]
            if total >= self.MAX_CAPABILITIES and latest is None:
                raise ValueError('Fable capability limit reached')
            version = int(latest or 0) + 1
            now = time.time()
            cid = f'{name}:v{version}'
            digest = self._digest(name, version, description, prompt, permissions)
            c.execute(
                "INSERT INTO fable_capabilities VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (cid, name, version, description, prompt, json.dumps(permissions), 1,
                 now, now, digest, 0, 0, 'untested'),
            )
            c.commit()
        return self.get(cid)

    def get(self, cid: str):
        with closing(sqlite3.connect(self.db)) as c:
            r = c.execute(
                "SELECT id,name,version,description,prompt,permissions,enabled,created,updated,"
                "definition_hash,success_count,failure_count,last_status FROM fable_capabilities WHERE id=?",
                (cid,),
            ).fetchone()
        if not r:
            return None
        return {
            'id': r[0], 'name': r[1], 'version': r[2], 'description': r[3], 'prompt': r[4],
            'permissions': json.loads(r[5] or '[]'), 'enabled': bool(r[6]), 'created': r[7],
            'updated': r[8], 'definition_hash': r[9], 'success_count': r[10],
            'failure_count': r[11], 'last_status': r[12],
        }

    def list(self, limit: int = 100):
        limit = max(1, min(int(limit), self.MAX_CAPABILITIES))
        with closing(sqlite3.connect(self.db)) as c:
            ids = [r[0] for r in c.execute(
                "SELECT id FROM fable_capabilities ORDER BY updated DESC LIMIT ?", (limit,)
            ).fetchall()]
        return [self.get(x) for x in ids]

    def set_enabled(self, cid: str, enabled: bool):
        with closing(sqlite3.connect(self.db)) as c:
            cur = c.execute(
                "UPDATE fable_capabilities SET enabled=?,updated=? WHERE id=?",
                (1 if enabled else 0, time.time(), cid),
            )
            c.commit()
        if not cur.rowcount:
            raise KeyError('capability not found')
        return self.get(cid)

    def record_result(self, cid: str, ok: bool):
        status = 'success' if ok else 'failed'
        col = 'success_count' if ok else 'failure_count'
        with closing(sqlite3.connect(self.db)) as c:
            c.execute(
                f"UPDATE fable_capabilities SET {col}={col}+1,last_status=?,updated=? WHERE id=?",
                (status, time.time(), cid),
            )
            c.commit()
        return self.get(cid)


class FableLabManager:
    """Runs the pinned Fable source as a separate QEMU research lab.

    Only fixed make targets are reachable. There is intentionally no caller-
    supplied shell, command, target, QEMU argument, device address or API key.
    """

    ACTION_TARGETS = {
        'build': None,
        'test_host': 'test-host',
        'test_all': 'test',
        'test_qemu': 'test-qemu',
        'iso': 'iso',
        'clean': 'clean',
    }

    def __init__(self, root: Path, source: Path, traces: FableTraceStore):
        self.root = root
        self.source = source
        self.traces = traces
        self.state_dir = root / 'data/fable'
        self.log_path = root / 'logs/fable-lab.log'
        self.state_path = self.state_dir / 'lab-state.json'
        self.lock = threading.Lock()
        self.process: subprocess.Popen | None = None
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)

    def _pin(self):
        cfg = self.root / 'config/online_sources.json'
        try:
            rows = json.loads(cfg.read_text(encoding='utf-8'))
            return next((x.get('sha', '') for x in rows if x.get('key') == 'fable_os'), '')
        except Exception:
            return ''

    def _required(self):
        names = ['README.md', 'AGENTS.md', 'README.os.md', 'Makefile', 'core', 'tools', 'vm', 'compiler']
        return {name: (self.source / name).exists() for name in names}

    def _tool_count(self):
        p = self.source / 'tools'
        return len(list(p.glob('*.c'))) if p.is_dir() else 0

    def _pid_alive(self, pid: int):
        if pid <= 0:
            return False
        if os.name == 'nt':
            try:
                out = subprocess.run(
                    ['tasklist', '/FI', f'PID eq {pid}', '/NH'], capture_output=True,
                    text=True, timeout=5, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)
                )
                return str(pid) in out.stdout
            except Exception:
                return False
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def _saved_state(self):
        try:
            return json.loads(self.state_path.read_text(encoding='utf-8'))
        except Exception:
            return {}

    def status(self):
        req = self._required()
        saved = self._saved_state()
        pid = int(saved.get('pid') or 0)
        running = bool(self.process and self.process.poll() is None) or self._pid_alive(pid)
        native_make = shutil.which('make')
        qemu = shutil.which('qemu-system-x86_64')
        wsl = shutil.which('wsl.exe') if os.name == 'nt' else None
        return {
            'integrated': True,
            'mode': 'isolated_qemu_research_lab',
            'source_present': self.source.is_dir(),
            'source_path': str(self.source),
            'pinned_sha': self._pin(),
            'required_files': req,
            'source_complete': bool(req) and all(req.values()),
            'tool_source_files': self._tool_count(),
            'host_platform': platform.system(),
            'native_make': bool(native_make),
            'native_qemu': bool(qemu),
            'wsl_available': bool(wsl),
            'runtime_ready': bool(self.source.is_dir() and ((native_make and os.name != 'nt') or wsl)),
            'running': running,
            'pid': pid if running else None,
            'log_path': str(self.log_path),
            'allowed_actions': sorted(self.ACTION_TARGETS) + ['start', 'stop', 'tail'],
            'arbitrary_shell': False,
            'arbitrary_qemu_args': False,
        }

    def _native_cmd(self, target: str | None, run_target: bool = False):
        if os.name == 'nt':
            if not shutil.which('wsl.exe'):
                raise RuntimeError('WSL is required to run the Fable lab from Windows')
            wp = subprocess.run(
                ['wsl.exe', 'wslpath', '-a', str(self.source)], capture_output=True,
                text=True, timeout=10, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)
            )
            if wp.returncode != 0 or not wp.stdout.strip():
                raise RuntimeError('Could not map the Fable source path into WSL')
            linux_path = wp.stdout.strip()
            make = 'make' + (f' {shlex.quote(target)}' if target else '')
            cmd = f'cd {shlex.quote(linux_path)} && {make}'
            return ['wsl.exe', 'bash', '-lc', cmd], None
        if not shutil.which('make'):
            raise RuntimeError('make is not installed')
        cmd = ['make'] + ([target] if target else [])
        return cmd, str(self.source)

    def run_action(self, action: str, timeout: int = 1800):
        action = str(action)
        if action not in self.ACTION_TARGETS:
            raise ValueError('unsupported Fable lab action')
        if not self.source.is_dir():
            raise RuntimeError('Fable source is missing')
        timeout = max(30, min(int(timeout), 3600))
        target = self.ACTION_TARGETS[action]
        cmd, cwd = self._native_cmd(target)
        started = time.time()
        proc = subprocess.run(
            cmd, cwd=cwd, capture_output=True, text=True, errors='replace', timeout=timeout,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0) if os.name == 'nt' else 0,
        )
        output = ((proc.stdout or '') + ('\n' if proc.stdout and proc.stderr else '') + (proc.stderr or ''))[-120000:]
        result = FableCommandResult(
            proc.returncode == 0, action, int(proc.returncode), int((time.time() - started) * 1000), output
        ).as_dict()
        self.log_path.write_text(output, encoding='utf-8', errors='replace')
        self.traces.verified(f'lab.{action}', {k: v for k, v in result.items() if k != 'output'} | {'output_tail': output[-8000:]},
                             'completed' if result['ok'] else 'failed')
        return result

    def start(self):
        with self.lock:
            st = self.status()
            if st['running']:
                return {'ok': True, 'already_running': True, 'status': st}
            if not self.source.is_dir():
                raise RuntimeError('Fable source is missing')
            cmd, cwd = self._native_cmd('run-nox', run_target=True)
            log = self.log_path.open('a', encoding='utf-8', errors='replace')
            kwargs = {
                'cwd': cwd,
                'stdout': log,
                'stderr': subprocess.STDOUT,
                'stdin': subprocess.DEVNULL,
                'text': True,
            }
            if os.name == 'nt':
                kwargs['creationflags'] = getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            else:
                kwargs['start_new_session'] = True
            self.process = subprocess.Popen(cmd, **kwargs)
            state = {'pid': self.process.pid, 'started': time.time(), 'action': 'run-nox'}
            self.state_path.write_text(json.dumps(state, indent=2), encoding='utf-8')
            self.traces.verified('lab.start', state)
            return {'ok': True, 'status': self.status()}

    def stop(self):
        with self.lock:
            st = self.status()
            pid = st.get('pid')
            if not pid:
                self.state_path.unlink(missing_ok=True)
                return {'ok': True, 'already_stopped': True, 'status': self.status()}
            if self.process and self.process.poll() is None:
                try:
                    if os.name == 'nt':
                        self.process.terminate()
                    else:
                        os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
                    self.process.wait(timeout=10)
                except Exception:
                    try:
                        self.process.kill()
                    except Exception:
                        pass
            else:
                try:
                    if os.name == 'nt':
                        subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True, timeout=10,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    else:
                        os.kill(pid, signal.SIGTERM)
                except Exception:
                    pass
            self.state_path.unlink(missing_ok=True)
            self.traces.verified('lab.stop', {'pid': pid})
            return {'ok': True, 'status': self.status()}

    def tail(self, limit: int = 200):
        limit = max(10, min(int(limit), 1000))
        if not self.log_path.exists():
            return {'lines': [], 'classified': []}
        lines = self.log_path.read_text(encoding='utf-8', errors='replace').splitlines()[-limit:]
        classified = [
            {'kind': 'kernel_candidate' if x.startswith('[') else 'prose', 'line': x}
            for x in lines
        ]
        return {'lines': lines, 'classified': classified}


class FableAgendaEngine:
    """Bounded autonomous scheduler inspired by Fable agenda semantics.

    Agenda entries can invoke only saved SARUS learned capabilities. The actual
    capability execution returns through ExecutionEngine/policy/receipts.
    """

    MAX_ITEMS = 8
    MIN_PERIOD = 60
    MAX_TOTAL_RUNS = 256
    MAX_CONSECUTIVE_FAILURES = 3

    def __init__(self, db: Path, capability_runner):
        self.db = db
        self.capability_runner = capability_runner
        self.stop_evt = threading.Event()
        self.thread: threading.Thread | None = None
        with closing(sqlite3.connect(db)) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS fable_agenda("
                "id TEXT PRIMARY KEY,name TEXT,when_mode TEXT,capability_id TEXT,period_seconds INTEGER,"
                "max_runs INTEGER,run_count INTEGER,consecutive_failures INTEGER,enabled INTEGER,"
                "next_run REAL,last_run REAL,last_status TEXT,created REAL)"
            )
            c.commit()

    def add(self, name: str, when_mode: str, capability_id: str, period_seconds: int = 3600, max_runs: int = 1):
        when_mode = str(when_mode).lower().strip()
        if when_mode not in {'boot', 'once', 'every'}:
            raise ValueError('when must be boot, once or every')
        with closing(sqlite3.connect(self.db)) as c:
            active = c.execute("SELECT COUNT(*) FROM fable_agenda WHERE enabled=1").fetchone()[0]
            if active >= self.MAX_ITEMS:
                raise ValueError('Fable agenda active-item limit reached')
        period = max(self.MIN_PERIOD, int(period_seconds or self.MIN_PERIOD)) if when_mode == 'every' else 0
        max_runs = max(1, min(int(max_runs or 1), self.MAX_TOTAL_RUNS))
        now = time.time()
        nxt = now + period if when_mode == 'every' else (now if when_mode == 'once' else 0)
        aid = str(uuid.uuid4())
        with closing(sqlite3.connect(self.db)) as c:
            c.execute(
                "INSERT INTO fable_agenda VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (aid, str(name).strip()[:100] or 'Fable Agenda', when_mode, str(capability_id), period,
                 max_runs, 0, 0, 1, nxt, 0, 'never', now),
            )
            c.commit()
        return self.get(aid)

    def get(self, aid: str):
        with closing(sqlite3.connect(self.db)) as c:
            r = c.execute("SELECT * FROM fable_agenda WHERE id=?", (aid,)).fetchone()
        if not r:
            return None
        keys = ['id','name','when','capability_id','period_seconds','max_runs','run_count',
                'consecutive_failures','enabled','next_run','last_run','last_status','created']
        out = dict(zip(keys, r))
        out['enabled'] = bool(out['enabled'])
        return out

    def list(self):
        with closing(sqlite3.connect(self.db)) as c:
            ids = [r[0] for r in c.execute("SELECT id FROM fable_agenda ORDER BY created DESC").fetchall()]
        return [self.get(x) for x in ids]

    def set_enabled(self, aid: str, enabled: bool):
        with closing(sqlite3.connect(self.db)) as c:
            cur = c.execute("UPDATE fable_agenda SET enabled=? WHERE id=?", (1 if enabled else 0, aid))
            c.commit()
        if not cur.rowcount:
            raise KeyError('agenda item not found')
        return self.get(aid)

    def _run(self, item):
        ok = False
        status = 'failed'
        try:
            result = self.capability_runner(item['capability_id'], source='fable_agenda')
            ok = bool(result.get('ok', result.get('status') == 'completed'))
            status = 'success' if ok else 'failed'
        except Exception as exc:
            result = {'ok': False, 'error': str(exc)}
        runs = item['run_count'] + 1
        fails = 0 if ok else item['consecutive_failures'] + 1
        enabled = runs < item['max_runs'] and fails < self.MAX_CONSECUTIVE_FAILURES
        if item['when'] in {'boot', 'once'}:
            enabled = False
        nxt = time.time() + item['period_seconds'] if enabled and item['when'] == 'every' else 0
        with closing(sqlite3.connect(self.db)) as c:
            c.execute(
                "UPDATE fable_agenda SET run_count=?,consecutive_failures=?,enabled=?,next_run=?,last_run=?,last_status=? WHERE id=?",
                (runs, fails, 1 if enabled else 0, nxt, time.time(), status, item['id']),
            )
            c.commit()
        return {'agenda': self.get(item['id']), 'result': result}

    def run_boot(self):
        results = []
        for item in self.list():
            if item['enabled'] and item['when'] == 'boot' and item['run_count'] == 0:
                results.append(self._run(item))
                break
        return results

    def tick(self):
        now = time.time()
        total = sum(x['run_count'] for x in self.list())
        if total >= self.MAX_TOTAL_RUNS:
            return []
        for item in self.list():
            if not item['enabled']:
                continue
            if item['when'] in {'once', 'every'} and item['next_run'] <= now:
                return [self._run(item)]
        return []

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.run_boot()

        def loop():
            while not self.stop_evt.wait(20):
                try:
                    self.tick()
                except Exception:
                    pass
        self.thread = threading.Thread(target=loop, name='sarus-fable-agenda', daemon=True)
        self.thread.start()


class FableIntegration:
    """Facade joining the Fable research source with SARUS-native services."""

    def __init__(self, app):
        self.app = app
        source_rel = app.registry.sources.get('fable_os', '')
        self.source = app.root / 'sources' / source_rel
        self.traces = FableTraceStore(app.root / 'data/sarus.db', app.receipts)
        self.capabilities = LearnedCapabilityStore(app.root / 'data/sarus.db')
        self.lab = FableLabManager(app.root, self.source, self.traces)
        self.agenda = FableAgendaEngine(app.root / 'data/sarus.db', self.run_capability)
        self.agenda.start()

    def run_capability(self, cid: str, source: str = 'fable_capability'):
        cap = self.capabilities.get(cid)
        if not cap:
            raise KeyError('Fable capability not found')
        if not cap['enabled']:
            raise PermissionError('Fable capability is disabled')
        task = self.app.execution.run(cap['prompt'], source=source)
        ok = task.get('status') == 'completed'
        cap = self.capabilities.record_result(cid, ok)
        trace = self.traces.verified(
            'capability.run',
            {'capability_id': cid, 'definition_hash': cap['definition_hash'], 'task_id': task.get('id'), 'ok': ok},
            'completed' if ok else 'failed',
            task_id=task.get('id') or 'fable-capability',
        )
        return {'ok': ok, 'capability': cap, 'task': task, 'trace': trace}

    def status(self):
        return {
            'name': 'Fable Intelligence Layer',
            'integrated': True,
            'source': self.lab.status(),
            'learned_capabilities': len(self.capabilities.list()),
            'agenda': {
                'items': len(self.agenda.list()),
                'limits': {
                    'max_items': self.agenda.MAX_ITEMS,
                    'min_period_seconds': self.agenda.MIN_PERIOD,
                    'max_total_runs': self.agenda.MAX_TOTAL_RUNS,
                    'max_consecutive_failures': self.agenda.MAX_CONSECUTIVE_FAILURES,
                    'one_action_per_tick': True,
                },
            },
            'trace': {
                'verified_receipt_chain': self.app.receipts.verify_chain()['ok'],
                'recent_count': len(self.traces.recent(100)),
                'model_prose_is_not_proof': True,
            },
            'direct_kernel_replacement': False,
            'integration_model': 'SARUS-native concepts + isolated original Fable QEMU lab',
        }
