from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from sarus.core.network import NetworkManager


class NetworkManagerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = Path(self.tmp.name) / 'jubi.db'
        self.net = NetworkManager(self.db)

    def tearDown(self):
        self.tmp.cleanup()

    def test_register_list_and_delete_device(self):
        item = self.net.register(
            '192.168.1.20',
            'NAS',
            [{'name': 'ssh', 'port': 22}, {'name': 'https', 'port': 443}],
            'authorized test device',
        )
        self.assertEqual(item['host'], '192.168.1.20')
        self.assertEqual(len(item['services']), 2)
        self.assertEqual(len(self.net.list_devices()), 1)
        self.assertTrue(self.net.delete(item['id'])['ok'])
        self.assertEqual(self.net.list_devices(), [])

    def test_rejects_invalid_hosts_and_ports(self):
        with self.assertRaises(ValueError):
            self.net.register('bad host; rm -rf /')
        with self.assertRaises(ValueError):
            self.net.register('192.168.1.10', services=[{'name': 'bad', 'port': 70000}])
        with self.assertRaises(ValueError):
            self.net.register('192.168.1.10', services='ssh:22')

    def test_parses_windows_and_linux_neighbor_cache_without_active_scan(self):
        windows = '''
Interface: 192.168.1.10 --- 0x4
  Internet Address      Physical Address      Type
  192.168.1.1           aa-bb-cc-dd-ee-ff     dynamic
  192.168.1.40          11-22-33-44-55-66     dynamic
'''
        linux = '''
192.168.1.1 dev eth0 lladdr aa:bb:cc:dd:ee:ff REACHABLE
192.168.1.40 dev eth0 lladdr 11:22:33:44:55:66 STALE
'''
        self.assertEqual(len(self.net._parse_neighbors(windows)), 2)
        self.assertEqual(len(self.net._parse_neighbors(linux)), 2)
        status = self.net.status()
        self.assertFalse(status['active_scan'])
        self.assertFalse(status['credential_bruteforce'])
        self.assertFalse(status['exploit_or_lateral_movement'])

    def test_registered_health_check_does_not_scan_ports(self):
        item = self.net.register('localhost', 'Local host', [])
        out = self.net.check(item['id'])
        self.assertTrue(out['ok'])
        self.assertEqual(out['services'], [])
        self.assertFalse(out['scan'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
