from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from sarus.core.windows import WindowsBroker


class TypedOperatorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'config').mkdir()
        (self.root / 'workspace').mkdir()
        (self.root / 'outputs').mkdir()
        (self.root / 'projects').mkdir()
        (self.root / 'config' / 'broker_allowlist.json').write_text(
            json.dumps({'path_scopes': {'user_workspace': ['workspace', 'outputs', 'projects']}}),
            encoding='utf-8',
        )
        self.broker = WindowsBroker(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def test_workspace_lifecycle_is_scoped(self):
        self.assertTrue(self.broker.execute_typed('workspace.directory.create', {'path': 'workspace/demo'}, {})['ok'])
        self.assertTrue(self.broker.execute_typed('workspace.file.write', {'path': 'workspace/demo/a.txt', 'content': 'hello'}, {})['ok'])
        read = self.broker.execute_typed('workspace.file.read', {'path': 'workspace/demo/a.txt'}, {})
        self.assertEqual(read['content'], 'hello')
        listed = self.broker.execute_typed('workspace.directory.list', {'path': 'workspace/demo'}, {})
        self.assertEqual(listed['items'][0]['name'], 'a.txt')
        copied = self.broker.execute_typed(
            'workspace.file.copy',
            {'source_path': 'workspace/demo/a.txt', 'destination_path': 'outputs/a.txt'},
            {},
        )
        self.assertTrue(copied['ok'])
        moved = self.broker.execute_typed(
            'workspace.file.move',
            {'source_path': 'outputs/a.txt', 'destination_path': 'projects/a.txt'},
            {},
        )
        self.assertTrue(moved['ok'])
        stat = self.broker.execute_typed('workspace.path.stat', {'path': 'projects/a.txt'}, {})
        self.assertTrue(stat['exists'])
        self.assertTrue(stat['is_file'])

    def test_workspace_escape_is_rejected(self):
        with self.assertRaises(PermissionError):
            self.broker.execute_typed('workspace.file.read', {'path': '../outside.txt'}, {})

    def test_config_keeps_arbitrary_shell_forbidden_and_delete_approved(self):
        cfg = json.loads((Path(__file__).resolve().parents[1] / 'config' / 'broker_allowlist.json').read_text(encoding='utf-8'))
        for forbidden in ('powershell', 'cmd', 'shell', 'arbitrary_exec', 'driver.raw_ioctl', 'kernel.write_memory'):
            self.assertIn(forbidden, cfg['forbidden_actions'])
        delete = cfg['actions']['workspace.file.delete']
        self.assertTrue(delete['requires_approval'])
        self.assertGreaterEqual(delete['risk'], 4)
        launch = cfg['actions']['app.launch']
        self.assertEqual(launch['resource_group'], 'apps')
        self.assertEqual(set(cfg['resources']['apps']), {'vscode', 'notepad', 'explorer'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
