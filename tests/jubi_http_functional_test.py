from pathlib import Path
import http.client
import json
import sys
import unittest
import os
import shutil
from types import SimpleNamespace
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from runtime_fixture import RuntimeFixture

class HttpFunctionalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.fixture=RuntimeFixture().__enter__()
    @classmethod
    def tearDownClass(cls): cls.fixture.__exit__(None,None,None)
    def request(self,path,body=None,headers=None,raw=None,method=None):
        c=http.client.HTTPConnection('127.0.0.1',self.fixture.http.server_port,timeout=10)
        h={'Content-Type':'application/json','X-JUBI-Token':self.fixture.server.SESSION_TOKEN,**(headers or {})}
        c.request(method or ('POST' if body is not None or raw is not None else 'GET'),path,
                  raw if raw is not None else json.dumps(body).encode() if body is not None else None,h)
        r=c.getresponse();data=r.read();status=r.status;rh=dict(r.getheaders());c.close()
        return status,json.loads(data) if data and 'application/json' in rh.get('Content-Type','') else data,rh

    def test_all_dashboard_pages_and_assets_load(self):
        for p in (ROOT/'sarus/web').glob('*.html'):
            status,body,headers=self.request('/'+p.name)
            self.assertEqual(status,200,p.name)
            self.assertEqual(headers.get('X-Frame-Options'),'DENY')
            self.assertIn('frame-ancestors',headers.get('Content-Security-Policy',''))
        for p in (ROOT/'sarus/web/assets').glob('*.js'): self.assertEqual(self.request('/assets/'+p.name)[0],200)

    def test_all_read_only_feature_endpoints(self):
        endpoints=['health','status','brain','brain/decisions','brain/performance','council','supervisor','research','network',
                   'network/devices','network/observations','vision','browser','providers','providers/performance','providers/requests',
                   'knowledge/status','knowledge/documents','experience','experience/stats','broker','doctor','events',
                   'models','capabilities','tasks','approvals','receipts','memory','automations','fable','fable/traces',
                   'fable/capabilities','fable/agenda','fable/lab/tail','conversations']
        for p in endpoints:
            result=self.request('/api/'+p)
            self.assertEqual(result[0],200,(p,result[1]))

    def test_host_origin_and_session_checks(self):
        for headers in ({'Host':'attacker.example'}, {'Origin':'https://attacker.example'}, {'Sec-Fetch-Site':'cross-site'}):
            self.assertEqual(self.request('/api/session',headers=headers)[0],403)
        self.assertEqual(self.request('/api/plan',{'text':'Hello'},{'X-JUBI-Token':'expired'})[1]['code'],'session_expired')
        self.assertEqual(self.request('/',headers={'Host':'attacker.example'},method='HEAD')[0],403)

    def test_bind_only_loopback(self):
        from sarus.server import loopback_host
        for host in ('0.0.0.0','192.168.0.2','::','example.com'):
            with self.assertRaises(RuntimeError):loopback_host(host)
        self.assertEqual(loopback_host('localhost'),'127.0.0.1')

    def test_malformed_bodies_are_client_errors(self):
        for raw in ('[]','null','42','"hello"','{bad','{"enabled":"false"}','{"text":null}','{"timeout":NaN}','{"timeout":1e309}'):
            self.assertEqual(self.request('/api/automation',raw=raw.encode())[0],400,raw)
        self.assertEqual(self.request('/api/unknown')[0],404)

    def test_chat_round_trip_persists_and_passes_context_to_ollama(self):
        status,first,_=self.request('/api/chat',{'text':'Project name is Lotus','provider':'ollama'})
        self.assertEqual(status,200,first)
        status,second,_=self.request('/api/chat',{'text':'What is the project name?','provider':'ollama','conversation_id':first['conversation_id']})
        self.assertEqual(status,200,second)
        generations=[b for p,b in self.fixture.model_requests if p=='/api/generate']
        self.assertIn('Lotus',generations[-1]['prompt'])
        self.assertEqual(len(self.request('/api/chat/history?id='+first['conversation_id'])[1]['messages']),4)

    def test_actual_workspace_lifecycle_and_signed_delete(self):
        def action(name,params,**extra):return self.request('/api/system/action',{'action_id':name,'parameters':params,**extra})
        self.assertTrue(action('workspace.file.write',{'path':'workspace/actual.txt','content':'Actual disk content'})[1]['ok'])
        self.assertEqual((self.fixture.root/'workspace/actual.txt').read_text(),'Actual disk content')
        self.assertEqual(action('workspace.file.read',{'path':'workspace/actual.txt'})[1]['result']['content'],'Actual disk content')
        status,pending,_=action('workspace.file.delete',{'path':'workspace/actual.txt'})
        self.assertEqual(status,423);self.assertTrue((self.fixture.root/'workspace/actual.txt').exists())
        request={'request_id':pending['request_id'],'action_id':'workspace.file.delete','parameters':{'path':'workspace/actual.txt'}}
        proof=self.fixture.app.privileged.create_approval_proof(request['request_id'],request['action_id'],request['parameters'])
        status,result,_=self.request('/api/system/action',request,{'X-JUBI-Approval':proof})
        self.assertTrue(result['ok'],result);self.assertFalse((self.fixture.root/'workspace/actual.txt').exists())
        self.assertEqual(self.request('/api/system/action',request,{'X-JUBI-Approval':proof})[0],403)

    def test_missing_file_has_failure_receipt(self):
        status,result,_=self.request('/api/system/action',{'action_id':'workspace.file.read','parameters':{'path':'workspace/does-not-exist'}})
        self.assertFalse(result['ok']);self.assertEqual(result['status'],'failed');self.assertTrue(result['receipt']['signature'])

    def test_knowledge_memory_and_task_details(self):
        self.assertEqual(self.request('/api/memory',{'title':'Test note','content':'persistent plain memory'})[0],200)
        self.assertTrue(self.request('/api/memory?q=persistent')[1])
        status,doc,_=self.request('/api/knowledge/ingest',{'content':'Document for semantic test','title':'Test document'})
        self.assertEqual(status,200,doc)
        self.assertTrue(self.request('/api/knowledge/search',{'query':'Document'})[1])
        self.assertEqual(self.request('/api/knowledge/delete',{'id':doc['id']})[0],200)
        status,task,_=self.request('/api/task',{'text':'Give a concise greeting'})
        self.assertEqual(status,200,task)
        detail=self.request('/api/task?id='+task['task_id'])[1]
        self.assertEqual(detail['status'],'completed',detail)

    def test_core_certification_does_not_weaken_full_native_requirements(self):
        import sarus.acceptance as acceptance
        production_path = self.fixture.root/'config/production.json'
        original = production_path.read_text()
        production = json.loads(original)
        production['require_sara_on_windows'] = True  # explicit native certification still must fail without SARA
        production_path.write_text(json.dumps(production))
        self.addCleanup(production_path.write_text, original)
        shutil.copyfile(ROOT/'BUILD_MANIFEST.json', self.fixture.root/'BUILD_MANIFEST.json')
        # Simulate only acceptance's platform selection; the native runtime is
        # deliberately absent. Keep the real Windows APIs outside this test.
        with patch.object(acceptance,'Jubi',return_value=self.fixture.app), \
             patch.object(acceptance,'os',SimpleNamespace(name='nt',environ=os.environ)), \
             patch.object(self.fixture.app,'shutdown'):
            full=acceptance.run_acceptance(self.fixture.root)
            core=acceptance.run_acceptance(self.fixture.root,core_only=True)
        full_checks={c['name']:c for c in full['checks']}
        core_checks={c['name']:c for c in core['checks']}
        self.assertEqual(full['profile'],'full');self.assertEqual(core['profile'],'core')
        native='SARA v7 native API bridge'
        self.assertTrue(full_checks[native]['required'])
        self.assertFalse(full_checks[native]['ok'])
        self.assertFalse(core_checks[native]['required'])
        self.assertFalse(core_checks[native]['ok'])
        for name in full_checks.keys()-{native}:
            self.assertEqual(core_checks[name]['required'],full_checks[name]['required'],name)

if __name__=='__main__':unittest.main(verbosity=2)
