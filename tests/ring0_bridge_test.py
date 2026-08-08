from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.policy import PolicyEngine
from sarus.core.privileged_broker import PrivilegedBroker
from sarus.core.receipts import ReceiptStore
from sarus.core.ring0 import IOCTL_SARUS_RING0_PING, IOCTL_SARUS_RING0_STATUS, Ring0Bridge
from sarus.core.windows import WindowsBroker


class Ring0BridgeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.receipts = ReceiptStore(Path(self.tmp.name) / 'ring0.db')
        self.windows = WindowsBroker(ROOT)
        self.policy = PolicyEngine(ROOT / 'config/policy.json')
        self.broker = PrivilegedBroker(
            ROOT,
            ROOT / 'config/broker_allowlist.json',
            self.policy,
            self.windows,
            self.receipts,
        )

    def tearDown(self):
        self.tmp.cleanup()

    def test_ring0_actions_are_explicitly_allowlisted(self):
        cfg = json.loads((ROOT / 'config/broker_allowlist.json').read_text(encoding='utf-8'))
        self.assertTrue(cfg['actions']['ring0.ping']['enabled'])
        self.assertTrue(cfg['actions']['ring0.status']['enabled'])

    def test_raw_driver_and_kernel_memory_actions_stay_denied(self):
        for action in ('driver.raw_ioctl', 'kernel.read_memory', 'kernel.write_memory'):
            r = self.broker.handle({'action_id': action, 'parameters': {}})
            self.assertFalse(r['ok'])
            self.assertEqual(r['status'], 'denied')

    def test_ring0_action_reaches_executor_instead_of_policy_block(self):
        r = self.broker.handle({'action_id': 'ring0.status', 'parameters': {}})
        # CI has no SarusRing0.sys loaded, so a controlled execution failure is
        # expected. The important property is that Ring0 is allowed/reached,
        # not denied as a forbidden kernel action.
        self.assertIn(r['status'], {'completed', 'failed'})
        self.assertNotEqual(r['status'], 'denied')

    def test_bridge_has_fixed_ioctl_surface_only(self):
        bridge = Ring0Bridge()
        self.assertFalse(hasattr(bridge, 'raw_ioctl'))
        self.assertFalse(hasattr(bridge, 'read_kernel_memory'))
        self.assertFalse(hasattr(bridge, 'write_kernel_memory'))
        self.assertEqual(IOCTL_SARUS_RING0_PING, 0x00226000)
        self.assertEqual(IOCTL_SARUS_RING0_STATUS, 0x00226004)

    @unittest.skipUnless(os.name == 'nt', 'Windows-only driver-open smoke')
    def test_windows_missing_or_installed_driver_is_reported_cleanly(self):
        r = Ring0Bridge().status()
        self.assertIn('driver_present', r)
        if r['ok']:
            self.assertEqual(r['protocol_version'], 1)
        else:
            self.assertIn('error', r)


if __name__ == '__main__':
    unittest.main(verbosity=2)
