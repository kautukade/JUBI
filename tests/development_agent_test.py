from __future__ import annotations
import json, tempfile, unittest
from pathlib import Path
from types import SimpleNamespace
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sarus.core.development import DevelopmentAgent

class Bus:
    def emit(self,*a,**k): pass

class Providers:
    def __init__(self): self.calls=0
    def generate(self,prompt,**kwargs):
        self.calls += 1
        if self.calls == 1:
            return {'response': json.dumps({'summary':'fix','edits':[{'path':'calc.py','content':'def add(a,b):\n    return a + b\n'}]}), 'jubi_provider_route': {'provider':'ollama'}}
        return {'response': json.dumps({'approved': True, 'reason':'syntax and requested change are good'}), 'jubi_provider_route': {'provider':'ollama'}}

class DevelopmentAgentTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name)
        for d in ('workspace','outputs','projects','config','data'): (self.root/d).mkdir(parents=True,exist_ok=True)
        (self.root/'config/broker_allowlist.json').write_text(json.dumps({'path_scopes':{'user_workspace':['workspace','outputs','projects']}}))
        self.db=self.root/'data/test.db'
        self.app=SimpleNamespace(root=self.root,db_path=self.db,providers=Providers(),bus=Bus())
        self.agent=DevelopmentAgent(self.app)
    def tearDown(self): self.tmp.cleanup()
    def test_edit_review_verify_cycle(self):
        project=self.root/'workspace/demo'; project.mkdir(); (project/'calc.py').write_text('def add(a,b):\n    return a - b\n')
        out=self.agent.run('fix add','workspace/demo')
        self.assertEqual(out['status'],'completed',out)
        self.assertEqual(out['changed_files'],['calc.py'])
        self.assertIn('a + b',(project/'calc.py').read_text())
        self.assertTrue(out['verification']['ok'])
    def test_path_escape_is_rejected(self):
        project=self.root/'workspace/demo'; project.mkdir(); (project/'a.py').write_text('x=1\n')
        self.app.providers.calls=0
        self.app.providers.generate=lambda *a,**k: {'response':json.dumps({'edits':[{'path':'../escape.py','content':'x=2\n'}]})}
        out=self.agent.run('bad','workspace/demo')
        self.assertEqual(out['status'],'failed')
        self.assertFalse((self.root/'workspace/escape.py').exists())
    def test_failed_verification_rolls_back(self):
        project=self.root/'workspace/demo'; project.mkdir(); path=project/'calc.py'; path.write_text('x=1\n')
        calls={'n':0}
        def gen(*a,**k):
            calls['n']+=1
            if calls['n']==1: return {'response':json.dumps({'edits':[{'path':'calc.py','content':'def broken(:\n'}]})}
            return {'response':json.dumps({'approved':False,'reason':'bad'})}
        self.app.providers.generate=gen
        out=self.agent.run('break','workspace/demo')
        self.assertEqual(out['status'],'failed')
        self.assertEqual(path.read_text(),'x=1\n')

if __name__=='__main__': unittest.main(verbosity=2)
