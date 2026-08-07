from __future__ import annotations
import os, subprocess, webbrowser
from pathlib import Path

class WindowsBroker:
    def __init__(self, root: Path): self.root = root.resolve()
    def available(self): return os.name == 'nt'
    def _ensure_workspace(self, p):
        path = Path(p).expanduser().resolve()
        if self.root not in path.parents and path != self.root: raise PermissionError('Path is outside SARUS workspace')
        return path
    def action(self, name: str, args: dict | None = None, approved=False):
        args = args or {}
        if name == 'open_url': webbrowser.open(str(args.get('url', ''))); return {'ok': True}
        if name == 'read_file':
            p = self._ensure_workspace(args['path']); return {'ok': True, 'path': str(p), 'content': p.read_text(encoding='utf-8', errors='replace')[:200000]}
        if name == 'write_file':
            p = self._ensure_workspace(args['path']); p.parent.mkdir(parents=True, exist_ok=True); p.write_text(str(args.get('content', '')), encoding='utf-8'); return {'ok': True, 'path': str(p)}
        if os.name != 'nt': return {'ok': False, 'error': 'Windows-only action', 'action': name}
        if name == 'open_app': subprocess.Popen([str(args['path']), *map(str, args.get('argv', []))]); return {'ok': True}
        if name == 'list_processes':
            cp = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True, timeout=15, encoding='utf-8', errors='replace'); return {'ok': cp.returncode == 0, 'output': cp.stdout[:200000]}
        if name == 'list_services':
            cp = subprocess.run(['sc', 'query', 'state=', 'all'], capture_output=True, text=True, timeout=20, encoding='utf-8', errors='replace'); return {'ok': cp.returncode == 0, 'output': cp.stdout[:200000]}
        if name in {'powershell', 'stop_process', 'service_control'} and not approved: raise PermissionError('Explicit approval required')
        if name == 'powershell':
            cp = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', str(args.get('command', ''))], capture_output=True, text=True, timeout=int(args.get('timeout', 120)), encoding='utf-8', errors='replace'); return {'ok': cp.returncode == 0, 'returncode': cp.returncode, 'stdout': cp.stdout[-100000:], 'stderr': cp.stderr[-100000:]}
        if name == 'stop_process':
            cmd = ['taskkill', '/PID', str(int(args['pid'])), '/T'] + (['/F'] if args.get('force') else []); cp = subprocess.run(cmd, capture_output=True, text=True, timeout=15); return {'ok': cp.returncode == 0, 'stdout': cp.stdout, 'stderr': cp.stderr}
        if name == 'service_control':
            action = str(args.get('action','query')).lower()
            if action not in {'start','stop','query'}: raise ValueError('service action must be start, stop or query')
            service = str(args.get('service','')).strip()
            if not service or any(ch in service for ch in '"&|<>'): raise ValueError('invalid service name')
            cp = subprocess.run(['sc', action, service], capture_output=True, text=True, timeout=30, encoding='utf-8', errors='replace'); return {'ok': cp.returncode == 0, 'returncode':cp.returncode, 'stdout': cp.stdout[-100000:], 'stderr': cp.stderr[-100000:]}
        raise ValueError('Unknown action: ' + name)
