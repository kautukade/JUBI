from __future__ import annotations

from dataclasses import dataclass, asdict
import hashlib
import json
import math
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


@dataclass
class BrainDecision:
    decision_id: str
    intent: str
    task_type: str
    complexity: int
    privacy: str
    tool_hint: str
    selected_model: str | None
    candidates: list[dict]
    reason: str
    mode: str = 'local-first'


class BrainRouter:
    """Adaptive local-first task classifier and model router for Jubi.

    Phase 1 intentionally stays local. It classifies the request, scores only
    models actually reported by Ollama, excludes cloud-through-Ollama models
    from automatic routing, records outcomes, and uses those outcomes to make
    later decisions better. Explicit model selection remains available to the
    user, but automatic routing never silently escalates to a cloud model.
    """

    INTENT_KEYWORDS = {
        'vision': (
            'screenshot', 'image', 'photo', 'picture', 'ui screenshot', 'screen error',
            'देख', 'फोटो', 'स्क्रीनशॉट', 'image analyze', 'vision',
        ),
        'coding': (
            'code', 'coding', 'python', 'javascript', 'typescript', 'react', 'fastapi',
            'django', 'flask', 'html', 'css', 'bug', 'debug', 'error fix', 'repository',
            'repo', 'git', 'npm', 'function', 'class', 'api endpoint', 'sql', 'database query',
            'compile', 'build failed', 'developer', 'development task',
        ),
        'research': (
            'research', 'compare sources', 'find sources', 'deep research', 'investigate',
            'study', 'evidence', 'latest information', 'web search', 'research karo',
        ),
        'planning': (
            'plan', 'roadmap', 'architecture', 'design system', 'strategy', 'steps',
            'implementation plan', 'project plan', 'workflow design',
        ),
        'document': (
            'document', 'report', 'proposal', 'email', 'draft', 'letter', 'jd ',
            'project report', 'readme', 'documentation', 'summary', 'summarize',
        ),
        'system': (
            'powershell', 'cmd', 'terminal', 'process', 'service', 'windows', 'computer',
            'file system', 'folder', 'ollama service', 'system health', 'install package',
        ),
    }

    COMPLEXITY_MARKERS = (
        'architecture', 'production', 'complete', 'detailed', 'deep', 'multi-step',
        'end to end', 'end-to-end', 'debug', 'analyze', 'compare', 'refactor', 'migration',
        'security', 'performance', 'optimize', 'entire', 'full project', 'all files',
    )

    SENSITIVE_MARKERS = (
        'password', 'secret', 'api key', 'token', 'credential', 'private key',
        'personal data', 'confidential', 'bank', 'aadhaar', 'pan card',
    )

    def __init__(self, db: Path, models, config_path: Path, event_bus=None):
        self.db = db
        self.models = models
        self.event_bus = event_bus
        self.cfg = self._load_config(config_path)
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS brain_decisions("
                "id TEXT PRIMARY KEY,ts REAL,prompt_hash TEXT,prompt_length INTEGER,"
                "intent TEXT,task_type TEXT,complexity INTEGER,privacy TEXT,tool_hint TEXT,"
                "selected_model TEXT,reason TEXT,status TEXT,latency_ms REAL,error TEXT)"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS model_performance("
                "model TEXT,task_type TEXT,successes INTEGER DEFAULT 0,failures INTEGER DEFAULT 0,"
                "total_latency_ms REAL DEFAULT 0,last_latency_ms REAL DEFAULT 0,"
                "last_status TEXT DEFAULT '',updated REAL DEFAULT 0,"
                "PRIMARY KEY(model,task_type))"
            )

    @staticmethod
    def _load_config(path: Path) -> dict:
        defaults = {
            'mode': 'local-first',
            'allow_cloud_through_ollama_by_default': False,
            'max_fallback_attempts': 3,
            'latency_target_ms': 30000,
            'performance_weight': 22.0,
            'latency_weight': 8.0,
            'configured_priority_weight': 16.0,
            'kind_match_weight': 100.0,
            'task_types': {
                'general': ['general', 'coding'],
                'coding': ['coding', 'general'],
                'vision': ['vision'],
                'research': ['general', 'coding'],
                'planning': ['general', 'coding'],
                'document': ['general', 'coding'],
                'system': ['general', 'coding'],
            },
        }
        try:
            loaded = json.loads(path.read_text(encoding='utf-8'))
            if isinstance(loaded, dict):
                defaults.update(loaded)
        except Exception:
            pass
        return defaults

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is None:
            return
        try:
            self.event_bus.emit(kind, payload)
        except Exception:
            pass

    @staticmethod
    def _contains(text: str, words: tuple[str, ...]) -> bool:
        return any(word in text for word in words)

    def classify(self, text: str, explicit_task_type: str | None = None) -> dict:
        raw = str(text or '').strip()
        if not raw:
            raise ValueError('brain routing requires a non-empty request')
        low = raw.lower()

        explicit = str(explicit_task_type or '').strip().lower()
        if explicit and explicit not in {'auto', 'smart'}:
            intent = explicit if explicit in self.cfg.get('task_types', {}) else 'general'
            task_type = explicit
        else:
            intent = 'general'
            # Order matters: vision and coding should beat generic planning terms.
            for name in ('vision', 'coding', 'research', 'system', 'document', 'planning'):
                if self._contains(low, self.INTENT_KEYWORDS[name]):
                    intent = name
                    break
            task_type = intent

        words = len(raw.split())
        complexity = 1
        if words >= 20:
            complexity += 1
        if words >= 60:
            complexity += 1
        marker_count = sum(1 for marker in self.COMPLEXITY_MARKERS if marker in low)
        if marker_count >= 1:
            complexity += 1
        if marker_count >= 3 or words >= 140:
            complexity += 1
        complexity = max(1, min(complexity, 5))

        privacy = 'high' if self._contains(low, self.SENSITIVE_MARKERS) else 'local'
        tool_hint = {
            'vision': 'vision-input',
            'system': 'typed-system-tools',
            'research': 'research-tools-later-phase',
            'coding': 'development-tools',
        }.get(intent, 'none')

        return {
            'intent': intent,
            'task_type': task_type,
            'complexity': complexity,
            'privacy': privacy,
            'tool_hint': tool_hint,
            'prompt_length': len(raw),
        }

    def _performance_rows(self) -> dict[tuple[str, str], dict]:
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT model,task_type,successes,failures,total_latency_ms,last_latency_ms,last_status,updated "
                "FROM model_performance"
            ).fetchall()
        out = {}
        for row in rows:
            out[(row[0], row[1])] = {
                'model': row[0], 'task_type': row[1], 'successes': int(row[2] or 0),
                'failures': int(row[3] or 0), 'total_latency_ms': float(row[4] or 0),
                'last_latency_ms': float(row[5] or 0), 'last_status': row[6] or '',
                'updated': float(row[7] or 0),
            }
        return out

    def _score_model(self, item: dict, task_type: str, performance: dict) -> tuple[float, list[str]]:
        model = item['name']
        kind = item.get('kind', 'unknown')
        allowed_kinds = self.cfg.get('task_types', {}).get(task_type)
        if not allowed_kinds:
            allowed_kinds = self.cfg.get('task_types', {}).get('general', ['general', 'coding'])

        reasons: list[str] = []
        score = 0.0
        if kind in allowed_kinds:
            idx = allowed_kinds.index(kind)
            score += float(self.cfg.get('kind_match_weight', 100.0)) - idx * 12
            reasons.append(f'{kind} matches {task_type}')
        else:
            return -10000.0, ['incompatible model kind']

        configured = list(getattr(self.models, 'cfg', {}).get(task_type, []))
        if not configured and task_type not in {'general', 'coding', 'vision', 'embedding'}:
            configured = list(getattr(self.models, 'cfg', {}).get('general', []))
        if model in configured:
            idx = configured.index(model)
            score += max(0.0, float(self.cfg.get('configured_priority_weight', 16.0)) - idx * 2.5)
            reasons.append('configured preference')

        perf = performance.get((model, task_type)) or performance.get((model, 'general'))
        if perf:
            total = perf['successes'] + perf['failures']
            if total:
                success_rate = perf['successes'] / total
                confidence = min(1.0, math.log2(total + 1) / 4.0)
                bonus = float(self.cfg.get('performance_weight', 22.0)) * success_rate * confidence
                penalty = float(self.cfg.get('performance_weight', 22.0)) * (1.0 - success_rate) * confidence * 0.7
                score += bonus - penalty
                reasons.append(f'{success_rate:.0%} measured success over {total} run(s)')
                if perf['successes']:
                    avg = perf['total_latency_ms'] / max(1, perf['successes'])
                    target = max(1000.0, float(self.cfg.get('latency_target_ms', 30000)))
                    latency_bonus = float(self.cfg.get('latency_weight', 8.0)) * max(-1.0, min(1.0, 1.0 - avg / target))
                    score += latency_bonus
                    reasons.append(f'avg latency {avg:.0f} ms')
        else:
            reasons.append('no measured history yet')

        return round(score, 3), reasons

    def route(self, text: str, task_type: str | None = 'auto', requested_model: str | None = None) -> dict:
        classification = self.classify(text, task_type)
        status = self.models.list_models()
        if not status.get('online'):
            raise RuntimeError(f'Ollama is not reachable at {self.models.base}. Start Ollama and try again.')

        items = [x for x in status.get('items', []) if isinstance(x, dict) and x.get('name')]
        installed = {x['name']: x for x in items}
        explicit = str(requested_model or '').strip()

        if explicit:
            if explicit not in installed:
                raise RuntimeError(f'The selected Ollama model {explicit!r} is not installed or currently available.')
            if installed[explicit].get('kind') == 'embedding':
                raise RuntimeError('Embedding models cannot be selected for normal chat generation.')
            candidates = [{
                'model': explicit,
                'kind': installed[explicit].get('kind', 'unknown'),
                'score': 1000.0,
                'reasons': ['explicit user model selection'],
            }]
            reason = 'Explicit model selection overrides automatic ranking.'
        else:
            performance = self._performance_rows()
            ranked = []
            allow_cloud = bool(self.cfg.get('allow_cloud_through_ollama_by_default', False))
            for item in items:
                kind = item.get('kind', 'unknown')
                if kind == 'embedding':
                    continue
                if kind == 'cloud-through-ollama' and not allow_cloud:
                    continue
                score, reasons = self._score_model(item, classification['task_type'], performance)
                if score <= -9999:
                    continue
                ranked.append({
                    'model': item['name'], 'kind': kind, 'score': score, 'reasons': reasons,
                })
            ranked.sort(key=lambda x: (-x['score'], x['model']))
            candidates = ranked
            if not candidates:
                raise RuntimeError(
                    f'No installed local Ollama model is compatible with task type {classification["task_type"]!r}.'
                )
            top = candidates[0]
            reason = (
                f"Selected {top['model']} because it ranked highest for {classification['task_type']} "
                f"using compatibility, configured preference and measured local performance."
            )

        decision = BrainDecision(
            decision_id=str(uuid.uuid4()),
            intent=classification['intent'],
            task_type=classification['task_type'],
            complexity=classification['complexity'],
            privacy=classification['privacy'],
            tool_hint=classification['tool_hint'],
            selected_model=candidates[0]['model'] if candidates else None,
            candidates=candidates,
            reason=reason,
            mode=str(self.cfg.get('mode', 'local-first')),
        )
        out = asdict(decision)
        out['prompt_length'] = classification['prompt_length']
        self._emit('BRAIN_ROUTE_DECIDED', {
            'decision_id': decision.decision_id,
            'intent': decision.intent,
            'task_type': decision.task_type,
            'complexity': decision.complexity,
            'privacy': decision.privacy,
            'selected_model': decision.selected_model,
            'candidate_count': len(decision.candidates),
        })
        return out

    def _record_decision(self, decision: dict, text: str, status: str, latency_ms: float, error: str = ''):
        prompt_hash = hashlib.sha256(str(text).encode('utf-8', errors='replace')).hexdigest()
        with transaction(self.db) as c:
            c.execute(
                "INSERT OR REPLACE INTO brain_decisions("
                "id,ts,prompt_hash,prompt_length,intent,task_type,complexity,privacy,tool_hint,"
                "selected_model,reason,status,latency_ms,error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    decision['decision_id'], time.time(), prompt_hash, int(decision.get('prompt_length', len(text))),
                    decision['intent'], decision['task_type'], int(decision['complexity']), decision['privacy'],
                    decision['tool_hint'], decision.get('selected_model') or '', decision.get('reason', ''),
                    status, float(latency_ms), str(error or '')[:2000],
                ),
            )

    def _record_model_outcome(self, model: str, task_type: str, ok: bool, latency_ms: float):
        with transaction(self.db) as c:
            c.execute(
                "INSERT INTO model_performance(model,task_type,successes,failures,total_latency_ms,last_latency_ms,last_status,updated) "
                "VALUES(?,?,?,?,?,?,?,?) "
                "ON CONFLICT(model,task_type) DO UPDATE SET "
                "successes=successes+excluded.successes,failures=failures+excluded.failures,"
                "total_latency_ms=total_latency_ms+excluded.total_latency_ms,"
                "last_latency_ms=excluded.last_latency_ms,last_status=excluded.last_status,updated=excluded.updated",
                (
                    model, task_type, 1 if ok else 0, 0 if ok else 1,
                    float(latency_ms) if ok else 0.0, float(latency_ms),
                    'success' if ok else 'failure', time.time(),
                ),
            )

    def generate(
        self,
        prompt: str,
        task_type: str | None = 'auto',
        model: str | None = None,
        system: str = 'You are Jubi, a local AI orchestrator.',
        timeout: int = 300,
    ) -> dict:
        decision = self.route(prompt, task_type, model)
        max_attempts = 1 if model else max(1, min(int(self.cfg.get('max_fallback_attempts', 3)), 5))
        attempts = decision['candidates'][:max_attempts]
        errors = []
        total_started = time.perf_counter()

        for index, candidate in enumerate(attempts, start=1):
            selected = candidate['model']
            started = time.perf_counter()
            self._emit('BRAIN_MODEL_ATTEMPT', {
                'decision_id': decision['decision_id'], 'model': selected,
                'task_type': decision['task_type'], 'attempt': index,
            })
            try:
                result = self.models.generate(
                    prompt,
                    task_type=decision['task_type'] if decision['task_type'] in {'general', 'coding', 'vision', 'embedding'} else 'general',
                    system=system,
                    model=selected,
                    timeout=timeout,
                )
                elapsed = (time.perf_counter() - started) * 1000.0
                self._record_model_outcome(selected, decision['task_type'], True, elapsed)
                decision['selected_model'] = selected
                decision['attempt'] = index
                decision['latency_ms'] = round(elapsed, 2)
                result = dict(result or {})
                result['jubi_route'] = decision
                result.setdefault('model', selected)
                total = (time.perf_counter() - total_started) * 1000.0
                self._record_decision(decision, prompt, 'success', total)
                self._emit('BRAIN_ROUTE_SUCCEEDED', {
                    'decision_id': decision['decision_id'], 'model': selected,
                    'task_type': decision['task_type'], 'latency_ms': round(elapsed, 2),
                    'attempt': index,
                })
                return result
            except Exception as exc:
                elapsed = (time.perf_counter() - started) * 1000.0
                self._record_model_outcome(selected, decision['task_type'], False, elapsed)
                errors.append({'model': selected, 'error': str(exc), 'latency_ms': round(elapsed, 2)})
                self._emit('BRAIN_MODEL_FAILED', {
                    'decision_id': decision['decision_id'], 'model': selected,
                    'task_type': decision['task_type'], 'attempt': index,
                    'error': str(exc)[:1000],
                })

        total = (time.perf_counter() - total_started) * 1000.0
        joined = '; '.join(f"{x['model']}: {x['error']}" for x in errors)
        self._record_decision(decision, prompt, 'failed', total, joined)
        raise RuntimeError('All compatible local model attempts failed. ' + joined)

    def recent_decisions(self, limit: int = 50) -> list[dict]:
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            rows = c.execute(
                "SELECT id,ts,prompt_hash,prompt_length,intent,task_type,complexity,privacy,tool_hint,"
                "selected_model,reason,status,latency_ms,error FROM brain_decisions ORDER BY ts DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'prompt_hash': r[2], 'prompt_length': r[3],
                'intent': r[4], 'task_type': r[5], 'complexity': r[6], 'privacy': r[7],
                'tool_hint': r[8], 'selected_model': r[9], 'reason': r[10], 'status': r[11],
                'latency_ms': r[12], 'error': r[13],
            }
            for r in rows
        ]

    def performance(self) -> list[dict]:
        rows = list(self._performance_rows().values())
        for item in rows:
            total = item['successes'] + item['failures']
            item['attempts'] = total
            item['success_rate'] = (item['successes'] / total) if total else None
            item['avg_success_latency_ms'] = (
                item['total_latency_ms'] / item['successes'] if item['successes'] else None
            )
        rows.sort(key=lambda x: (-x['successes'], x['failures'], x['model'], x['task_type']))
        return rows

    def status(self) -> dict:
        decisions = self.recent_decisions(30)
        perf = self.performance()
        models = self.models.list_models()
        return {
            'name': 'Jubi Advanced Local Brain',
            'phase': 'Phase 1',
            'mode': self.cfg.get('mode', 'local-first'),
            'automatic_cloud_escalation': bool(self.cfg.get('allow_cloud_through_ollama_by_default', False)),
            'ollama_online': bool(models.get('online')),
            'detected_models': len(models.get('items', [])),
            'tracked_model_task_pairs': len(perf),
            'recorded_decisions': len(decisions),
            'max_fallback_attempts': int(self.cfg.get('max_fallback_attempts', 3)),
            'performance': perf,
            'recent_decisions': decisions,
        }
