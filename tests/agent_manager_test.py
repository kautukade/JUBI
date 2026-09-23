from __future__ import annotations
import json,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from sarus.core.agent_manager import AgentManager

class Brain:
    def classify(self,text,task_type='auto'): return {'task_type':'coding' if task_type=='auto' else task_type}
class Supervisor:
    def plan(self,*a,**k): return {'steps':['inspect','edit','verify']}
class Dev:
    def run(self,*a,**k): return {'status':'completed','changed_files':['a.py']}
class Research:
    def research(self,*a,**k): return {'answer':'research answer'}
class Providers:
    def generate(self,*a,**k): return {'response':'general answer','jubi_provider_route':{'provider':'ollama'}}
class Bus:
    def emit(self,*a,**k): pass

class AgentManagerTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); root=Path(self.tmp.name); (root/'data').mkdir()
        self.app=SimpleNamespace(db_path=root/'data/db.sqlite',brain=Brain(),supervisor=Supervisor(),development=Dev(),research=Research(),providers=Providers(),bus=Bus())
        self.m=AgentManager(self.app)
    def tearDown(self): self.tmp.cleanup()
    def test_coding_routes_to_real_specialist(self):
        out=self.m.run('fix code','auto','workspace/demo')
        self.assertEqual(out['status'],'completed'); self.assertEqual(out['task_type'],'coding'); self.assertEqual(out['result']['changed_files'],['a.py'])
    def test_research_routes_to_research_agent(self):
        out=self.m.run('find evidence','research')
        self.assertEqual(out['status'],'completed'); self.assertEqual(out['result']['answer'],'research answer')
    def test_general_routes_to_provider(self):
        out=self.m.run('hello','general')
        self.assertEqual(out['status'],'completed'); self.assertEqual(out['result']['answer'],'general answer')

if __name__=='__main__': unittest.main(verbosity=2)
