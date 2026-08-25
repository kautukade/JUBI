from __future__ import annotations

from dataclasses import asdict
import json
import threading
import time
import traceback
import uuid

from .database import read_connection, transaction
from .orchestrator import Step


TASK_STATES = {
    'queued', 'planning', 'running', 'waiting_approval', 'completed',
    'partial', 'failed', 'rejected', 'cancelled',
}
STEP_STATES = {
    'queued', 'running', 'waiting_approval', 'completed', 'failed',
    'denied', 'rejected', 'skipped',
}


class ExecutionEngine:
    """Persistent, resumable execution engine.

    Planning is performed once and the serialized plan is stored in SQLite.
    Approval-required tasks can therefore survive a Jubi restart and resume the
    exact pending step instead of regenerating a potentially different plan.
    """

    def __init__(self, app):
        self.app = app
        self.db = app.db_path
        self._approval_lock = threading.Lock()
        with transaction(self.db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS tasks("
                "id TEXT PRIMARY KEY,ts REAL,request TEXT,status TEXT,result TEXT,source TEXT)"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS approvals("
                "id TEXT PRIMARY KEY,ts REAL,task_id TEXT,step_id TEXT,action TEXT,status TEXT,payload TEXT)"
            )
            cols = {r[1] for r in c.execute('PRAGMA table_info(approvals)').fetchall()}
            if 'resolved_ts' not in cols:
                c.execute("ALTER TABLE approvals ADD COLUMN resolved_ts REAL DEFAULT 0")
            if 'consumed' not in cols:
                c.execute("ALTER TABLE approvals ADD COLUMN consumed INTEGER DEFAULT 0")
            c.execute(
                "CREATE TABLE IF NOT EXISTS task_state("
                "task_id TEXT PRIMARY KEY,request TEXT,source TEXT,capability_id TEXT,"
                "steps TEXT,current_index INTEGER,results TEXT,context TEXT,status TEXT,updated REAL)"
            )

    def _save_task(self, tid, request, status, result=None, source='user'):
        if status not in TASK_STATES:
            raise ValueError('invalid task status: ' + str(status))
        with transaction(self.db) as c:
            c.execute(
                "INSERT OR REPLACE INTO tasks VALUES(?,?,?,?,?,?)",
                (
                    tid,
                    time.time(),
                    request,
                    status,
                    json.dumps(result, ensure_ascii=False, default=str) if result is not None else '',
                    source,
                ),
            )

    def _save_state(
        self,
        tid: str,
        request: str,
        source: str,
        capability_id,
        steps: list[dict],
        current_index: int,
        results: list,
        context: list,
        status: str,
    ):
        if status not in TASK_STATES:
            raise ValueError('invalid task status: ' + str(status))
        with transaction(self.db) as c:
            c.execute(
                "INSERT OR REPLACE INTO task_state VALUES(?,?,?,?,?,?,?,?,?,?)",
                (
                    tid,
                    request,
                    source,
                    capability_id or '',
                    json.dumps(steps, ensure_ascii=False, default=str),
                    int(current_index),
                    json.dumps(results, ensure_ascii=False, default=str),
                    json.dumps(context, ensure_ascii=False, default=str),
                    status,
                    time.time(),
                ),
            )

    def _load_state(self, tid: str):
        with read_connection(self.db) as c:
            row = c.execute(
                "SELECT task_id,request,source,capability_id,steps,current_index,results,context,status,updated "
                "FROM task_state WHERE task_id=?",
                (tid,),
            ).fetchone()
        if not row:
            return None
        return {
            'task_id': row[0],
            'request': row[1],
            'source': row[2],
            'capability_id': row[3] or None,
            'steps': json.loads(row[4] or '[]'),
            'current_index': int(row[5] or 0),
            'results': json.loads(row[6] or '[]'),
            'context': json.loads(row[7] or '[]'),
            'status': row[8],
            'updated': row[9],
        }

    def create_approval(self, tid, step, payload):
        with transaction(self.db) as c:
            existing = c.execute(
                "SELECT id FROM approvals WHERE task_id=? AND step_id=? AND status='pending' AND consumed=0 "
                "ORDER BY ts DESC LIMIT 1",
                (tid, step.id),
            ).fetchone()
            if existing:
                return existing[0]
            aid = str(uuid.uuid4())
            c.execute(
                "INSERT INTO approvals(id,ts,task_id,step_id,action,status,payload,resolved_ts,consumed) "
                "VALUES(?,?,?,?,?,?,?,?,?)",
                (
                    aid,
                    time.time(),
                    tid,
                    step.id,
                    step.task,
                    'pending',
                    json.dumps(payload, ensure_ascii=False, default=str),
                    0,
                    0,
                ),
            )
        return aid

    def approvals(self, status='pending'):
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,task_id,step_id,action,status,payload,resolved_ts,consumed "
                "FROM approvals WHERE status=? ORDER BY ts DESC",
                (status,),
            ).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'task_id': r[2], 'step_id': r[3],
                'action': r[4], 'status': r[5], 'payload': json.loads(r[6] or '{}'),
                'resolved_ts': r[7], 'consumed': bool(r[8]),
            }
            for r in rows
        ]

    def _approval(self, aid: str):
        with read_connection(self.db) as c:
            r = c.execute(
                "SELECT id,ts,task_id,step_id,action,status,payload,resolved_ts,consumed "
                "FROM approvals WHERE id=?",
                (aid,),
            ).fetchone()
        if not r:
            return None
        return {
            'id': r[0], 'ts': r[1], 'task_id': r[2], 'step_id': r[3],
            'action': r[4], 'status': r[5], 'payload': json.loads(r[6] or '{}'),
            'resolved_ts': r[7], 'consumed': bool(r[8]),
        }

    def set_approval(self, aid, status):
        if status not in {'approved', 'rejected'}:
            raise ValueError('approval status must be approved or rejected')
        with self._approval_lock:
            approval = self._approval(aid)
            if not approval:
                raise KeyError('approval not found')
            if approval['status'] != 'pending' or approval['consumed']:
                raise RuntimeError('approval has already been resolved')

            with transaction(self.db) as c:
                cur = c.execute(
                    "UPDATE approvals SET status=?,resolved_ts=? WHERE id=? AND status='pending' AND consumed=0",
                    (status, time.time(), aid),
                )
            if not cur.rowcount:
                raise RuntimeError('approval was resolved concurrently')

            state = self._load_state(approval['task_id'])
            if not state:
                raise RuntimeError('task state required for approval resume is missing')
            if state['status'] != 'waiting_approval':
                raise RuntimeError('task is not waiting for approval')
            if state['current_index'] >= len(state['steps']):
                raise RuntimeError('pending approval points outside the persisted plan')
            pending = state['steps'][state['current_index']]
            if pending.get('id') != approval['step_id']:
                raise RuntimeError('approval does not match the task pending step')

            if status == 'rejected':
                pending['status'] = 'rejected'
                payload = self._task_payload(state, 'rejected')
                self._save_state(
                    state['task_id'], state['request'], state['source'], state['capability_id'],
                    state['steps'], state['current_index'], state['results'], state['context'], 'rejected',
                )
                self._save_task(state['task_id'], state['request'], 'rejected', payload, state['source'])
                with transaction(self.db) as c:
                    c.execute("UPDATE approvals SET consumed=1 WHERE id=?", (aid,))
                self.app.bus.emit(
                    'APPROVAL_REJECTED',
                    {'approval_id': aid, 'task_id': state['task_id'], 'step_id': approval['step_id']},
                )
                return {'id': aid, 'status': 'rejected', 'task': payload}

            self.app.bus.emit(
                'APPROVAL_APPROVED',
                {'approval_id': aid, 'task_id': state['task_id'], 'step_id': approval['step_id']},
            )
            result = self._continue_task(state['task_id'], approved_step_id=approval['step_id'])
            with transaction(self.db) as c:
                c.execute("UPDATE approvals SET consumed=1 WHERE id=?", (aid,))
            return {'id': aid, 'status': 'approved', 'task': result}

    @staticmethod
    def _task_payload(state: dict, status: str):
        # ``id`` is retained as a compatibility alias for the SARUS-era Fable
        # code while ``task_id`` is the canonical identifier going forward.
        return {
            'id': state['task_id'],
            'task_id': state['task_id'],
            'request': state['request'],
            'status': status,
            'steps': state['results'],
            'current_index': state['current_index'],
            'total_steps': len(state['steps']),
        }

    def _execute_step(self, tid: str, step: Step, request: str, capability_id, context: list):
        action = 'defensive_readonly' if step.source == 'cai' else step.task
        policy = self.app.policy.evaluate(action, step.risk, step.source)
        rec = asdict(step) | {'policy': policy}
        self.app.bus.emit('STEP_STARTED', {'task_id': tid, **rec})

        if policy['decision'] == 'deny':
            out = {'ok': False, 'status': 'denied', 'reason': policy['reason']}
        else:
            try:
                adapter = self.app.adapters.get(step.source)
                cap_meta = self.app.registry.get(capability_id) if capability_id else None
                selected = capability_id if cap_meta and cap_meta.get('source') == step.source else None
                out = adapter.execute(
                    step.task + '\n\nOriginal user request: ' + request,
                    self.app,
                    step,
                    selected,
                    context[-3:],
                )
                if policy['decision'] == 'isolated' and isinstance(out, dict):
                    out['policy_isolation'] = True
            except Exception as exc:
                out = {
                    'ok': False,
                    'status': 'error',
                    'error': str(exc),
                    'trace': traceback.format_exc(limit=3),
                }

        status = 'completed' if out.get('ok') else out.get('status', 'failed')
        if status not in STEP_STATES:
            status = 'failed'
        receipt = self.app.receipts.create(tid, step.id, step.source, status, out)
        item = rec | {'status': status, 'result': out, 'receipt': receipt}
        self.app.bus.emit(
            'STEP_FINISHED',
            {'task_id': tid, 'step_id': step.id, 'source': step.source, 'status': status, 'receipt': receipt['hash']},
        )
        return item, out, policy

    def _continue_task(self, tid: str, approved_step_id: str | None = None):
        state = self._load_state(tid)
        if not state:
            raise KeyError('task state not found')
        if state['status'] in {'completed', 'partial', 'failed', 'rejected', 'cancelled'}:
            return self._task_payload(state, state['status'])

        state['status'] = 'running'
        self._save_state(
            tid, state['request'], state['source'], state['capability_id'], state['steps'],
            state['current_index'], state['results'], state['context'], 'running',
        )
        self._save_task(tid, state['request'], 'running', self._task_payload(state, 'running'), state['source'])
        self.app.bus.emit('TASK_RESUMED', {'task_id': tid, 'from_index': state['current_index']})

        while state['current_index'] < len(state['steps']):
            idx = state['current_index']
            raw = dict(state['steps'][idx])
            step = Step(**{k: raw[k] for k in ('id', 'agent', 'source', 'task', 'risk', 'status') if k in raw})
            action = 'defensive_readonly' if step.source == 'cai' else step.task
            policy = self.app.policy.evaluate(action, step.risk, step.source)

            if policy['decision'] == 'approval' and step.id != approved_step_id:
                step.status = 'waiting_approval'
                state['steps'][idx] = asdict(step)
                rec = asdict(step) | {'policy': policy, 'step_index': idx}
                aid = self.create_approval(tid, step, rec)
                state['status'] = 'waiting_approval'
                self._save_state(
                    tid, state['request'], state['source'], state['capability_id'], state['steps'],
                    idx, state['results'], state['context'], 'waiting_approval',
                )
                payload = self._task_payload(state, 'waiting_approval') | {'approval_id': aid}
                self._save_task(tid, state['request'], 'waiting_approval', payload, state['source'])
                self.app.bus.emit(
                    'APPROVAL_REQUIRED',
                    {'task_id': tid, 'approval_id': aid, 'step_id': step.id, 'step_index': idx, **rec},
                )
                return payload

            use_approval = approved_step_id == step.id
            if use_approval:
                approved_step_id = None

            step.status = 'running'
            state['steps'][idx] = asdict(step)
            self._save_state(
                tid, state['request'], state['source'], state['capability_id'], state['steps'],
                idx, state['results'], state['context'], 'running',
            )

            if use_approval and policy['decision'] == 'approval':
                try:
                    adapter = self.app.adapters.get(step.source)
                    cap_meta = self.app.registry.get(state['capability_id']) if state['capability_id'] else None
                    selected = state['capability_id'] if cap_meta and cap_meta.get('source') == step.source else None
                    out = adapter.execute(
                        step.task + '\n\nOriginal user request: ' + state['request'],
                        self.app,
                        step,
                        selected,
                        state['context'][-3:],
                    )
                except Exception as exc:
                    out = {'ok': False, 'status': 'error', 'error': str(exc), 'trace': traceback.format_exc(limit=3)}
                step_status = 'completed' if out.get('ok') else out.get('status', 'failed')
                if step_status not in STEP_STATES:
                    step_status = 'failed'
                rec = asdict(step) | {'policy': policy, 'approved': True}
                receipt = self.app.receipts.create(tid, step.id, step.source, step_status, out)
                item = rec | {'status': step_status, 'result': out, 'receipt': receipt}
                self.app.bus.emit(
                    'STEP_FINISHED',
                    {'task_id': tid, 'step_id': step.id, 'source': step.source, 'status': step_status, 'receipt': receipt['hash']},
                )
            else:
                item, out, _ = self._execute_step(
                    tid, step, state['request'], state['capability_id'], state['context']
                )
                step_status = item['status']

            step.status = step_status
            state['steps'][idx] = asdict(step)
            state['results'].append(item)
            state['context'].append({'source': step.source, 'result': out.get('output', out)})
            state['current_index'] = idx + 1

            if step.source == 'second_brain' and out.get('ok'):
                try:
                    self.app.memory.add(
                        str(out.get('output', '')),
                        title='Pipeline knowledge: ' + state['request'][:100],
                        namespace='pipeline',
                        metadata={'task_id': tid, 'source': 'second_brain'},
                    )
                except Exception as exc:
                    self.app.bus.emit('MEMORY_WRITE_FAILED', {'task_id': tid, 'error': str(exc)[:1000]})

            self._save_state(
                tid, state['request'], state['source'], state['capability_id'], state['steps'],
                state['current_index'], state['results'], state['context'], 'running',
            )

        failed = any(not x.get('result', {}).get('ok', False) for x in state['results'])
        final_status = 'partial' if failed else 'completed'
        state['status'] = final_status
        self._save_state(
            tid, state['request'], state['source'], state['capability_id'], state['steps'],
            state['current_index'], state['results'], state['context'], final_status,
        )
        payload = self._task_payload(state, final_status)
        self._save_task(tid, state['request'], final_status, payload, state['source'])
        self.app.bus.emit('TASK_FINISHED', {'task_id': tid, 'status': final_status})
        return payload

    def run(self, request: str, source='user', capability_id=None):
        request = str(request).strip()
        if not request:
            raise ValueError('task request cannot be empty')
        tid = str(uuid.uuid4())
        self._save_task(tid, request, 'planning', {'id': tid, 'task_id': tid}, source)
        self.app.bus.emit('TASK_STARTED', {'task_id': tid, 'request': request, 'source': source})
        steps = self.app.orchestrator.plan(request)
        serialized = [asdict(step) for step in steps]
        self._save_state(tid, request, source, capability_id, serialized, 0, [], [], 'queued')
        return self._continue_task(tid)

    def recent_tasks(self, limit=50):
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,request,status,result,source FROM tasks ORDER BY ts DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                'id': x[0], 'task_id': x[0], 'ts': x[1], 'request': x[2], 'status': x[3],
                'result': json.loads(x[4]) if x[4] else None, 'source': x[5],
            }
            for x in rows
        ]
