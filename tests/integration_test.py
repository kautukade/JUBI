from pathlib import Path
import sys,json,unittest,threading,urllib.request,urllib.error
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from sarus.core.app import Sarus

class T(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.app=Sarus(ROOT); cls.orig_generate=cls.app.models.generate_text; cls.app.models.generate_text=lambda prompt,task_type='general',system='',model=None,timeout=300: f'MOCK_OK[{task_type}] '+prompt[:160]
 @classmethod
 def tearDownClass(cls): cls.app.models.generate_text=cls.orig_generate
 def test_01_all_10_sources_connected(self):
  s=self.app.status(); self.assertEqual(len(s['adapters']),10); self.assertTrue(all(a['connected'] for a in s['adapters']))
 def test_02_registry_exact_original_file_count(self):
  summary=self.app.registry.summary(); self.assertEqual(sum(x['files'] for x in summary.values()),17356); self.assertEqual(set(summary),set(json.loads((ROOT/'config/sources.json').read_text())))
 def test_03_orchestrator_cross_repo_pipeline(self):
  steps=self.app.orchestrator.execute_dry('research leads, build website, inspect screen, remember client SOP, security audit and benchmark improvement'); src={s['source'] for s in steps}; self.assertTrue({'hermes','awesome_llm_apps','agency_agents','ecc','superpowers','sara','second_brain','cai','autoresearch','fable_os'}.issubset(src))
 def test_04_real_execution_engine_all_10_adapters(self):
  r=self.app.execution.run('research leads, build website, inspect screen, remember client SOP, security audit and benchmark improvement',source='test'); self.assertEqual(r['status'],'completed'); src={x['source'] for x in r['steps']}; self.assertTrue(set(json.loads((ROOT/'config/sources.json').read_text())).issubset(src)); self.assertTrue(all(x['result'].get('ok') for x in r['steps']))
 def test_05_cai_isolation(self): self.assertEqual(self.app.policy.evaluate('active_test',2,'cai')['decision'],'isolated')
 def test_06_high_risk_approval(self): self.assertEqual(self.app.policy.evaluate('send_external_message',4,'core')['decision'],'approval')
 def test_07_never_auto_kernel(self): self.assertEqual(self.app.policy.evaluate('unbounded_kernel_access',5,'core')['decision'],'deny')
 def test_08_model_router_has_local_roles(self):
  cfg=json.loads((ROOT/'config/models.json').read_text()); self.assertIn('qwen2.5:7b',cfg['general']); self.assertIn('qwen2.5-coder:7b',cfg['coding']); self.assertIn('qwen2.5vl:3b',cfg['vision']); self.assertIn('nomic-embed-text-v2-moe:latest',cfg['embedding']); self.assertTrue(all('cloud' in x for x in cfg['cloud_disabled']))
 def test_09_capability_read_and_search(self):
  for src in json.loads((ROOT/'config/sources.json').read_text()):
   rows=self.app.registry.search('',src,None,1); self.assertTrue(rows,src); detail=self.app.registry.read(rows[0]['id']); self.assertEqual(detail['source'],src)
 def test_10_receipt_chain_content_verified(self): self.assertTrue(self.app.receipts.verify_chain()['ok'])
 def test_11_workspace_path_guard(self):
  with self.assertRaises(PermissionError): self.app.windows.action('read_file',{'path':str(ROOT.parent/'outside.txt')})

class HttpSmoke(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  import sarus.server as server; cls.server_mod=server; cls.old=server.APP.models.generate_text; server.APP.models.generate_text=lambda prompt,task_type='general',system='',model=None,timeout=300:'HTTP_MOCK_OK'; cls.httpd=server.ThreadingHTTPServer(('127.0.0.1',0),server.H); cls.port=cls.httpd.server_address[1]; cls.th=threading.Thread(target=cls.httpd.serve_forever,daemon=True); cls.th.start()
 @classmethod
 def tearDownClass(cls): cls.httpd.shutdown(); cls.httpd.server_close(); cls.server_mod.APP.models.generate_text=cls.old
 @classmethod
 def get(cls,path):
  with urllib.request.urlopen(f'http://127.0.0.1:{cls.port}{path}',timeout=10) as r:return r.status,json.load(r)
 @classmethod
 def post(cls,path,body,token=None,origin=None):
  data=json.dumps(body).encode(); h={'Content-Type':'application/json'}
  if token:h['X-SARUS-Token']=token
  if origin:h['Origin']=origin
  req=urllib.request.Request(f'http://127.0.0.1:{cls.port}{path}',data,h,method='POST')
  try:
   with urllib.request.urlopen(req,timeout=30) as r:return r.status,json.load(r)
  except urllib.error.HTTPError as e:return e.code,json.load(e)
 def test_20_get_status(self): self.assertEqual(self.get('/api/status')[0],200)
 def test_21_post_requires_session_token(self): self.assertEqual(self.post('/api/plan',{'text':'hello'})[0],403)
 def test_22_post_and_cross_origin_protection(self):
  _,sess=self.get('/api/session'); tok=sess['token']; self.assertEqual(self.post('/api/plan',{'text':'research'},tok)[0],200); self.assertEqual(self.post('/api/plan',{'text':'research'},tok,'https://evil.example')[0],403)

if __name__=='__main__': unittest.main(verbosity=2)
