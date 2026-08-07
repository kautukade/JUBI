from __future__ import annotations
import os, platform, shutil, sys
class Doctor:
    def __init__(self, app): self.app = app
    def run(self):
        models = self.app.models.list_models(); installed = set(models.get('models', [])); required = ['qwen2.5:7b', 'qwen2.5-coder:7b', 'qwen2.5vl:3b', 'nomic-embed-text-v2-moe:latest']; checks = []
        def add(name, ok, detail='', level='required'): checks.append({'name': name, 'ok': bool(ok), 'detail': str(detail), 'level': level})
        add('Python >= 3.11', sys.version_info >= (3, 11), sys.version.split()[0]); add('Writable data directory', os.access(self.app.root / 'data', os.W_OK), self.app.root / 'data'); add('Ollama service', models.get('online', False), models.get('error', 'online'))
        for m in required: add('Ollama model ' + m, m in installed, 'installed' if m in installed else 'missing')
        for cmd, level in [('git', 'recommended'), ('node', 'recommended'), ('npm', 'recommended'), ('powershell', 'windows'), ('ffmpeg', 'optional'), ('docker', 'optional'), ('qemu-system-x86_64', 'optional')]:
            p = shutil.which(cmd); add('Command ' + cmd, bool(p), p or 'not found', level)
        for a in self.app.adapters.connect(): add('Source ' + a.name, a.connected, a.path)
        for name,st in self.app.native.status().items(): add('Native runtime ' + name, st.get('ready',False), st.get('mode',''), 'recommended' if name in {'sara','hermes','ecc'} else 'optional')
        if os.name == 'nt': add('Windows platform', True, platform.platform(), 'required')
        else: add('Windows platform', False, platform.platform(), 'target-only')
        core = [c for c in checks if c['level'] == 'required']; return {'name': 'SARUS Doctor', 'core_ready': all(c['ok'] for c in core), 'checks': checks, 'models': models}
