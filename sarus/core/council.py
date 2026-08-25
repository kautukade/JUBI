from __future__ import annotations

import hashlib
import json
import re
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


class AICouncil:
    """Multi-model deliberation that preserves Jubi provider/privacy policy.

    Council members are ordinary model calls; they do not execute tools or
    privileged actions. High-privacy prompts remain local because every call is
    routed through ProviderManager. Council history stores prompt hashes and
    result metadata, not the original prompt text.
    """

    def __init__(self, db: Path, brain, providers, event_bus=None):
        self.db = db
        self.brain = brain
        self.providers = providers
        self.event_bus = event_bus
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS council_runs("
                "id TEXT PRIMARY KEY,ts REAL,prompt_hash TEXT,task_type TEXT,privacy TEXT,mode TEXT,"
                "member_count INTEGER,success_count INTEGER,judge_provider TEXT,judge_model TEXT,"
                "status TEXT,latency_ms REAL,metadata TEXT)"
            )

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is not None:
            try:
                self.event_bus.emit(kind, payload)
            except Exception:
                pass

    def _members(self, prompt: str, task_type: str, max_members: int) -> tuple[dict, list[dict]]:
        preview = self.providers.route_preview(prompt, task_type, 'auto')
        specs: list[dict] = []
        try:
            route = self.brain.route(prompt, task_type)
            for item in route.get('candidates', [])[:3]:
                specs.append({'provider': 'ollama', 'model': item.get('model'), 'label': f"Local · {item.get('model')}"})
        except Exception:
            pass
        if preview.get('mode') != 'local_only' and not preview.get('high_privacy_cloud_blocked'):
            for provider in preview.get('cloud_configured', []):
                specs.append({'provider': provider, 'model': None, 'label': provider})
        dedup = []
        seen = set()
        for item in specs:
            key = (item['provider'], item.get('model') or '')
            if key not in seen:
                seen.add(key)
                dedup.append(item)
        return preview, dedup[:max(1, min(int(max_members), 6))]

    def run(self, prompt: str, task_type: str = 'auto', max_members: int = 4, judge_provider: str = 'auto') -> dict:
        prompt = str(prompt or '').strip()
        if not prompt:
            raise ValueError('council prompt is required')
        started = time.perf_counter()
        preview, specs = self._members(prompt, task_type, max_members)
        if not specs:
            raise RuntimeError('No compatible Council members are currently available')
        run_id = str(uuid.uuid4())
        answers = []
        errors = []
        for index, spec in enumerate(specs, start=1):
            try:
                result = self.providers.generate(
                    prompt,
                    task_type=preview['task_type'],
                    provider=spec['provider'],
                    model=spec.get('model'),
                    system=(
                        'You are an independent Jubi Council member. Solve the problem independently. '
                        'State assumptions, key reasoning evidence, risks and a concrete recommendation. '
                        'Do not claim tool execution unless the prompt itself contains verified evidence.'
                    ),
                )
                answers.append({
                    'member': index, 'provider': (result.get('jubi_provider_route') or {}).get('provider', spec['provider']),
                    'model': result.get('model') or spec.get('model'), 'label': spec['label'],
                    'answer': str(result.get('response') or result.get('output') or '').strip(),
                    'route': result.get('jubi_provider_route') or result.get('jubi_route') or {},
                })
            except Exception as exc:
                errors.append({'member': index, 'provider': spec['provider'], 'model': spec.get('model'), 'error': str(exc)[:1000]})
        if not answers:
            raise RuntimeError('Every Council member failed: ' + '; '.join(x['error'] for x in errors))
        packet = []
        for item in answers:
            packet.append(f"MEMBER {item['member']} — {item['provider']} / {item['model']}\n{item['answer'][:10000]}")
        judge_prompt = (
            'Original problem:\n' + prompt + '\n\nIndependent Council answers:\n\n' + '\n\n'.join(packet) +
            '\n\nAct as the Council Judge. Compare the answers, identify agreements/disagreements, reject unsupported claims, '
            'and produce one best final answer. Prefer evidence and practical correctness over majority vote.'
        )
        judge = self.providers.generate(
            judge_prompt,
            task_type='planning',
            provider=judge_provider,
            system='You are Jubi Council Judge. Synthesize multiple independent model answers into one rigorous final result.',
        )
        elapsed = (time.perf_counter() - started) * 1000.0
        jroute = judge.get('jubi_provider_route') or judge.get('jubi_route') or {}
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO council_runs(id,ts,prompt_hash,task_type,privacy,mode,member_count,success_count,"
                "judge_provider,judge_model,status,latency_ms,metadata) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    run_id, time.time(), hashlib.sha256(prompt.encode('utf-8', errors='replace')).hexdigest(),
                    preview['task_type'], preview['privacy'], preview['mode'], len(specs), len(answers),
                    str(jroute.get('provider') or 'ollama'), str(judge.get('model') or jroute.get('selected_model') or ''),
                    'success', elapsed, json.dumps({'errors': errors}, ensure_ascii=False),
                ),
            )
        self._emit('COUNCIL_COMPLETED', {'id': run_id, 'members': len(answers), 'errors': len(errors), 'latency_ms': round(elapsed, 2)})
        return {
            'id': run_id, 'classification': preview, 'members': answers, 'errors': errors,
            'final': str(judge.get('response') or judge.get('output') or '').strip(),
            'judge_route': jroute, 'latency_ms': round(elapsed, 2),
        }

    def recent(self, limit: int = 50) -> list[dict]:
        limit = max(1, min(int(limit), 200))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,prompt_hash,task_type,privacy,mode,member_count,success_count,judge_provider,judge_model,status,latency_ms,metadata "
                "FROM council_runs ORDER BY ts DESC LIMIT ?", (limit,)
            ).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'prompt_hash': r[2], 'task_type': r[3], 'privacy': r[4], 'mode': r[5],
             'member_count': r[6], 'success_count': r[7], 'judge_provider': r[8], 'judge_model': r[9],
             'status': r[10], 'latency_ms': r[11], 'metadata': json.loads(r[12] or '{}')}
            for r in rows
        ]


class MultiAgentSupervisor:
    """Reasoning-only planner/supervisor for complex tasks.

    It decomposes work, asks specialist model roles to produce artifacts, and
    performs a final review. It intentionally does not call Windows/LAN/browser
    tools directly; execution remains behind Jubi's existing policy/broker and
    dedicated tool layers.
    """

    ROLE_SYSTEMS = {
        'planner': 'You are Jubi Planner. Decompose complex work into ordered, testable steps.',
        'coding': 'You are Jubi Coding Specialist. Produce technically precise implementation guidance or code-oriented analysis.',
        'research': 'You are Jubi Research Specialist. Separate facts, assumptions, unknowns and evidence needs.',
        'business': 'You are Jubi Business Specialist. Focus on practical operations, customers, cost, risk and deliverables.',
        'document': 'You are Jubi Documentation Specialist. Produce clear structured professional content.',
        'reviewer': 'You are Jubi Reviewer. Find defects, contradictions, missing validation and unsafe assumptions.',
        'general': 'You are a Jubi Specialist. Complete the assigned subtask carefully and concretely.',
    }

    def __init__(self, db: Path, brain, providers, knowledge, experience, event_bus=None):
        self.db = db
        self.brain = brain
        self.providers = providers
        self.knowledge = knowledge
        self.experience = experience
        self.event_bus = event_bus
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS supervisor_runs("
                "id TEXT PRIMARY KEY,ts REAL,request_hash TEXT,task_type TEXT,status TEXT,step_count INTEGER,"
                "provider TEXT,model TEXT,latency_ms REAL,plan TEXT,results TEXT,review TEXT)"
            )

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is not None:
            try:
                self.event_bus.emit(kind, payload)
            except Exception:
                pass

    @staticmethod
    def _extract_json(text: str):
        text = str(text or '').strip()
        candidates = [text]
        fenced = re.findall(r'```(?:json)?\s*(.*?)```', text, flags=re.I | re.S)
        candidates.extend(fenced)
        match = re.search(r'(\{.*\}|\[.*\])', text, flags=re.S)
        if match:
            candidates.append(match.group(1))
        for candidate in candidates:
            try:
                return json.loads(candidate)
            except Exception:
                continue
        return None

    def _context(self, request: str, task_type: str) -> dict:
        similar = []
        knowledge = []
        try:
            similar = self.experience.similar(request, task_type=task_type, limit=4)
        except Exception:
            pass
        try:
            knowledge = self.knowledge.search(request, limit=4)
        except Exception:
            pass
        return {'experiences': similar, 'knowledge': knowledge}

    def plan(self, request: str, task_type: str = 'auto', provider: str = 'auto') -> dict:
        request = str(request or '').strip()
        if not request:
            raise ValueError('supervisor request is required')
        classification = self.brain.classify(request, task_type)
        context = self._context(request, classification['task_type'])
        context_text = json.dumps({
            'past_experiences': [
                {'task': x.get('request'), 'success': x.get('success'), 'lesson': x.get('lesson'), 'provider': x.get('provider'), 'model': x.get('model')}
                for x in context['experiences']
            ],
            'local_knowledge': [
                {'title': x.get('title'), 'source': x.get('source'), 'text': str(x.get('text', ''))[:1200]}
                for x in context['knowledge']
            ],
        }, ensure_ascii=False)
        prompt = (
            'Task:\n' + request + '\n\nRelevant local context (may be empty):\n' + context_text +
            '\n\nReturn ONLY valid JSON with this shape: '
            '{"goal":"...","steps":[{"id":"S1","role":"coding|research|business|document|general","task":"...","deliverable":"...","depends_on":[],"risk":"low|medium|high"}],"verification":["..."]}. '
            'Keep 2-8 steps. This is a reasoning plan only; do not claim that tools or system actions have already run.'
        )
        result = self.providers.generate(prompt, task_type='planning', provider=provider, system=self.ROLE_SYSTEMS['planner'])
        parsed = self._extract_json(result.get('response') or result.get('output'))
        if not isinstance(parsed, dict) or not isinstance(parsed.get('steps'), list):
            parsed = {
                'goal': request,
                'steps': [{'id': 'S1', 'role': classification['task_type'] if classification['task_type'] in self.ROLE_SYSTEMS else 'general', 'task': request, 'deliverable': 'Completed specialist response', 'depends_on': [], 'risk': 'medium'}],
                'verification': ['Review the result against the original request.'],
            }
        parsed['steps'] = parsed['steps'][:8]
        return {'classification': classification, 'plan': parsed, 'planner_route': result.get('jubi_provider_route') or result.get('jubi_route') or {}}

    def run(self, request: str, task_type: str = 'auto', provider: str = 'auto') -> dict:
        request = str(request or '').strip()
        started = time.perf_counter()
        planned = self.plan(request, task_type, provider)
        plan = planned['plan']
        results = []
        for step in plan.get('steps', []):
            role = str(step.get('role') or 'general').lower()
            if role not in self.ROLE_SYSTEMS or role in {'planner', 'reviewer'}:
                role = 'general'
            prior = '\n\n'.join(f"{x['id']}: {x['output'][:3000]}" for x in results[-3:])
            prompt = (
                f"Overall goal:\n{request}\n\nAssigned step {step.get('id')}:\n{step.get('task')}\n"
                f"Expected deliverable: {step.get('deliverable')}\nPrevious step outputs:\n{prior or 'None'}\n\n"
                'Complete only this reasoning subtask. Do not claim external tool execution unless verified evidence is supplied.'
            )
            try:
                out = self.providers.generate(prompt, task_type=role if role in {'coding','research','document'} else 'general', provider=provider, system=self.ROLE_SYSTEMS[role])
                results.append({'id': step.get('id'), 'role': role, 'status': 'success', 'output': str(out.get('response') or out.get('output') or ''), 'route': out.get('jubi_provider_route') or out.get('jubi_route') or {}})
            except Exception as exc:
                results.append({'id': step.get('id'), 'role': role, 'status': 'failed', 'output': '', 'error': str(exc)[:1000], 'route': {}})
        packet = '\n\n'.join(f"{x['id']} ({x['role']}, {x['status']}):\n{x.get('output') or x.get('error','')}" for x in results)
        review_prompt = (
            'Original task:\n' + request + '\n\nPlan:\n' + json.dumps(plan, ensure_ascii=False) +
            '\n\nSpecialist outputs:\n' + packet +
            '\n\nReview the work. Identify missing requirements, contradictions, unverifiable claims and concrete fixes. Then provide a final integrated result.'
        )
        review = self.providers.generate(review_prompt, task_type='planning', provider=provider, system=self.ROLE_SYSTEMS['reviewer'])
        elapsed = (time.perf_counter() - started) * 1000.0
        route = review.get('jubi_provider_route') or review.get('jubi_route') or {}
        run_id = str(uuid.uuid4())
        status = 'completed' if all(x['status'] == 'success' for x in results) else 'partial'
        final_text = str(review.get('response') or review.get('output') or '').strip()
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO supervisor_runs(id,ts,request_hash,task_type,status,step_count,provider,model,latency_ms,plan,results,review) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    run_id, time.time(), hashlib.sha256(request.encode('utf-8', errors='replace')).hexdigest(),
                    planned['classification']['task_type'], status, len(results), str(route.get('provider') or 'ollama'),
                    str(review.get('model') or route.get('selected_model') or ''), elapsed,
                    json.dumps(plan, ensure_ascii=False), json.dumps(results, ensure_ascii=False), final_text,
                ),
            )
        try:
            self.experience.record(request, final_text[:4000], status == 'completed', task_type=planned['classification']['task_type'], kind='supervisor', provider=str(route.get('provider') or ''), model=str(review.get('model') or ''), latency_ms=elapsed, lesson='Supervisor outcome stored for future similar planning.')
        except Exception:
            pass
        self._emit('SUPERVISOR_COMPLETED', {'id': run_id, 'status': status, 'steps': len(results), 'latency_ms': round(elapsed, 2)})
        return {'id': run_id, 'status': status, 'plan': plan, 'results': results, 'final': final_text, 'review_route': route, 'latency_ms': round(elapsed, 2)}

    def recent(self, limit: int = 30) -> list[dict]:
        limit = max(1, min(int(limit), 100))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,request_hash,task_type,status,step_count,provider,model,latency_ms FROM supervisor_runs ORDER BY ts DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {'id': r[0], 'ts': r[1], 'request_hash': r[2], 'task_type': r[3], 'status': r[4],
             'step_count': r[5], 'provider': r[6], 'model': r[7], 'latency_ms': r[8]}
            for r in rows
        ]
