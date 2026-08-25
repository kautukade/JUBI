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

        if os.name == 'nt':
            add('Windows platform', True, platform.platform(), 'required')
        else:
            add('Windows platform', False, platform.platform(), 'target-only')

        core = [c for c in checks if c['level'] == 'required']
        return {
            'name': 'Jubi Doctor',
            'core_ready': all(c['ok'] for c in core),
            'checks': checks,
            'models': models,
            'required_models': required,
            'minimum_python': '.'.join(map(str, minimum_python)),
        }
