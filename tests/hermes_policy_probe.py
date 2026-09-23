"""Subprocess fixture: real Hermes constructors/delegation, no real inference."""
import json
import os
import sys
import subprocess
from pathlib import Path

root, home, source = map(Path, sys.argv[1:4])
sys.path.insert(0, str(root))
sys.path.insert(0, str(source))
from sarus.integrations.hermes_transport import install_transport
from sarus.core.provider_policy import ProviderPolicyError
approved = (sys.executable, '-I', '-c', 'raise SystemExit(7)')
create_client = install_transport(None, 'http://127.0.0.1:9', [], process_commands=(approved,))
import hermes_cli.env_loader as loader
loader.load_hermes_dotenv = lambda *args, **kwargs: []
import run_agent
from tools.delegate_tool import delegate_task
run_agent.AIAgent._create_openai_client = create_client
common = dict(base_url='http://127.0.0.1:9/v1', provider='custom', api_key='local-no-key',
              api_mode='chat_completions', enabled_toolsets=[], skip_memory=True,
              skip_context_files=True, quiet_mode=True, max_iterations=1)
output = {}
output['approved_command_exit'] = subprocess.run(list(approved)).returncode
try:
    subprocess.run([*approved, 'extra-argument'])
except ProviderPolicyError:
    output['modified_command_denied'] = True
try:
    run_agent.AIAgent(model='qwen:cloud', **common)
except RuntimeError as exc:
    # Hermes wraps constructor errors; require the original policy exception.
    output['explicit_worker_cloud_blocked'] = (
        isinstance(exc, ProviderPolicyError) or isinstance(exc.__context__, ProviderPolicyError))
parent = run_agent.AIAgent(model='local:latest', **common)
try:
    child = json.loads(delegate_task(goal='Attempt cloud', parent_agent=parent, max_iterations=1, background=False))
    output['child_result'] = child
except RuntimeError as exc:
    if not isinstance(exc.__context__, ProviderPolicyError):
        raise
    output['child_result'] = {'status': 'blocked', 'error': str(exc)}
parent._delegate_depth = 1
output['nested_result'] = json.loads(delegate_task(goal='Nested attempt', parent_agent=parent, role='orchestrator'))
import urllib.request
try:
    urllib.request.urlopen('https://example.invalid/private', timeout=1)
except Exception as exc:
    output['direct_network_denied'] = 'Jubi inference boundary' in str(exc)
(home/'result.json').write_text(json.dumps(output, indent=2))
