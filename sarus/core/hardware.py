"""Read-only, bounded hardware discovery. Unknown is not a successful probe."""
from __future__ import annotations

import ctypes
import json
import os
import platform
import shutil
import subprocess
import struct
import time
from pathlib import Path

GIB = 1024 ** 3


def _physical_cores() -> int | None:
    if os.name != 'nt':
        # Linux VPS installations should not require psutil just to admit a
        # local model. Prefer kernel-provided topology and use psutil only as a
        # secondary optional source.
        try:
            blocks = Path('/proc/cpuinfo').read_text(encoding='utf-8', errors='replace').split('\n\n')
            pairs = set()
            for block in blocks:
                fields = {}
                for line in block.splitlines():
                    if ':' in line:
                        key, value = line.split(':', 1)
                        fields[key.strip()] = value.strip()
                if 'physical id' in fields and 'core id' in fields:
                    pairs.add((fields['physical id'], fields['core id']))
            if pairs:
                return len(pairs)
        except OSError:
            pass
        try:
            import psutil
            return psutil.cpu_count(logical=False)
        except (ImportError, OSError):
            return None
    try:
        api = ctypes.WinDLL('kernel32', use_last_error=True).GetLogicalProcessorInformationEx
        size = ctypes.c_ulong(0)
        api(0, None, ctypes.byref(size))  # RelationProcessorCore
        if not 8 <= size.value <= 4 * 1024 * 1024:
            return None
        buffer = ctypes.create_string_buffer(size.value)
        if not api(0, buffer, ctypes.byref(size)):
            return None
        offset, count = 0, 0
        while offset + 8 <= size.value:
            relation, length = struct.unpack_from('<II', buffer.raw, offset)
            if length < 8 or offset + length > size.value:
                return None
            count += int(relation == 0)
            offset += length
        return count or None
    except (OSError, AttributeError):
        return None


def _windows_registry() -> dict:
    """Independent read-only fallbacks; registry presence isn't device health."""
    import winreg
    result = {'cpu': {}, 'gpu': [], 'devices': [], 'os': {}, 'errors': []}

    def values(path):
        data = {}
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0, winreg.KEY_READ) as key:
            for index in range(winreg.QueryInfoKey(key)[1]):
                name, value, _ = winreg.EnumValue(key, index)
                data[name] = value
        return data

    def children(path):
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0, winreg.KEY_READ) as key:
            return [winreg.EnumKey(key, i) for i in range(min(winreg.QueryInfoKey(key)[0], 512))]

    try:
        cpu = values(r'HARDWARE\DESCRIPTION\System\CentralProcessor\0')
        result['cpu'] = {'name': cpu.get('ProcessorNameString', '').strip(),
                         'vendor': cpu.get('VendorIdentifier'), 'source': 'Windows registry'}
    except OSError as exc:
        result['errors'].append({'probe': 'registry.cpu', 'error': str(exc)})
    try:
        version = values(r'SOFTWARE\Microsoft\Windows NT\CurrentVersion')
        result['os'] = {'build': str(version.get('CurrentBuildNumber', '')),
                        'update_build_revision': version.get('UBR'),
                        'display_version': version.get('DisplayVersion'), 'source': 'Windows registry'}
    except OSError as exc:
        result['errors'].append({'probe': 'registry.windows', 'error': str(exc)})
    gpu_key = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    try:
        for child in children(gpu_key):
            if not child.isdigit():
                continue
            data = values(gpu_key + '\\' + child)
            if not data.get('DriverDesc'):
                continue
            vram = data.get('HardwareInformation.qwMemorySize', data.get('HardwareInformation.MemorySize'))
            if isinstance(vram, bytes):
                vram = int.from_bytes(vram, 'little')
            result['gpu'].append({'name': data['DriverDesc'], 'vendor': data.get('ProviderName'),
                                  'vram_bytes': vram if isinstance(vram, int) and vram > 0 else None,
                                  'driver': data.get('DriverVersion'), 'source': 'Windows display registry',
                                  'vram_kind': 'reported dedicated; shared RAM is not added'})
    except OSError as exc:
        result['errors'].append({'probe': 'registry.gpu', 'error': str(exc)})
    for flow, kind in [('Capture', 'microphone'), ('Render', 'speaker')]:
        base = r'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio' + '\\' + flow
        try:
            for child in children(base):
                endpoint = values(base + '\\' + child)
                props = values(base + '\\' + child + '\\Properties')
                name = (props.get('{a45c254e-df1c-4efd-8020-67d146a850e0},14')
                        or props.get('{b3f8fa53-0004-438e-9003-51a46e139bfc},6')
                        or props.get('{a45c254e-df1c-4efd-8020-67d146a850e0},2'))
                if name:
                    result['devices'].append({'kind': kind, 'name': str(name),
                                              'active': endpoint.get('DeviceState') == 1,
                                              'source': 'Windows audio endpoint registry'})
        except OSError as exc:
            result['errors'].append({'probe': 'registry.' + flow, 'error': str(exc)})
    # Camera class driver entries are a fallback inventory, not proof of an
    # active camera or microphone permission. CIM can refine current state.
    for guid in ('{ca3e7ab9-b4c3-4ae6-8251-579ef933890f}', '{6bdd1fc6-810f-11d0-bec7-08002be2092f}'):
        base = 'SYSTEM\\CurrentControlSet\\Control\\Class\\' + guid
        try:
            for child in children(base):
                if child.isdigit():
                    data = values(base + '\\' + child)
                    if data.get('DriverDesc'):
                        result['devices'].append({'kind': 'camera_or_imaging', 'name': data['DriverDesc'],
                                                  'active': None, 'source': 'installed device driver registry'})
        except OSError as exc:
            result['errors'].append({'probe': 'registry.camera', 'error': str(exc)})
    return result


def memory_snapshot() -> dict:
    if os.name == 'nt':
        class Status(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong)] + [
                (name, ctypes.c_ulonglong) for name in
                ('total', 'available', 'page_total', 'page_available', 'virtual_total',
                 'virtual_available', 'extended_available')]
        info = Status()
        info.length = ctypes.sizeof(info)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(info)):
            return {'status': 'AVAILABLE', 'total_bytes': info.total,
                    'available_bytes': info.available, 'available_commit_bytes': info.page_available,
                    'source': 'GlobalMemoryStatusEx'}
    if os.name != 'nt':
        try:
            values = {}
            for line in Path('/proc/meminfo').read_text(encoding='utf-8', errors='replace').splitlines():
                if ':' not in line:
                    continue
                key, raw = line.split(':', 1)
                parts = raw.strip().split()
                if parts and parts[0].isdigit():
                    values[key] = int(parts[0]) * 1024
            total = values.get('MemTotal')
            available = values.get('MemAvailable', values.get('MemFree'))
            if total and available is not None:
                # Available RAM plus currently free swap is an upper bound for
                # admission that explicitly allows CPU paging. It is not a
                # performance guarantee.
                commit = int(available) + int(values.get('SwapFree', 0))
                return {'status': 'AVAILABLE', 'total_bytes': int(total),
                        'available_bytes': int(available), 'available_commit_bytes': commit,
                        'source': '/proc/meminfo'}
        except OSError:
            pass
    try:
        import psutil
        info = psutil.virtual_memory()
        return {'status': 'AVAILABLE', 'total_bytes': info.total,
                'available_bytes': info.available, 'source': 'psutil'}
    except (ImportError, OSError):
        return {'status': 'DEPENDENCY_MISSING', 'total_bytes': None, 'available_bytes': None}


def _command(argv: list[str], timeout=8) -> dict:
    try:
        result = subprocess.run(argv, capture_output=True, timeout=timeout,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        def decode(value):
            encoding = 'utf-16' if value.startswith((b'\xff\xfe', b'\xfe\xff')) else (
                'utf-16-le' if b'\x00' in value[:100] else 'utf-8')
            return value.decode(encoding, errors='replace').strip()
        stdout, stderr = decode(result.stdout), decode(result.stderr)
        if result.returncode:
            return {'status': 'FAILED', 'exit_code': result.returncode,
                    'error': (stderr or stdout)[:500]}
        return {'status': 'AVAILABLE', 'value': stdout}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {'status': 'FAILED', 'error': str(exc)[:500]}


def profile_hardware(storage_path: Path, models=None) -> dict:
    """Installer/explicit refresh probe; never installs or starts a service."""
    result = {
        'schema_version': 1, 'captured_at': time.time(),
        'os': {'system': platform.system(), 'release': platform.release(),
               'version': platform.version(), 'architecture': platform.machine()},
        'cpu': {'name': platform.processor(), 'logical_cores': os.cpu_count()},
        'memory': memory_snapshot(),
        'gpu': {'status': 'DEPENDENCY_MISSING', 'devices': []},
        'devices': {'status': 'DEPENDENCY_MISSING', 'items': []},
    }
    result['cpu']['physical_cores'] = _physical_cores()
    try:
        usage = shutil.disk_usage(storage_path)
        result['storage'] = {'status': 'AVAILABLE', 'total_bytes': usage.total,
                             'free_bytes': usage.free, 'disk_type': 'unknown'}
    except OSError as exc:
        result['storage'] = {'status': 'FAILED', 'error': str(exc)}
    names = ('ollama', 'git', 'code', 'docker', 'wsl', 'chrome', 'msedge', 'firefox')
    programs = {name: shutil.which(name) for name in names}
    program_errors = {}
    # GUI programs are commonly installed without adding themselves to PATH.
    candidates = {
        'ollama': [('LOCALAPPDATA', 'Programs/Ollama/ollama.exe')],
        'code': [('LOCALAPPDATA', 'Programs/Microsoft VS Code/Code.exe')],
        'chrome': [('PROGRAMFILES', 'Google/Chrome/Application/chrome.exe')],
        'msedge': [('PROGRAMFILES(X86)', 'Microsoft/Edge/Application/msedge.exe')],
        'firefox': [('PROGRAMFILES', 'Mozilla Firefox/firefox.exe')],
    }
    for name, paths in candidates.items():
        for env, suffix in paths:
            base = os.environ.get(env)
            candidate = Path(base) / suffix if base else None
            try:
                if not programs[name] and candidate and candidate.is_file():
                    programs[name] = str(candidate)
            except OSError as exc:
                program_errors[name] = str(exc)[:300]
    result['programs'] = {k: {'status': 'AVAILABLE' if v else 'DEPENDENCY_MISSING', 'path': v}
                          for k, v in programs.items()}
    for name, error in program_errors.items():
        if not programs[name]:
            result['programs'][name].update(status='FAILED', error=error)
    if os.name == 'nt':
        fallback = _windows_registry()
        result['cpu'].update(fallback['cpu'])
        result['os'].update(fallback['os'])
        result['probe_errors'] = fallback['errors']
        # Device names only: no microphone/camera capture, serial numbers or LAN scan.
        script = """
$ErrorActionPreference='Stop'
@{
  gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,AdapterCompatibility,AdapterRAM,DriverVersion)
  devices=@(Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -in @('AudioEndpoint','Camera','Image') } | Select-Object Name,PNPClass,Status)
  disks=@(Get-PhysicalDisk | Select-Object MediaType,BusType)
} | ConvertTo-Json -Depth 5 -Compress
"""
        probe = _command(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script])
        if probe['status'] == 'AVAILABLE':
            try:
                data = json.loads(probe['value'])
                result['gpu'] = {'status': 'AVAILABLE', 'devices': [
                    {'name': d.get('Name'), 'vendor': d.get('AdapterCompatibility'),
                     'vram_bytes': d.get('AdapterRAM'), 'driver': d.get('DriverVersion'), 'source': 'CIM'}
                    for d in data['gpu']],
                                  'vram_note': 'AdapterRAM is advisory; may truncate above 4 GiB'}
                result['devices'] = {'status': 'AVAILABLE', 'items': data['devices'],
                                      'note': 'Enumeration does not prove capture/playback permission'}
                result['storage']['physical_disks'] = data['disks']
            except (ValueError, KeyError, TypeError) as exc:
                probe = {'status': 'FAILED', 'error': str(exc)}
        if probe['status'] != 'AVAILABLE':
            result['probe_errors'].append({'probe': 'CIM', 'error': probe.get('error')})
            result['gpu'] = {'status': 'PARTIAL' if fallback['gpu'] else 'FAILED', 'devices': fallback['gpu']}
            result['devices'] = {'status': 'PARTIAL' if fallback['devices'] else 'FAILED',
                                 'items': fallback['devices'], 'note': 'Inventory only; no capture or playback attempted'}
    nvidia = shutil.which('nvidia-smi')
    if nvidia:
        probe = _command([nvidia, '--query-gpu=name,memory.total,memory.free,driver_version',
                          '--format=csv,noheader,nounits'])
        result['nvidia'] = probe
    if models is not None:
        result['ollama'] = {'endpoint': models.base, **models.list_models()}
        if not result['ollama'].get('online'):
            from .models import OllamaRouter, _runtime_ollama_url
            for base in dict.fromkeys([_runtime_ollama_url(), 'http://127.0.0.1:11434', 'http://127.0.0.1:11500']):
                if not base or base == models.base:
                    continue
                candidate = OllamaRouter(models.config_path, base)
                status = candidate.list_models()
                if status.get('online'):
                    result['ollama'] = {'endpoint': base, **status}
                    break
    result['wsl'] = _command([programs['wsl'], '--status'], timeout=4) if programs['wsl'] else {'status': 'DEPENDENCY_MISSING'}
    result['docker'] = _command([programs['docker'], '-H', 'npipe:////./pipe/docker_engine', 'version',
                                 '--format', '{{json .}}'], timeout=4) if programs['docker'] else {'status': 'DEPENDENCY_MISSING'}
    return result


def admission(model_bytes: int | None, memory: dict, requested_workers=1, *, allow_cpu_paging=False) -> dict:
    """Conservative initial RAM admission, explicitly an estimate not a benchmark.

    Account for resident weights, a bounded context allowance and 2 GiB physical
    headroom. When CPU paging is explicitly allowed, the commit check covers the
    estimated worker itself; the 2 GiB headroom is already reserved in physical RAM
    and must not be counted a second time against Windows' available commit limit.
    """
    available = memory.get('available_bytes')
    if not available or not model_bytes or model_bytes <= 0:
        return {'admitted': False, 'max_workers': 0, 'reason': 'Memory/model size unknown'}
    # Local /api/ps observed ~8% weight/runtime overhead for the installed
    # Q4 coding model at 4096 context. Reserve 10% plus a separate 512 MiB
    # context allowance; this remains an estimate, not a memory guarantee.
    estimate = int(model_bytes * 1.10) + GIB // 2
    budget = max(0, int(available) - 2 * GIB)
    available_commit = int(memory.get('available_commit_bytes') or 0)
    workers = min(max(1, int(requested_workers)), 3, budget // estimate)
    paging = (workers == 0 and allow_cpu_paging and available >= 2 * GIB
              and memory.get('total_bytes', 0) >= estimate + 2 * GIB
              and available_commit >= estimate)
    if paging:
        workers = 1
    return {'admitted': workers > 0, 'max_workers': workers,
            'estimated_worker_bytes': estimate, 'budget_bytes': budget,
            'available_bytes': int(available), 'available_commit_bytes': available_commit,
            'required_commit_bytes': estimate,
            'mode': 'cpu_paging' if paging else 'physical_ram',
            'reason': 'Conservative RAM estimate; live latency/quality calibration pending'}
