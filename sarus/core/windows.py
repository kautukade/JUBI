from __future__ import annotations

import json
import os
import shutil
import subprocess
import webbrowser
from pathlib import Path

from sarus.core.ring0 import Ring0Bridge


class WindowsBroker:
    """Low-level executor for already-authorized typed actions.

    There is deliberately no arbitrary shell/PowerShell/cmd primitive. Workspace
    operations are confined to configured roots, apps/services/processes are
    resource allowlisted, and Ring-0 remains limited to compiled fixed methods.
    """

    _BLOCKED_LEGACY = {'powershell', 'stop_process', 'service_control', 'open_app'}

    def __init__(self, root: Path):
        self.root = root.resolve()
        self.file_roots = self._load_file_roots()
        self.ring0 = Ring0Bridge()

    def _load_file_roots(self) -> tuple[Path, ...]:
        cfg_path = self.root / 'config' / 'broker_allowlist.json'
        try:
            cfg = json.loads(cfg_path.read_text(encoding='utf-8'))
            roots = cfg.get('path_scopes', {}).get('user_workspace', [])
        except (OSError, ValueError, TypeError):
            roots = []
        return tuple((self.root / str(p)).resolve() for p in roots if str(p).strip())

    def available(self):
        return os.name == 'nt'

    def _ensure_workspace(self, p):
        path = Path(p).expanduser()
        if not path.is_absolute():
            path = self.root / path
        path = path.resolve()
        if not self.file_roots:
            raise PermissionError('No Jubi broker workspace roots are configured')
        if not any(path == base or base in path.parents for base in self.file_roots):
            raise PermissionError('Path is outside approved Jubi broker workspaces')
        return path

    @staticmethod
    def _run(argv: list[str], timeout: int, cwd: Path | None = None):
        cp = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding='utf-8',
            errors='replace',
            shell=False,
            cwd=str(cwd) if cwd else None,
        )
        return {
            'ok': cp.returncode == 0,
            'returncode': cp.returncode,
            'stdout': cp.stdout[-100000:],
            'stderr': cp.stderr[-100000:],
        }

    def action(self, name: str, args: dict | None = None, approved=False):
        args = args or {}
        if name in self._BLOCKED_LEGACY:
            raise PermissionError('Legacy privileged action disabled; use the typed PrivilegedBroker API')
        mapping = {
            'open_url': 'url.open',
            'read_file': 'workspace.file.read',
            'write_file': 'workspace.file.write',
            'list_processes': 'system.processes.list',
            'list_services': 'system.services.list',
            'ring0_ping': 'ring0.ping',
            'ring0_status': 'ring0.status',
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
            if not p.is_file():
                raise FileNotFoundError(str(p))
            return {
                'ok': True,
                'path': str(p),
                'content': p.read_text(encoding='utf-8', errors='replace')[:200000],
            }

        if action_id == 'workspace.file.write':
            p = self._ensure_workspace(parameters['path'])
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(str(parameters.get('content', '')), encoding='utf-8')
            return {'ok': True, 'path': str(p), 'bytes': p.stat().st_size}

        if action_id == 'workspace.path.stat':
            p = self._ensure_workspace(parameters['path'])
            if not p.exists():
                return {'ok': True, 'path': str(p), 'exists': False}
            st = p.stat()
            return {
                'ok': True, 'path': str(p), 'exists': True, 'is_file': p.is_file(),
                'is_dir': p.is_dir(), 'size': st.st_size, 'modified': st.st_mtime,
            }

        if action_id == 'workspace.directory.list':
            p = self._ensure_workspace(parameters['path'])
            if not p.is_dir():
                raise NotADirectoryError(str(p))
            items = []
            for child in sorted(p.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower()))[:500]:
                try:
                    st = child.stat()
                    items.append(
                        {
                            'name': child.name,
                            'path': str(child),
                            'is_dir': child.is_dir(),
                            'size': st.st_size if child.is_file() else None,
                            'modified': st.st_mtime,
                        }
                    )
                except OSError:
                    continue
            return {'ok': True, 'path': str(p), 'items': items, 'truncated': len(items) >= 500}

        if action_id == 'workspace.directory.create':
            p = self._ensure_workspace(parameters['path'])
            p.mkdir(parents=bool(parameters.get('parents', True)), exist_ok=True)
            return {'ok': True, 'path': str(p)}

        if action_id in {'workspace.file.copy', 'workspace.file.move'}:
            src = self._ensure_workspace(parameters['source_path'])
            dst = self._ensure_workspace(parameters['destination_path'])
            if not src.is_file():
                raise FileNotFoundError(str(src))
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists() and not bool(parameters.get('overwrite', False)):
                raise FileExistsError(str(dst))
            if action_id.endswith('.copy'):
                shutil.copy2(src, dst)
            else:
                if dst.exists():
                    dst.unlink()
                shutil.move(str(src), str(dst))
            return {'ok': True, 'source_path': str(src), 'destination_path': str(dst)}

        if action_id == 'workspace.file.delete':
            p = self._ensure_workspace(parameters['path'])
            if not p.is_file():
                raise FileNotFoundError(str(p))
            size = p.stat().st_size
            p.unlink()
            return {'ok': True, 'path': str(p), 'deleted_bytes': size}

        if action_id in {'development.git.status', 'development.git.log'}:
            p = self._ensure_workspace(parameters['path'])
            if not p.is_dir():
                raise NotADirectoryError(str(p))
            if action_id.endswith('.status'):
                return self._run(['git', 'status', '--short', '--branch'], 20, cwd=p)
            limit = max(1, min(int(parameters.get('limit', 20)), 100))
            return self._run(['git', 'log', f'-{limit}', '--oneline', '--decorate'], 20, cwd=p)

        if action_id == 'ring0.ping':
            return self.ring0.ping()

        if action_id == 'ring0.status':
            return self.ring0.status()

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

        if action_id == 'app.launch':
            argv = resolved.get('argv')
            if not isinstance(argv, list) or not argv or not all(isinstance(x, str) and x for x in argv):
                raise ValueError('invalid allowlisted app mapping')
            command = list(argv)
            workspace_path = str(parameters.get('workspace_path') or '').strip()
            if workspace_path:
                command.append(str(self._ensure_workspace(workspace_path)))
            proc = subprocess.Popen(command, shell=False, cwd=str(self.root))
            return {'ok': True, 'pid': proc.pid, 'resource_id': resolved.get('resource_id')}

        raise PermissionError('Typed Windows action is not implemented: ' + action_id)
