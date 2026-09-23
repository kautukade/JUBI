from __future__ import annotations
import json, os, tempfile, unittest
from pathlib import Path
from types import SimpleNamespace
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from sarus.core.privileged_broker import PrivilegedBroker
from sarus.core.policy import PolicyEngine

class Receipts:
    SIGNATURE_ALGORITHM='test'
    def create(self,*a,**k): return {'hash':'x'}
class Executor:
    def execute_typed(self,*a,**k): return {'ok':True}

class BrokerPlatformTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); root=Path(self.tmp.name); (root/'data').mkdir(); (root/'config').mkdir()
        cfg={'schema':'x','actions':{'workspace.file.read':{'enabled':True,'risk':0,'parameters':{'path':{'type':'string','required':True}}},'ring0.status':{'enabled':True,'risk':1,'parameters':{}},'service.stop':{'enabled':True,'risk':4,'requires_approval':True,'parameters':{}}},'forbidden_actions':[]}
        (root/'config/b.json').write_text(json.dumps(cfg)); (root/'config/p.json').write_text(json.dumps({'default':'allow_read','levels':{},'always_require_approval':[],'isolated_only':[],'never_auto':[]}))
        self.b=PrivilegedBroker(root,root/'config/b.json',PolicyEngine(root/'config/p.json'),Executor(),Receipts())
    def tearDown(self): self.tmp.cleanup()
    @unittest.skipIf(os.name=='nt','Linux-specific')
    def test_linux_status_hides_desktop_and_host_mutation(self):
        s=self.b.status(); self.assertIn('workspace.file.read',s['available_actions']); self.assertIn('ring0.status',s['unavailable_actions']); self.assertIn('service.stop',s['unavailable_actions'])

if __name__=='__main__': unittest.main(verbosity=2)
