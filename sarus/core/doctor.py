from __future__ import annotations

import json
import os
import platform
import shutil
import sys


class Doctor:
    def __init__(self, app):
        self.app = app

    def _production(self) -> dict:
        path = self.app.root / 'config' / 'production.json'
        try:
            return json.loads(path.read_text(encoding='utf-8'))
        except Exception:
            return {}

    @staticmethod
    def _minimum_python(value: str) -> tuple[int, int]:
        try:
            parts = str(value).split('.')
            return int(parts[0]), int(parts[1])
        except Exception:
            return 3, 11

    def run(self):
        prod = self._production()
        required = list(prod.get('required_models', []))
        minimum_python = self._minimum_python(prod.get('minimum_python', '3.11'))
        models = self.app.models.list_models()
        installed = set(models.get('models', []))
        checks = []

        def add(name, ok, detail='', level='required'):
            checks.append({'name': name, 'ok': bool(ok), 'detail': str(detail), 'level': level})

        add(
            f'Python >= {minimum_python[0]}.{minimum_python[1]}',
            sys.version_info >= minimum_python,
            sys.version.split()[0],
        )
        add('Writable data directory', os.access(self.app.root / 'data', os.W_OK), self.app.root / 'data')
        add('Ollama service', models.get('online', False), models.get('error', 'online'))
        local_chat = [item['name'] for item in models.get('items', [])
                      if item.get('kind') in {'general', 'coding'}]
        add('Installed local chat candidate', bool(local_chat), ', '.join(local_chat) or 'User-approved model setup required')
        for model in required:
            add('Ollama model ' + model, model in installed, 'installed' if model in installed else 'missing')

        for cmd, level in [
            ('git', 'recommended'),
            ('node', 'recommended'),
            ('npm', 'recommended'),
            ('powershell', 'windows'),
            ('ffmpeg', 'optional'),
            ('docker', 'optional'),
            ('qemu-system-x86_64', 'optional'),
        ]:
            p = shutil.which(cmd)
            add('Command ' + cmd, bool(p), p or 'not found', level)

        for adapter in self.app.adapters.connect():
            add('Source ' + adapter.name, adapter.connected, adapter.path)
        for name, state in self.app.native.status().items():
            add(
                'Native runtime ' + name,
                state.get('ready', False),
                state.get('mode', ''),
                'recommended' if name in {'sara', 'hermes', 'ecc'} else 'optional',
            )

        profiles = prod.get('deployment_profiles', {})
        default_profile = 'windows_desktop' if os.name == 'nt' else (
            'linux_vps' if platform.system() == 'Linux' else 'development'
        )
        deployment_profile = os.environ.get('JUBI_DEPLOYMENT_PROFILE', default_profile).strip() or default_profile
        deployment = profiles.get(deployment_profile, {})
        if deployment_profile in profiles:
            add(
                'Deployment profile ' + deployment_profile,
                deployment.get('supported') is True,
                deployment.get('scope') or deployment.get('ui') or '',
                'required',
            )

        if deployment_profile == 'windows_desktop':
            add('Windows platform', os.name == 'nt', platform.platform(), 'required')
        elif deployment_profile == 'linux_vps':
            add('Linux VPS platform', platform.system() == 'Linux', platform.platform(), 'required')
            hermes_runtime = getattr(self.app, 'hermes', None)
            hermes = hermes_runtime.status() if hermes_runtime is not None else {
                'ready': False, 'reason': 'Hermes runtime not attached to this app fixture'
            }
            require_hermes = os.environ.get('JUBI_REQUIRE_HERMES', '0').lower() in {'1', 'true', 'yes', 'on'}
            add(
                'Hermes pilot dependencies',
                hermes.get('ready', False),
                json.dumps(hermes, sort_keys=True),
                'required' if require_hermes else 'recommended',
            )
            browser = self.app.browser.status() if getattr(self.app, 'browser', None) else {
                'ready': False, 'reason': 'Browser runtime not attached'
            }
            require_browser = os.environ.get('JUBI_REQUIRE_BROWSER', '0').lower() in {'1', 'true', 'yes', 'on'}
            add(
                'VPS browser runtime',
                browser.get('ready', False),
                json.dumps(browser, sort_keys=True),
                'required' if require_browser else 'recommended',
            )
        else:
            add('Development platform', True, platform.platform(), 'optional')

        core = [c for c in checks if c['level'] == 'required']
        return {
            'name': 'Jubi Doctor',
            'core_ready': all(c['ok'] for c in core),
            'checks': checks,
            'models': models,
            'required_models': required,
            'minimum_python': '.'.join(map(str, minimum_python)),
            'deployment_profile': deployment_profile,
            'deployment': deployment,
        }
