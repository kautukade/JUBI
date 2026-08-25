from __future__ import annotations

import ipaddress
import json
import os
import re
import socket
import subprocess
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction


_HOST_RE = re.compile(r'^[A-Za-z0-9._-]{1,253}$')


class NetworkManager:
    """Authorized LAN visibility without scanning/exploitation.

    The manager intentionally provides only:
    - passive neighbour discovery from the host ARP/neighbor cache;
    - an explicit user-managed device registry;
    - DNS resolution and reachability checks for ports already registered by the
      user for a device.

    It does not brute-force credentials, enumerate arbitrary port ranges,
    exploit services, perform stealth discovery, or execute remote commands.
    """

    def __init__(self, db: Path, event_bus=None):
        self.db = db
        self.event_bus = event_bus
        with transaction(db) as c:
            c.execute(
                "CREATE TABLE IF NOT EXISTS network_devices("
                "id TEXT PRIMARY KEY,ts REAL,updated REAL,label TEXT,host TEXT UNIQUE,"
                "services TEXT,notes TEXT,enabled INTEGER DEFAULT 1)"
            )
            c.execute(
                "CREATE TABLE IF NOT EXISTS network_observations("
                "id TEXT PRIMARY KEY,ts REAL,device_id TEXT,host TEXT,ip TEXT,mac TEXT,"
                "kind TEXT,status TEXT,details TEXT)"
            )

    def _emit(self, kind: str, payload: dict):
        if self.event_bus is None:
            return
        try:
            self.event_bus.emit(kind, payload)
        except Exception:
            pass

    @staticmethod
    def _host(value: str) -> str:
        host = str(value or '').strip()
        if not host:
            raise ValueError('host is required')
        try:
            ipaddress.ip_address(host)
            return host
        except ValueError:
            pass
        if not _HOST_RE.fullmatch(host) or '..' in host or host.startswith(('-', '.')) or host.endswith(('-', '.')):
            raise ValueError('invalid host or IP address')
        return host

    @staticmethod
    def _services(value) -> list[dict]:
        if value in (None, ''):
            return []
        if not isinstance(value, list):
            raise ValueError('services must be a list')
        out = []
        seen = set()
        for item in value[:8]:
            if not isinstance(item, dict):
                raise ValueError('each service must be an object')
            port = item.get('port')
            if type(port) is not int or not 1 <= port <= 65535:
                raise ValueError('service port must be an integer between 1 and 65535')
            name = str(item.get('name') or f'tcp/{port}').strip()[:80]
            if port in seen:
                continue
            seen.add(port)
            out.append({'name': name, 'port': port})
        return out

    def register(self, host: str, label: str = '', services=None, notes: str = '') -> dict:
        host = self._host(host)
        service_list = self._services(services)
        now = time.time()
        label = str(label or host).strip()[:160]
        notes = str(notes or '')[:2000]
        with transaction(self.db) as c:
            row = c.execute('SELECT id,ts FROM network_devices WHERE host=?', (host,)).fetchone()
            if row:
                did, created = row[0], float(row[1] or now)
                c.execute(
                    'UPDATE network_devices SET updated=?,label=?,services=?,notes=?,enabled=1 WHERE id=?',
                    (now, label, json.dumps(service_list), notes, did),
                )
            else:
                did, created = str(uuid.uuid4()), now
                c.execute(
                    'INSERT INTO network_devices(id,ts,updated,label,host,services,notes,enabled) VALUES(?,?,?,?,?,?,?,1)',
                    (did, created, now, label, host, json.dumps(service_list), notes),
                )
        item = self.get(did)
        self._emit('NETWORK_DEVICE_REGISTERED', {'device_id': did, 'host': host, 'services': len(service_list)})
        return item

    def get(self, device_id: str) -> dict | None:
        with read_connection(self.db) as c:
            r = c.execute(
                'SELECT id,ts,updated,label,host,services,notes,enabled FROM network_devices WHERE id=?',
                (str(device_id),),
            ).fetchone()
        if not r:
            return None
        return {
            'id': r[0], 'ts': r[1], 'updated': r[2], 'label': r[3], 'host': r[4],
            'services': json.loads(r[5] or '[]'), 'notes': r[6] or '', 'enabled': bool(r[7]),
        }

    def list_devices(self) -> list[dict]:
        with read_connection(self.db) as c:
            rows = c.execute(
                'SELECT id,ts,updated,label,host,services,notes,enabled FROM network_devices ORDER BY updated DESC'
            ).fetchall()
        return [
            {
                'id': r[0], 'ts': r[1], 'updated': r[2], 'label': r[3], 'host': r[4],
                'services': json.loads(r[5] or '[]'), 'notes': r[6] or '', 'enabled': bool(r[7]),
            }
            for r in rows
        ]

    def delete(self, device_id: str) -> dict:
        item = self.get(device_id)
        if not item:
            raise KeyError('network device not found')
        with transaction(self.db) as c:
            c.execute('DELETE FROM network_devices WHERE id=?', (item['id'],))
        self._emit('NETWORK_DEVICE_REMOVED', {'device_id': item['id'], 'host': item['host']})
        return {'ok': True, 'id': item['id']}

    @staticmethod
    def _passive_command() -> list[str] | None:
        if os.name == 'nt':
            return ['arp', '-a']
        if os.path.exists('/sbin/ip') or os.path.exists('/usr/sbin/ip') or os.path.exists('/bin/ip'):
            return ['ip', 'neigh', 'show']
        return None

    @staticmethod
    def _parse_neighbors(text: str) -> list[dict]:
        found = {}
        for line in str(text or '').splitlines():
            # Windows: 192.168.1.1   aa-bb-cc-dd-ee-ff   dynamic
            m = re.search(r'\b(\d{1,3}(?:\.\d{1,3}){3})\b\s+([0-9a-fA-F:-]{11,20})\s+', line)
            if m:
                ip, mac = m.group(1), m.group(2).replace('-', ':').lower()
            else:
                # Linux: 192.168.1.1 dev eth0 lladdr aa:bb:... REACHABLE
                m = re.search(r'^(\d{1,3}(?:\.\d{1,3}){3})\b.*?\blladdr\s+([0-9a-fA-F:]{17})\b', line.strip())
                if not m:
                    continue
                ip, mac = m.group(1), m.group(2).lower()
            try:
                addr = ipaddress.ip_address(ip)
                if not addr.is_private and not addr.is_link_local:
                    continue
            except ValueError:
                continue
            found[ip] = {'ip': ip, 'mac': mac, 'source': 'host-neighbor-cache'}
        return sorted(found.values(), key=lambda x: tuple(int(p) for p in x['ip'].split('.')))

    def passive_discover(self) -> dict:
        command = self._passive_command()
        if not command:
            return {'ok': False, 'devices': [], 'error': 'no supported neighbour-cache command is available'}
        try:
            cp = subprocess.run(command, capture_output=True, text=True, timeout=12, shell=False, errors='replace')
        except Exception as exc:
            return {'ok': False, 'devices': [], 'error': str(exc)[:500]}
        devices = self._parse_neighbors(cp.stdout)
        now = time.time()
        with transaction(self.db) as c:
            for item in devices:
                c.execute(
                    'INSERT INTO network_observations(id,ts,device_id,host,ip,mac,kind,status,details) VALUES(?,?,?,?,?,?,?,?,?)',
                    (str(uuid.uuid4()), now, '', item['ip'], item['ip'], item['mac'], 'passive-neighbor', 'observed', '{}'),
                )
        self._emit('NETWORK_PASSIVE_DISCOVERY', {'observed': len(devices), 'active_scan': False})
        return {'ok': cp.returncode == 0, 'devices': devices, 'active_scan': False, 'command': command[0]}

    def check(self, device_id: str, timeout: float = 1.5) -> dict:
        item = self.get(device_id)
        if not item:
            raise KeyError('network device not found')
        host = item['host']
        started = time.perf_counter()
        try:
            infos = socket.getaddrinfo(host, None, type=socket.SOCK_STREAM)
            ips = []
            for info in infos:
                ip = info[4][0]
                if ip not in ips:
                    ips.append(ip)
            if not ips:
                raise OSError('host did not resolve')
        except Exception as exc:
            result = {
                'ok': False, 'device_id': item['id'], 'host': host, 'resolved_ips': [],
                'services': [], 'latency_ms': round((time.perf_counter() - started) * 1000, 2),
                'error': str(exc)[:500], 'scan': False,
            }
            self._store_check(item, result)
            return result

        service_results = []
        for service in item['services'][:8]:
            port = int(service['port'])
            s_started = time.perf_counter()
            reachable = False
            error = ''
            try:
                with socket.create_connection((host, port), timeout=max(0.2, min(float(timeout), 5.0))):
                    reachable = True
            except Exception as exc:
                error = str(exc)[:180]
            service_results.append(
                {
                    'name': service['name'], 'port': port, 'reachable': reachable,
                    'latency_ms': round((time.perf_counter() - s_started) * 1000, 2), 'error': error,
                }
            )
        result = {
            'ok': True,
            'device_id': item['id'],
            'host': host,
            'resolved_ips': ips[:8],
            'services': service_results,
            'latency_ms': round((time.perf_counter() - started) * 1000, 2),
            'scan': False,
        }
        self._store_check(item, result)
        return result

    def _store_check(self, item: dict, result: dict):
        with transaction(self.db) as c:
            c.execute(
                'INSERT INTO network_observations(id,ts,device_id,host,ip,mac,kind,status,details) VALUES(?,?,?,?,?,?,?,?,?)',
                (
                    str(uuid.uuid4()), time.time(), item['id'], item['host'],
                    (result.get('resolved_ips') or [''])[0], '', 'registered-health',
                    'ok' if result.get('ok') else 'error', json.dumps(result, ensure_ascii=False),
                ),
            )
        self._emit(
            'NETWORK_DEVICE_CHECKED',
            {'device_id': item['id'], 'host': item['host'], 'ok': bool(result.get('ok')), 'ports': len(item['services'])},
        )

    def recent_observations(self, limit: int = 100) -> list[dict]:
        limit = max(1, min(int(limit), 500))
        with read_connection(self.db) as c:
            rows = c.execute(
                'SELECT id,ts,device_id,host,ip,mac,kind,status,details FROM network_observations ORDER BY ts DESC LIMIT ?',
                (limit,),
            ).fetchall()
        out = []
        for r in rows:
            try:
                details = json.loads(r[8] or '{}')
            except Exception:
                details = {}
            out.append(
                {'id': r[0], 'ts': r[1], 'device_id': r[2], 'host': r[3], 'ip': r[4], 'mac': r[5],
                 'kind': r[6], 'status': r[7], 'details': details}
            )
        return out

    def status(self) -> dict:
        return {
            'mode': 'authorized-lan',
            'registered_devices': len(self.list_devices()),
            'active_scan': False,
            'credential_bruteforce': False,
            'exploit_or_lateral_movement': False,
            'capabilities': ['passive-neighbor-cache', 'explicit-device-registry', 'registered-service-health'],
        }
