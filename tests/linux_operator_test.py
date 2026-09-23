from __future__ import annotations
import json, os, tempfile, unittest
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from sarus.core.windows import WindowsBroker

class LinuxOperatorTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        (self.root/'config').mkdir(); (self.root/'workspace').mkdir()
        (self.root/'config/broker_allowlist.json').write_text(json.dumps({'path_scopes':{'user_workspace':['workspace']}}))
        self.b=WindowsBroker(self.root)
    def tearDown(self): self.tmp.cleanup()
    def test_cross_platform_workspace_and_git_contract(self):
        out=self.b.execute_typed('workspace.file.write',{'path':'workspace/a.txt','content':'hello'}, {})
        self.assertTrue(out['ok']); self.assertEqual((self.root/'workspace/a.txt').read_text(),'hello')
    @unittest.skipIf(os.name=='nt','Linux-specific')
    def test_linux_process_inventory(self):
        out=self.b.execute_typed('system.processes.list',{}, {})
        self.assertTrue(out['ok'],out); self.assertIn('PID',out['stdout'].upper())
    @unittest.skipIf(os.name=='nt','Linux-specific')
    def test_linux_service_mapping_requires_allowlisted_unit(self):
        with self.assertRaises(ValueError): self.b.execute_typed('service.query',{}, {'linux_unit':'bad/unit'})

if __name__=='__main__': unittest.main(verbosity=2)
