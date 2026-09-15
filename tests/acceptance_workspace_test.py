import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sarus.integrations.acceptance_workspace import AcceptanceWorkspace, validate_module

spec = importlib.util.spec_from_file_location('acceptance_controller', ROOT / 'scripts/run_development_acceptance.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / 'project'
        self.project.mkdir()
        (self.project / 'README.md').write_text('Multiply price and quantity.')
        (self.project / 'pricing.py').write_text('def subtotal(unit_price, quantity):\n    return unit_price + quantity\n')
        (self.project / 'test_pricing.py').write_text(controller.PROJECT_TESTS)
        harness = self.root / 'run_tests.py'
        harness.write_text(controller.HARNESS)
        self.events = []
        self.workspace = AcceptanceWorkspace(self.project, harness, self.events)

    def test_real_failure_edit_test_success_and_independent_verification(self):
        self.assertEqual(self.workspace.execute('test')['exit_code'], 1)
        self.workspace.execute('write', 'pricing.py', 'def subtotal(unit_price, quantity):\n    return unit_price * quantity\n')
        self.assertEqual(self.workspace.execute('test')['exit_code'], 0)
        self.workspace.phase = 'verifying'
        self.assertEqual(self.workspace.execute('verify')['exit_code'], 0)
        self.assertIn('-    return unit_price + quantity', self.workspace.execute('diff')['diff'])

    def test_worker_cannot_change_tests_or_escape_workspace(self):
        for path in ('test_pricing.py', '../kanban.db', str(self.root / 'sessions.db')):
            with self.assertRaises(PermissionError):
                self.workspace.execute('write', path, 'anything')
        with self.assertRaises(PermissionError):
            self.workspace.execute('read', '../kanban.db')

    def test_arbitrary_python_is_rejected_before_execution(self):
        for code in ('import os\n', 'def subtotal(unit_price, quantity):\n return __import__("os").system("whoami")',
                     'def subtotal(unit_price, quantity):\n while True: pass',
                     'def subtotal(unit_price, quantity):\n return unit_price ** quantity'):
            with self.assertRaises(PermissionError):
                validate_module(code)

    def test_review_has_no_write_permission_and_worker_cannot_verify(self):
        with self.assertRaises(PermissionError):
            self.workspace.execute('verify')
        self.workspace.phase = 'review'
        with self.assertRaises(PermissionError):
            self.workspace.execute('write', 'pricing.py', 'def subtotal(unit_price, quantity):\n return 0')
        with self.assertRaises(PermissionError):
            self.workspace.execute('test')
        self.workspace.phase = 'closed'
        with self.assertRaises(PermissionError):
            self.workspace.execute('read', 'pricing.py')

    def test_changed_test_harness_and_cancellation_fail_closed(self):
        (self.project / 'test_pricing.py').write_text('print("fake green")')
        with self.assertRaises(PermissionError):
            self.workspace.execute('test')
        self.workspace.cancel_check = lambda: True
        with self.assertRaises(RuntimeError):
            self.workspace.execute('read', 'pricing.py')


if __name__ == '__main__':
    unittest.main(verbosity=2)
