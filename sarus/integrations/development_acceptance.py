"""Real, deliberately narrow M1 coding acceptance; invoked by its controller.

Hermes owns child lifecycles, sessions and Kanban. Jubi owns the only workspace
capability, inference policy, resource admission, independent checks and memory.
The arithmetic restriction makes this disposable experiment safe to execute;
it does not certify arbitrary repository execution or the legacy task engine.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import time
import sqlite3
from contextlib import closing
from pathlib import Path

from sarus.core.brain import BrainRouter
from sarus.core.capabilities import CapabilityRegistry, CapabilitySpec
from sarus.core.experience import ExperienceEngine
from sarus.core.hardware import admission, memory_snapshot
from sarus.core.models import OllamaRouter
from sarus.core.provider_policy import LOCAL_ONLY, InferenceTransport
from sarus.integrations.acceptance_workspace import AcceptanceWorkspace
from sarus.integrations.hermes_transport import install_transport


def run(request):
    root, home = Path(request['root']), Path(request['home'])
    project = home / 'project'
    events, receipts = [], []
    cancelled = lambda: (home / 'CANCEL').exists()
    workspace = AcceptanceWorkspace(project, home / 'run_tests.py', events, cancelled)
    models = OllamaRouter(root / 'config/models.json', request['endpoint'])
    brain = BrainRouter(home / 'jubi-evidence.db', models, root / 'config/brain.json')
    decision = brain.route(request['goal'], 'coding')
    selected, resources, context_capacity = None, None, None
    compatibility = []
    for candidate in decision['candidates']:
        item = next(x for x in models.list_models()['items'] if x['name'] == candidate['model'])
        metadata = InferenceTransport(models.base).json('/api/show', {'model': item['name']})
        LOCAL_ONLY.check_model(item['name'], metadata)
        capacity = max([int(v) for k, v in metadata.get('model_info', {}).items()
                        if k.endswith('.context_length') and isinstance(v, int)] or [0])
        if 'tools' not in metadata.get('capabilities', []) or capacity < 64000:
            compatibility.append({'model': item['name'], 'eligible': False,
                                  'reason': 'Hermes requires native tools and at least 64K model context capacity'})
            continue
        resources = admission(item.get('size'), memory_snapshot(), allow_cpu_paging=True)
        if resources['admitted']:
            selected = item['name']
            context_capacity = capacity
            break
    if not selected:
        return {'status': 'FAILED', 'reason': 'No installed tool-capable local model passed memory admission',
                'brain_decision': decision, 'admission': resources}
    (home / 'config.yaml').write_text(json.dumps({
        'model': {'default': selected, 'provider': 'custom', 'base_url': models.base + '/v1', 'context_length': context_capacity},
        'agent': {'api_max_retries': 1, 'enforce_tool_use': True},
        'delegation': {'max_concurrent_children': 1, 'max_spawn_depth': 1, 'max_iterations': 8,
                       'child_timeout_seconds': 420, 'inherit_mcp_toolsets': False, 'subagent_auto_approve': False},
        'memory': {'memory_enabled': False, 'user_profile_enabled': False},
        'plugins': {'enabled': False}, 'tools': {'tool_search': {'enabled': 'off'}},
        'telemetry': {'shared_metrics': {'enabled': False}},
        'context_compression': {'enabled': False}}), encoding='utf-8')
    source_cfg = json.loads((root / 'config/sources.json').read_text(encoding='utf-8'))
    source = root / 'sources' / source_cfg['hermes']
    if (source / '.env').exists():
        raise RuntimeError('Refusing a source .env in the managed Hermes runtime')
    sys.path.insert(0, str(source))
    create_client = install_transport(None, models.base, receipts, max_tokens=768,
                                      process_commands=workspace.commands, max_calls=12,
                                      cancel_check=cancelled, context_tokens=4096, request_timeout=240)
    import hermes_cli.env_loader as env_loader
    env_loader.load_hermes_dotenv = lambda *args, **kwargs: []
    import run_agent
    import model_tools
    from tools.registry import registry as hermes_registry
    from tools.delegate_tool import delegate_task
    from hermes_state import SessionDB
    from hermes_cli import kanban_db
    run_agent.AIAgent._create_openai_client = create_client

    (home / 'empty-sources.json').write_text('{}')
    registry = CapabilityRegistry(home, home / 'empty-sources.json', home / 'source-index.json')
    schema = {'type': 'object', 'required': ['operation'], 'additionalProperties': False, 'properties': {
        'operation': {'type': 'string', 'enum': ['read', 'write', 'test', 'diff']},
        'path': {'type': 'string', 'enum': ['pricing.py', 'test_pricing.py', 'README.md']},
        'content': {'type': 'string', 'maxLength': 4000}}}
    registry.register_executor(CapabilitySpec(
        id='workspace.acceptance', name='Approved disposable project', source='jubi', version='1', category='coding',
        description='Inspect files, edit only pricing.py, run fixed tests, inspect diff.', platforms=('Windows',),
        dependencies=(), permissions=('disposable-project.read', 'pure-arithmetic.write', 'fixed-tests.run'),
        privacy='local_only', risk=1, input_schema=schema, output_schema={'type': 'object'},
        health_check='Protected file hashes and AST constraints', executor='AcceptanceWorkspace.execute',
        timeout_seconds=8, resource_requirements={'max_code_bytes': 4000},
        isolation='Arithmetic AST allowlist; fixed commands; no arbitrary Python or shell',
        availability='EXPERIMENTAL', verification='Unmodified tests plus independent acceptance cases'), workspace.execute)
    hermes_registry.register('jubi_workspace', 'jubi_workspace',
        {'name': 'jubi_workspace', 'description': 'Real approved workspace operations. Read README.md, pricing.py and test_pricing.py; test; write full pricing.py; test; diff.', 'parameters': schema},
        handler=lambda args, **kwargs: registry.execute('workspace.acceptance', args))
    native_dispatch = hermes_registry.dispatch
    def dispatch(name, args, **kwargs):
        if name != 'jubi_workspace':
            return json.dumps({'error': 'Tool is not approved for this task'})
        return native_dispatch(name, args, **kwargs)
    hermes_registry.dispatch = dispatch
    native_call = model_tools.handle_function_call
    def call(function_name, arguments, *args, **kwargs):
        if function_name != 'jubi_workspace':
            return json.dumps({'error': 'Tool is not approved for this task'})
        return native_call(function_name, arguments, *args, **kwargs)
    model_tools.handle_function_call = call
    run_agent.handle_function_call = call

    sessions = SessionDB(home / 'sessions.db')
    common = dict(base_url=models.base + '/v1', api_key='local-no-key', provider='custom',
                  api_mode='chat_completions', model=selected, enabled_toolsets=['jubi_workspace'],
                  max_iterations=8, max_tokens=768, quiet_mode=True, skip_context_files=True,
                  skip_memory=True, session_db=sessions, fallback_model=None)
    parent = run_agent.AIAgent(**common)
    if {tool['function']['name'] for tool in parent.tools} != {'jubi_workspace'}:
        raise RuntimeError('Unexpected tool exposure in acceptance worker: ' + str([t['function']['name'] for t in parent.tools]))
    sessions.create_session(parent.session_id, 'jubi-development', model=selected)
    board = kanban_db.connect(home / 'kanban.db')
    task_id = kanban_db.create_task(board, title=request['goal'], workspace_kind='dir',
                                    workspace_path=str(project), max_retries=0,
                                    max_runtime_seconds=600, model_override=selected, provider_override='custom')
    task = kanban_db.claim_task(board, task_id, claimer='jubi-acceptance', ttl_seconds=620)
    if not task:
        raise RuntimeError('Hermes Kanban claim failed')
    result = {'status': 'FAILED', 'task_id': task_id, 'parent_session': parent.session_id,
              'model': selected, 'brain_decision': decision, 'admission': resources,
              'provider_compatibility': compatibility, 'model_context_capacity': context_capacity,
              'runtime_context_limit': 4096,
              'limits': {'max_children': 2, 'depth': 1, 'concurrency': 1, 'child_timeout_seconds': 420,
                         'overall_timeout_seconds': 600, 'provider_retries': 0, 'max_inference_calls': 12,
                         'max_worker_iterations': 8, 'cancellation': 'CANCEL sentinel + controller process termination'},
              'events': events, 'inference_receipts': receipts, 'policy_revision': LOCAL_ONLY.revision}
    (home / 'evidence.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    try:
        guidance = 'Canonical development loop: inspect the specification and files; reproduce a failing test; make the smallest fix; run tests; review the diff; require fresh independent verification.'
        # Progressive loading: one bounded workflow, never the whole catalogue.
        skill = root / 'sources' / source_cfg['superpowers'] / 'skills/verification-before-completion/SKILL.md'
        ecc = root / 'sources' / source_cfg['ecc'] / 'agents/code-reviewer.md'
        result['workflow_sources'] = []
        for path in (skill, ecc):
            text = path.read_text(encoding='utf-8')
            result['workflow_sources'].append({'path': str(path.relative_to(root)), 'sha256': hashlib.sha256(text.encode()).hexdigest()})
        result['coding'] = json.loads(delegate_task(
            goal=request['goal'] + '\nUse jubi_workspace operations to do the work. Start by reading README.md, pricing.py, test_pricing.py and running test. Do not stop at a plan. Only pricing.py may change. End after test passes and diff is inspected.',
            context=guidance + '\n' + skill.read_text(encoding='utf-8'), role='leaf', max_iterations=8, background=False, parent_agent=parent))
        if any(row.get('status') != 'completed' for row in result['coding'].get('results', [])):
            # A timed-out Hermes thread must not retain write permission or
            # overlap a reviewer. The controller job contains the entire run.
            workspace.phase = 'blocked'
            raise RuntimeError('Coding child did not finish; further delegation denied')
        workspace.phase = 'review'
        diff = workspace.execute('diff')['diff']
        result['diff'] = diff
        review_context = guidance + '\n' + ecc.read_text(encoding='utf-8') + '\nDiff to review:\n' + diff
        if diff:
            result['review'] = json.loads(delegate_task(
                goal='Review the actual diff and project requirements using read and diff operations only. Check that tests were not weakened. Return only JSON {"approved": true or false, "reason": "..."}. Do not edit files.',
                context=review_context, role='leaf', max_iterations=3, background=False, parent_agent=parent))
        else:
            result['review'] = {'status': 'NOT_RUN', 'reason': 'No actual change to review; no extra child spawned'}
        summary = (result['review'].get('results') or [{}])[0].get('summary', '')
        try:
            review = json.loads(re.sub(r'^```(?:json)?\s*|\s*```$', '', summary.strip()))
        except ValueError:
            review = {'approved': False, 'reason': 'Reviewer did not provide a valid verdict'}
        result['reviewer_outcome'] = review
        workspace.phase = 'verifying'
        verification = workspace.execute('verify')
        result['verifier_outcome'] = verification
        coding_events = [e for e in events if e['phase'] == 'coding']
        tests = [e['result']['exit_code'] for e in coding_events if e['operation'] == 'test' and 'result' in e]
        wrote = any(e['operation'] == 'write' and 'result' in e for e in coding_events)
        passed = (tests and tests[0] != 0 and tests[-1] == 0 and wrote and bool(diff)
                  and review.get('approved') is True and verification['exit_code'] == 0
                  and not cancelled())
        result['acceptance_passed'] = bool(passed)
        result['changed_files'] = ['pricing.py'] if diff else []
        if passed:
            if not kanban_db.complete_task(board, task_id, expected_run_id=task.current_run_id,
                                           result='Verified disposable coding acceptance', metadata={'policy': LOCAL_ONLY.revision}):
                raise RuntimeError('Kanban completion was not accepted')
            result['status'] = 'PASS'
        else:
            kanban_db.block_task(board, task_id, reason='Acceptance criteria did not all pass',
                                 expected_run_id=task.current_run_id)
        result['kanban_status'] = kanban_db.get_task(board, task_id).status
        # Persist provenance even if local embeddings are unavailable.
        memory = ExperienceEngine(home / 'jubi-evidence.db', None)
        result['experience'] = memory.record(request['goal'], json.dumps({'task': task_id, 'status': result['status']}),
            bool(passed), task_type='coding', provider='ollama', model=selected, tool='workspace.acceptance',
            metadata={'task_id': task_id, 'policy_revision': LOCAL_ONLY.revision, 'verification_exit': verification['exit_code']})
        with closing(sqlite3.connect(home / 'sessions.db')) as connection:
            result['session_rows'] = [dict(zip(['id', 'parent_session_id'], row)) for row in
                                     connection.execute('SELECT id,parent_session_id FROM sessions')]
        result['network_evidence'] = {'boundary': 'All Python socket connects outside guarded inference denied',
                                      'remote_provider_calls': 0, 'endpoint': models.base,
                                      'scope': 'Instrumented worker process; not a system-wide packet capture'}
    except Exception as exc:
        result['error'] = str(exc)
        kanban_db.block_task(board, task_id, reason=str(exc)[:500], expected_run_id=task.current_run_id)
    finally:
        workspace.phase = 'closed'
        result['kanban_status'] = kanban_db.get_task(board, task_id).status
        result['changed_files'] = ['pricing.py'] if (project / 'pricing.py').read_text(encoding='utf-8') != workspace.baseline else []
        if 'experience' not in result:
            result['experience'] = ExperienceEngine(home / 'jubi-evidence.db', None).record(
                request['goal'], result.get('error', 'Acceptance failed'), False, task_type='coding',
                provider='ollama', model=selected, tool='workspace.acceptance',
                metadata={'task_id': task_id, 'policy_revision': LOCAL_ONLY.revision})
        with closing(sqlite3.connect(home / 'sessions.db')) as connection:
            result['session_rows'] = [dict(zip(['id', 'parent_session_id'], row)) for row in
                                     connection.execute('SELECT id,parent_session_id FROM sessions')]
        result['network_evidence'] = {'boundary': 'Python socket connects outside guarded inference denied',
                                      'remote_provider_calls': 0, 'endpoint': models.base,
                                      'scope': 'Instrumented worker process; not a system-wide packet capture'}
        sessions.close()
        board.close()
    return result


if __name__ == '__main__':
    request = json.load(sys.stdin)
    result = run(request)
    Path(request['home'], 'evidence.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'evidence': str(Path(request['home'], 'evidence.json'))}))
