from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

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
        self.workspace = ROOT / 'workspace'
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.allowed_file = self.workspace / '.broker-security-read.txt'
        self.allowed_file.write_text('SARUS broker workspace test', encoding='utf-8')

    def tearDown(self):
        self.allowed_file.unlink(missing_ok=True)
        (self.workspace / '.broker-audit-test.txt').unlink(missing_ok=True)
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

    def test_approval_proof_is_bound_to_request_action_and_parameters(self):
        rid = str(uuid.uuid4())
        params = {'resource_id': 'ollama'}
        proof = self.broker.create_approval_proof(rid, 'process.stop', params, ttl_seconds=60)
        self.assertTrue(self.broker._approval_ok(proof, rid, 'process.stop', params))
        self.assertFalse(self.broker._approval_ok(proof, str(uuid.uuid4()), 'process.stop', params))
        self.assertFalse(self.broker._approval_ok(proof, rid, 'service.stop', params))
        self.assertFalse(self.broker._approval_ok(proof, rid, 'process.stop', {'resource_id': 'ollama', 'force': True}))

    def test_protected_secret_file_can_be_loaded_without_environment_secret(self):
        secret_path = Path(self.tmp.name) / 'approval.secret'
        secret_path.write_text('A' * 64, encoding='utf-8')
        old_secret = os.environ.pop('SARUS_BROKER_APPROVAL_SECRET', None)
        old_file = os.environ.get('SARUS_BROKER_SECRET_FILE')
        os.environ['SARUS_BROKER_SECRET_FILE'] = str(secret_path)
        try:
            broker = PrivilegedBroker(
                ROOT,
                ROOT / 'config/broker_allowlist.json',
                self.policy,
                self.windows,
                self.receipts,
            )
            self.assertTrue(broker.status()['approval_secret_configured'])
            self.assertEqual(broker.status()['approval_secret_source'], 'protected-local-file')
        finally:
            if old_secret is not None:
                os.environ['SARUS_BROKER_APPROVAL_SECRET'] = old_secret
            if old_file is None:
                os.environ.pop('SARUS_BROKER_SECRET_FILE', None)
            else:
                os.environ['SARUS_BROKER_SECRET_FILE'] = old_file

    def test_receipt_key_migrates_outside_workspace(self):
        workspace = Path(self.tmp.name) / 'workspace-data'
        workspace.mkdir()
        legacy = workspace / 'receipt-signing.key'
        original_key = os.urandom(32)
        legacy.write_bytes(original_key)
        external = Path(self.tmp.name) / 'protected' / 'receipt-signing.key'
        old = os.environ.get('SARUS_RECEIPT_SIGNING_KEY_FILE')
        os.environ['SARUS_RECEIPT_SIGNING_KEY_FILE'] = str(external)
        try:
            store = ReceiptStore(workspace / 'receipts.db')
            self.assertTrue(external.is_file())
            self.assertEqual(external.read_bytes(), original_key)
            self.assertFalse(legacy.exists())
            store.create('t', 's', 'test', 'completed', {'ok': True})
            self.assertTrue(store.verify_chain()['ok'])
        finally:
            if old is None:
                os.environ.pop('SARUS_RECEIPT_SIGNING_KEY_FILE', None)
            else:
                os.environ['SARUS_RECEIPT_SIGNING_KEY_FILE'] = old

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
            'parameters': {'path': str(self.allowed_file), 'extra': 'nope'},
        })
        self.assertEqual(r['status'], 'invalid')

    def test_allowlisted_workspace_read(self):
        r = self.broker.handle({
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(self.allowed_file)},
        })
        self.assertTrue(r['ok'])
        self.assertIn('SARUS', r['result']['content'])
        receipt_json = json.dumps(r['receipt'], ensure_ascii=False)
        self.assertNotIn(r['result']['content'], receipt_json)
        self.assertTrue(r['receipt']['payload']['result']['content']['redacted'])

    def test_sensitive_write_content_is_redacted_from_receipt(self):
        target = self.workspace / '.broker-audit-test.txt'
        marker = 'TOP-SECRET-MARKER-' + uuid.uuid4().hex
        r = self.broker.handle({
            'action_id': 'workspace.file.write',
            'parameters': {'path': str(target), 'content': marker},
        })
        self.assertTrue(r['ok'])
        self.assertNotIn(marker, json.dumps(r['receipt'], ensure_ascii=False))
        redacted = r['receipt']['payload']['parameters']['content']
        self.assertTrue(redacted['redacted'])
        self.assertEqual(redacted['bytes'], len(marker.encode('utf-8')))

    def test_sarus_source_and_security_config_are_not_workspace_writable(self):
        targets = [
            ROOT / 'config' / 'broker_allowlist.json',
            ROOT / 'sarus' / 'core' / 'windows.py',
            ROOT / 'README.md',
        ]
        for target in targets:
            r = self.broker.handle({
                'action_id': 'workspace.file.write',
                'parameters': {'path': str(target), 'content': 'SHOULD-NOT-WRITE'},
            })
            self.assertFalse(r['ok'], target)
            self.assertEqual(r['status'], 'denied', target)

    def test_path_outside_approved_workspace_is_denied_for_read(self):
        r = self.broker.handle({
            'action_id': 'workspace.file.read',
            'parameters': {'path': str(ROOT / 'README.md')},
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
            'parameters': {'path': str(self.allowed_file)},
        }
        first = self.broker.handle(req)
        second = self.broker.handle(req)
        self.assertTrue(first['ok'])
        self.assertFalse(second['ok'])
        self.assertEqual(second['status'], 'denied')

    def test_receipt_chain_detects_signed_rows(self):
        self.broker.handle({'action_id': 'workspace.file.read', 'parameters': {'path': str(self.allowed_file)}})
        v = self.receipts.verify_chain()
        self.assertTrue(v['ok'])
        self.assertGreater(v['signed_count'], 0)
        self.assertEqual(v['algorithm'], 'HMAC-SHA256')

    @unittest.skipUnless(os.name == 'nt', 'Windows-only broker smoke')
    def test_windows_readonly_system_actions(self):
        processes = self.broker.handle({'action_id': 'system.processes.list', 'parameters': {}})
        services = self.broker.handle({'action_id': 'system.services.list', 'parameters': {}})
        self.assertTrue(processes['ok'])
        self.assertTrue(services['ok'])
        self.assertNotIn(processes['result'].get('stdout', '')[:50], json.dumps(processes['receipt']))
        self.assertTrue(processes['receipt']['payload']['result']['stdout']['redacted'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
