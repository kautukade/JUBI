"""Jubi-owned admission/lifecycle around the vendored Hermes runtime.

This first slice is intentionally analysis-only and explicitly experimental.
No second task database: durable task ownership remains reserved for Hermes
Kanban. A subprocess result is evidence, not a task completion transition.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import shutil
from contextlib import closing
from pathlib import Path

from .hardware import admission, memory_snapshot
from .provider_policy import LOCAL_ONLY, InferenceTransport
from sarus.integrations.hermes_compact import (
    ANALYSIS_MIN_CONTEXT,
    ANALYSIS_TARGET_CONTEXT,
    compact_excerpt,
    compact_profile,
)


class HermesRuntime:
    def __init__(self, root: Path, models, python_exe=None):
        self.root = root.resolve()
        self.models = models
        sources = json.loads((root / 'config/sources.json').read_text(encoding='utf-8'))
        self.source = self.root / 'sources' / sources['hermes']
        self.python = str(python_exe or sys.executable)

    def analyze(self, prompt: str, model: str, context='', timeout=180) -> dict:
        LOCAL_ONLY.check_model_name(model)
        status = self.models.list_models()
        item = next((x for x in status.get('items', []) if x['name'] == model), None)
        if not item:
            raise RuntimeError('Selected Hermes model is not installed')
        admitted = admission(item.get('size'), memory_snapshot())
        if not admitted['admitted']:
            return {'status': 'DEPENDENCY_MISSING', 'reason': 'Insufficient known free RAM',
                    'admission': admitted, 'tools_executed': False}
        transport = InferenceTransport(self.models.base)
        metadata = transport.json('/api/show', {'model': model})
        LOCAL_ONLY.check_model(model, metadata)
        profile = compact_profile(metadata, require_tools=False,
                                  min_context=ANALYSIS_MIN_CONTEXT,
                                  target_context=ANALYSIS_TARGET_CONTEXT)
        if not profile['eligible']:
            return {'status': 'DEPENDENCY_MISSING', 'reason': profile['reason'],
                    'compact_profile': profile, 'tools_executed': False}
        if not (self.source / 'run_agent.py').is_file():
            return {'status': 'DEPENDENCY_MISSING', 'reason': 'Hermes runtime source missing'}
        workspace = self.root / 'data/hermes-analysis'
        workspace.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='run-', dir=workspace) as directory:
            home = Path(directory)
            # JSON is valid YAML; no runtime YAML dependency in the Jubi server.
            config = {
                'model': {'default': model, 'provider': 'custom', 'base_url': self.models.base + '/v1',
                          'context_length': profile['runtime_context']},
                'agent': {'api_max_retries': 0},
                'delegation': {'max_concurrent_children': 1, 'max_spawn_depth': 1,
                               'max_iterations': 2, 'child_timeout_seconds': min(timeout, 150),
                               'inherit_mcp_toolsets': False, 'subagent_auto_approve': False},
                'memory': {'memory_enabled': False, 'user_profile_enabled': False},
                'plugins': {'enabled': False},
                'telemetry': {'shared_metrics': {'enabled': False}},
                'context_compression': {'enabled': False},
            }
            (home / 'config.yaml').write_text(json.dumps(config), encoding='utf-8')
            output = home / 'result.json'
            request = {
                'source': str(self.source),
                'base_url': self.models.base,
                'model': model,
                'prompt': compact_excerpt(str(prompt), 6000),
                'context': compact_excerpt(str(context), 6000),
                'output': str(output),
            }
            # No inherited credentials, proxy, source config or plugin search paths.
            env = {k: v for k, v in os.environ.items() if k.upper() in
                   {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATHEXT', 'TEMP', 'TMP', 'PATH'}}
            env.update(HERMES_HOME=str(home), PYTHONPATH=str(self.root), PYTHONUTF8='1',
                       PYTHONDONTWRITEBYTECODE='1',
                       PYTHONIOENCODING='utf-8', PYTHONNOUSERSITE='1',
                       HOME=str(home), USERPROFILE=str(home), LOCALAPPDATA=str(home), APPDATA=str(home))
            try:
                process = subprocess.run([self.python, '-m', 'sarus.integrations.hermes_worker'],
                                         input=json.dumps(request), cwd=home, env=env, text=True,
                                         encoding='utf-8', errors='replace', capture_output=True,
                                         timeout=timeout,
                                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            except subprocess.TimeoutExpired:
                return {'status': 'FAILED', 'reason': 'Hermes analysis process timed out',
                        'tools_executed': False, 'admission': admitted,
                        'compact_profile': profile}
            if process.returncode or not output.is_file():
                return {'status': 'FAILED', 'reason': 'Hermes runtime did not produce valid evidence',
                        'diagnostic': process.stderr[-6000:], 'tools_executed': False,
                        'admission': admitted, 'compact_profile': profile}
            result = json.loads(output.read_text(encoding='utf-8'))
            result['admission'] = admitted
            result['compact_profile'] = profile
            # Preserve actual Hermes sessions before temporary runtime cleanup.
            import sqlite3
            session_ids = []
            with closing(sqlite3.connect(home / 'sessions.db')) as source_db:
                session_ids = [x[0] for x in source_db.execute('SELECT id FROM sessions')]
                archive = workspace / (result['parent_session'] + '.db')
                with closing(sqlite3.connect(archive)) as destination:
                    source_db.backup(destination)
            result['session_ids'] = session_ids
            result['session_archive'] = str(archive.relative_to(self.root))
            # Rewrite every transcript reference to its durable archive.
            log_archive = workspace / (result['parent_session'] + '-logs')
            def archive_paths(value):
                if isinstance(value, dict):
                    return {k: archive_paths(v) for k, v in value.items()}
                if isinstance(value, list):
                    return [archive_paths(v) for v in value]
                if isinstance(value, str) and value.startswith(str(home)):
                    path = Path(value)
                    if path.is_file() and path.is_relative_to(home):
                        destination = log_archive / path.relative_to(home)
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, destination)
                        return str(destination)
                return value
            result = archive_paths(result)
            return result
