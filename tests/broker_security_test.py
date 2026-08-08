from __future__ import annotations

import os
import tempfile
import time
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

from sarus.core.policy import PolicyEngine
from sarus.core.privileged_broker import PrivilegedBroker
from sarus.core.receipts import ReceiptStore
from sarus.core.windows import WindowsBroker


class BrokerSecurityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        db = Path(self.tmp.name) / 'broker-test.db'
        self.receipts = ReceiptStore(db)
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

    def test_default_deny_and_signed_denial_receipt(self):
        r = self.broker.handle({'action_id': 'not.allowlisted', 'parameters': {}})
        self.assertFalse(r['ok'])
        self.assertEqual(r['status'], 'denied')
        self.assertTrue(r['receipt']['signature']['value'])
        v = self.receipts.verify_chain()
        self.assertTrue(v['ok'])
        self.assertEqual(v['signed_count'], 1)

    def test_legacy_powershell_is_blocked_even_with_boolean_approval(self):
        with self.assertRaises(PermissionError):
            self.windows.action('powershell', {'command': 'whoami'}, approved=True)

    def test_json_approved_flag_is_not_authorization(self):
        r = self.broker.handle({
            'action_id': 'process.stop',
            'parameters': {'resource_id': 'ollama'},
            'approved': True,
        })
        self.assertFalse(r['ok'])
        self.assertEqual(r['status'], 'invalid')

    def test_high_risk_action_requires_out_of_band_proof(self):
        r = self.broker.handle({
            'action_id': 'process.stop',
            'parameters': {'resource_id': 'ollama'},
        })
        self.assertFalse(r['ok'])
        self.assertEqual(r['status'], 'approval_required')

    def test_kernel_memory_action_permanently_denied(self):
        r = self.broker.handle({'action_id': 'kernel.read_memory', 'parameters': {}})
        self.assertEqual(r['status'], 'denied')

    def test_unknown_resource_is_denied(self):
        r = self.broker.handle({
            'action_id': 'service.query',
            'parameters': {'resource_id': 'not-allowlisted'},
        })
        self.assertEqual(r['status'], 'denied')

    def test_parameter_schema_rejects_extra_fields(self):
        r = self.broker.handle({
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(ROOT / 'README.md'), 'extra': 'nope'},
        })
        self.assertEqual(r['status'], 'invalid')

    def test_allowlisted_workspace_read(self):
        r = self.broker.handle({
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(ROOT / 'README.md')},
        })
        self.assertTrue(r['ok'])
        self.assertIn('SARUS', r['result']['content'])

    def test_workspace_escape_is_denied(self):
        r = self.broker.handle({
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(ROOT.parent / 'outside.txt')},
        })
        self.assertFalse(r['ok'])
        self.assertEqual(r['status'], 'denied')

    def test_non_http_url_is_invalid(self):
        r = self.broker.handle({
            'action_id': 'url.open',
            'parameters': {'url': 'file:///etc/passwd'},
        })
        self.assertEqual(r['status'], 'invalid')

    def test_replay_is_denied(self):
        rid = str(uuid.uuid4())
        req = {
            'request_id': rid,
            'nonce': 'n-' + uuid.uuid4().hex,
            'timestamp': time.time(),
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(ROOT / 'README.md')},
        }
        first = self.broker.handle(req)
        second = self.broker.handle(req)
        self.assertTrue(first['ok'])
        self.assertFalse(second['ok'])
        self.assertEqual(second['status'], 'denied')

    def test_receipt_chain_detects_signed_rows(self):
        self.broker.handle({'action_id': 'workspace.file.read', 'parameters': {'path': str(ROOT / 'README.md')}})
        v = self.receipts.verify_chain()
        self.assertTrue(v['ok'])
        self.assertGreater(v['signed_count'], 0)
        self.assertEqual(v['algorithm'], 'HMAC-SHA256')


if __name__ == '__main__':
    unittest.main(verbosity=2)
