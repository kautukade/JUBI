"""Internal analysis-only Hermes subprocess. No arbitrary code/tools enabled.

Jubi launches this module with a dedicated HERMES_HOME and sanitized environment.
The adapter uses real AIAgent, delegate_task and SessionDB. General software
execution stays unavailable until OS containment and verified Kanban completion
are integrated; an analysis result is never a completed coding task.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main():
    request = json.load(sys.stdin)
    source = Path(request['source']).resolve()
    home = Path(os.environ['HERMES_HOME']).resolve()
    if not home.is_dir() or (source / '.env').exists():
        raise RuntimeError('Hermes requires a private prepared home and no source .env')
    sys.path.insert(0, str(source))
    from sarus.integrations.hermes_transport import install_transport
    receipts = []
    create_client = install_transport(None, request['base_url'], receipts)
    # Refuse inherited secret hydration before importing the source runtime.
    import hermes_cli.env_loader as env_loader
    env_loader.load_hermes_dotenv = lambda *a, **k: []
    import run_agent
    run_agent.AIAgent._create_openai_client = create_client
    from hermes_state import SessionDB
    from tools.delegate_tool import delegate_task
    from tools.registry import registry

    # Enforce at dispatch as well as schema. Forged model tool calls cannot
    # reach native terminal, browser, Kanban, plugins or arbitrary source tools.
    def deny_tool(*args, **kwargs):
        return json.dumps({'error': 'Executable tools are disabled in the Jubi analysis pilot'})
    registry.dispatch = deny_tool
    run_agent.handle_function_call = deny_tool
    import model_tools
    model_tools.handle_function_call = deny_tool
    db = SessionDB(home / 'sessions.db')
    common = dict(base_url=request['base_url'] + '/v1', api_key='local-no-key',
                  provider='custom', api_mode='chat_completions', model=request['model'],
                  max_iterations=2, max_tokens=512, quiet_mode=True,
                  enabled_toolsets=[], skip_context_files=True, skip_memory=True,
                  session_db=db, fallback_model=None,
                  ephemeral_system_prompt='Analyze only the supplied evidence. Do not claim to edit files or execute tests.')
    parent = run_agent.AIAgent(**common)
    if parent.tools:
        raise RuntimeError('Hermes unexpectedly enabled tools in the analysis pilot')
    db.create_session(parent.session_id, 'jubi-analysis', model=request['model'])
    result = json.loads(delegate_task(goal=request['prompt'], context=request.get('context', ''),
                                      role='leaf', max_iterations=2, background=False, parent_agent=parent))
    output = {'status': 'EXPERIMENTAL', 'tools_executed': False, 'parent_session': parent.session_id,
              'delegation': result, 'inference_receipts': receipts,
              'policy_revision': 'local-only-v1'}
    Path(request['output']).write_text(json.dumps(output, indent=2), encoding='utf-8')
    db.close()


if __name__ == '__main__':
    main()
