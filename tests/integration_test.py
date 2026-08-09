from pathlib import Path
import sys,json,unittest,threading,urllib.request,urllib.error,time,uuid
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
SAFE_FILE=ROOT/'workspace'/'.integration-broker-read.txt'
def safe_file():
 SAFE_FILE.parent.mkdir(parents=True,exist_ok=True); SAFE_FILE.write_text('SARUS integration workspace test',encoding='utf-8'); return SAFE_FILE
from sarus.core.app import Sarus

class T(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.app=Sarus(ROOT); cls.orig_generate=cls.app.models.generate_text; cls.app.models.generate_text=lambda prompt,task_type='general',system='',model=None,timeout=300: f'MOCK_OK[{task_type}] '+prompt[:160]
 @classmethod
 def tearDownClass(cls): cls.app.models.generate_text=cls.orig_generate; SAFE_FILE.unlink(missing_ok=True)
 def test_01_all_10_sources_connected(self):
  s=self.app.status(); self.assertEqual(len(s['adapters']),10); self.assertTrue(all(a['connected'] for a in s['adapters']))
 def test_02_registry_exact_original_file_count(self):
  summary=self.app.registry.summary(); manifest=json.loads((ROOT/'BUILD_MANIFEST.json').read_text()); self.assertEqual(sum(x['files'] for x in summary.values()),manifest['indexed_original_files']); self.assertEqual(len(summary),manifest['source_repositories']); self.assertEqual(set(summary),set(json.loads((ROOT/'config/sources.json').read_text())))
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
 def test_12_legacy_powershell_is_blocked_even_if_approved(self):
  with self.assertRaises(PermissionError): self.app.windows.action('powershell',{'command':'Write-Host unsafe'},approved=True)
 def test_13_unknown_broker_action_default_denied_and_receipted(self):
  r=self.app.privileged.handle({'action_id':'not.allowlisted','parameters':{}}); self.assertFalse(r['ok']); self.assertEqual(r['status'],'denied'); self.assertTrue(r['receipt']['signature']['value'])
 def test_14_allowlisted_workspace_read(self):
  r=self.app.privileged.handle({'action_id':'workspace.file.read','parameters':{'path':str(safe_file())}}); self.assertTrue(r['ok']); self.assertIn('SARUS',r['result']['content']); self.assertTrue(r['receipt']['signature']['value'])
 def test_15_high_risk_broker_action_needs_out_of_band_approval(self):
  r=self.app.privileged.handle({'action_id':'process.stop','parameters':{'resource_id':'ollama'}}); self.assertFalse(r['ok']); self.assertEqual(r['status'],'approval_required')
 def test_16_kernel_action_is_permanently_denied(self):
  r=self.app.privileged.handle({'action_id':'kernel.read_memory','parameters':{}}); self.assertEqual(r['status'],'denied')
 def test_17_replay_request_is_denied(self):
  rid=str(uuid.uuid4()); nonce='replay-'+uuid.uuid4().hex; req={'request_id':rid,'nonce':nonce,'timestamp':time.time(),'action_id':'workspace.file.read','parameters':{'path':str(safe_file())}}; a=self.app.privileged.handle(req); b=self.app.privileged.handle(req); self.assertTrue(a['ok']); self.assertFalse(b['ok']); self.assertEqual(b['status'],'denied')
 def test_18_receipt_signatures_verify(self):
  v=self.app.receipts.verify_chain(); self.assertTrue(v['ok']); self.assertGreater(v['signed_count'],0); self.assertEqual(v['algorithm'],'HMAC-SHA256')

class HttpSmoke(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  import sarus.server as server; cls.server_mod=server; cls.old=server.APP.models.generate_text; server.APP.models.generate_text=lambda prompt,task_type='general',system='',model=None,timeout=300:'HTTP_MOCK_OK'; cls.httpd=server.ThreadingHTTPServer(('127.0.0.1',0),server.H); cls.port=cls.httpd.server_address[1]; cls.th=threading.Thread(target=cls.httpd.serve_forever,daemon=True); cls.th.start()
 @classmethod
 def tearDownClass(cls): cls.httpd.shutdown(); cls.httpd.server_close(); cls.server_mod.APP.models.generate_text=cls.old; SAFE_FILE.unlink(missing_ok=True)
 @classmethod
 def get(cls,path):
  with urllib.request.urlopen(f'http://127.0.0.1:{cls.port}{path}',timeout=10) as r:return r.status,json.load(r)
 @classmethod
 def post(cls,path,body,token=None,origin=None,approval=None):
  data=json.dumps(body).encode(); h={'Content-Type':'application/json'}
  if token:h['X-SARUS-Token']=token
  if origin:h['Origin']=origin
  if approval:h['X-SARUS-Approval']=approval
  req=urllib.request.Request(f'http://127.0.0.1:{cls.port}{path}',data,h,method='POST')
  try:
   with urllib.request.urlopen(req,timeout=30) as r:return r.status,json.load(r)
  except urllib.error.HTTPError as e:return e.code,json.load(e)
 def test_20_get_status(self): self.assertEqual(self.get('/api/status')[0],200)
 def test_21_post_requires_session_token(self): self.assertEqual(self.post('/api/plan',{'text':'hello'})[0],403)
 def test_22_post_and_cross_origin_protection(self):
  _,sess=self.get('/api/session'); tok=sess['token']; self.assertEqual(self.post('/api/plan',{'text':'research'},tok)[0],200); self.assertEqual(self.post('/api/plan',{'text':'research'},tok,'https://evil.example')[0],403)
 def test_23_broker_status_endpoint(self):
  code,b=self.get('/api/broker'); self.assertEqual(code,200); self.assertEqual(b['default'],'deny'); self.assertFalse(b['arbitrary_shell']); self.assertFalse(b['kernel_direct_access'])
 def test_24_system_action_uses_typed_request(self):
  _,sess=self.get('/api/session'); tok=sess['token']; code,b=self.post('/api/system/action',{'action_id':'workspace.file.read','parameters':{'path':str(safe_file())}},tok); self.assertEqual(code,200); self.assertTrue(b['ok']); self.assertTrue(b['receipt']['signature']['value'])
 def test_25_legacy_system_action_shape_rejected(self):
  _,sess=self.get('/api/session'); tok=sess['token']; code,b=self.post('/api/system/action',{'name':'powershell','args':{'command':'whoami'},'approved':True},tok); self.assertEqual(code,400); self.assertEqual(b['status'],'invalid')

if __name__=='__main__': unittest.main(verbosity=2)
