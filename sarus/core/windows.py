from __future__ import annotations

import os
import subprocess
import webbrowser
from pathlib import Path


class WindowsBroker:
    """Low-level executor for already-authorized typed actions.

    This class deliberately has no arbitrary PowerShell/cmd/exec primitive.
    Privileged callers must go through PrivilegedBroker, which resolves logical
    resource IDs before this executor is reached.
    """

    _BLOCKED_LEGACY = {'powershell', 'stop_process', 'service_control', 'open_app'}

    def __init__(self, root: Path):
        self.root = root.resolve()

    def available(self):
        return os.name == 'nt'

    def _ensure_workspace(self, p):
        path = Path(p).expanduser().resolve()
        if self.root not in path.parents and path != self.root:
            raise PermissionError('Path is outside SARUS workspace')
        return path

    @staticmethod
    def _run(argv: list[str], timeout: int):
        cp = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding='utf-8',
            errors='replace',
            shell=False,
        )
        return {
            'ok': cp.returncode == 0,
            'returncode': cp.returncode,
            'stdout': cp.stdout[-100000:],
            'stderr': cp.stderr[-100000:],
        }

    def action(self, name: str, args: dict | None = None, approved=False):
        """Backward-compatible low-risk API.

        Old privileged names are intentionally blocked even when approved=True.
        This prevents an LLM from turning a boolean into arbitrary shell access.
        """
        args = args or {}
        if name in self._BLOCKED_LEGACY:
            raise PermissionError('Legacy privileged action disabled; use the typed PrivilegedBroker API')
        mapping = {
            'open_url': 'url.open',
            'read_file': 'workspace.file.read',
            'write_file': 'workspace.file.write',
            'list_processes': 'system.processes.list',
            'list_services': 'system.services.list',
        }
        action_id = mapping.get(name)
        if not action_id:
            raise ValueError('Unknown action: ' + name)
        return self.execute_typed(action_id, args, {})

    def execute_typed(self, action_id: str, parameters: dict | None = None, resolved: dict | None = None):
        parameters = parameters or {}
        resolved = resolved or {}

        if action_id == 'url.open':
            url = str(parameters.get('url', ''))
            if not (url.startswith('http://') or url.startswith('https://')):
                raise ValueError('only http/https URLs are allowed')
            return {'ok': bool(webbrowser.open(url)), 'url': url}

        if action_id == 'workspace.file.read':
            p = self._ensure_workspace(parameters['path'])
            return {
                'ok': True,
                'path': str(p),
                'content': p.read_text(encoding='utf-8', errors='replace')[:200000],
            }

        if action_id == 'workspace.file.write':
            p = self._ensure_workspace(parameters['path'])
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(str(parameters.get('content', '')), encoding='utf-8')
            return {'ok': True, 'path': str(p)}

        if os.name != 'nt':
            return {'ok': False, 'error': 'Windows-only action', 'action': action_id}

        if action_id == 'system.processes.list':
            return self._run(['tasklist', '/FO', 'CSV', '/NH'], 15)

        if action_id == 'system.services.list':
            return self._run(['sc.exe', 'query', 'state=', 'all'], 20)

        if action_id in {'service.query', 'service.start', 'service.stop'}:
            service = str(resolved.get('service_name', '')).strip()
            if not service or any(ch in service for ch in '"&|<>\r\n'):
                raise ValueError('invalid allowlisted service mapping')
            verb = action_id.split('.', 1)[1]
            return self._run(['sc.exe', verb, service], 30)

        if action_id == 'process.stop':
            image = str(resolved.get('image_name', '')).strip()
            if not image or any(ch in image for ch in '"&|<>/\\\r\n'):
                raise ValueError('invalid allowlisted process mapping')
            argv = ['taskkill.exe', '/IM', image, '/T']
            if parameters.get('force'):
                argv.append('/F')
            return self._run(argv, 15)

        # There is intentionally no generic executable, shell, raw driver,
        # IOCTL, kernel-memory, registry-anywhere, or arbitrary service path.
        raise PermissionError('Typed Windows action is not implemented: ' + action_id)
