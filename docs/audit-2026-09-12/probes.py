"""Audit reproductions with local doubles; no network or host action execution."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import json
import os
import sys
import tempfile
import uuid

ROOT=Path(__file__).resolve().parents[2]
OUT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
from sarus.core.brain import BrainRouter
from sarus.core.providers import ProviderManager
from sarus.core.experience import ExperienceEngine
from sarus.core.policy import PolicyEngine
from sarus.core.execution import ExecutionEngine
from sarus.core.orchestrator import Step
from sarus.core.privileged_broker import PrivilegedBroker
from sarus.core.receipts import ReceiptStore

findings={}
with tempfile.TemporaryDirectory(prefix='jubi-probes-') as td:
    temp=Path(td)
    with patch.dict(os.environ,{'SARUS_RECEIPT_SIGNING_KEY_FILE':str(temp/'receipt.key'),'SARUS_BROKER_APPROVAL_SECRET':'audit-only-test-secret-at-least-32-characters'}):
        class Models:
            cfg={}
            base='local-test-double'
            selected='embed-a'
            def list_models(self):
                return {'online':True,'models':['example:cloud'],'items':[{'name':'example:cloud','kind':'cloud-through-ollama'}]}
            def generate(self,*args,**kwargs): return {'model':kwargs['model'],'response':'Test response only'}
            def choose(self,*args): return self.selected
            def embed(self,*args,**kwargs): return [1.0,0.0]
        models=Models()
        credentials=SimpleNamespace(status=lambda p:{'configured':False},path=temp/'no-credentials')
        brain=BrainRouter(temp/'test.db',models,ROOT/'config/brain.json')
        providers=ProviderManager(temp/'test.db',brain,ROOT/'config/providers.json',credentials=credentials)
        response=providers.generate('Test request',model='example:cloud')
        findings['explicit_cloud_model_in_local_only']={'reproduced':response['model']=='example:cloud' and response['jubi_provider_route']['cloud'] is False,'mode':providers.mode(),'route':response['jubi_provider_route'],'note':'No real model/provider called.'}
        experience=ExperienceEngine(temp/'test.db',models)
        experience.record('apples','fruit',True)
        models.selected='embed-b'
        match=experience.similar('unrelated')[0]
        findings['experience_compares_different_embedding_models']={'reproduced':match['score']>0.9,'score':match['score'],'stored_model':'embed-a','query_model':'embed-b'}
        calls=[]
        adapter=SimpleNamespace(execute=lambda *a,**kw:(calls.append('in-process adapter invoked') or {'ok':True,'output':'Test double'}))
        policy=PolicyEngine(ROOT/'config/policy.json')
        receipts=ReceiptStore(temp/'test.db')
        app=SimpleNamespace(root=temp,db_path=temp/'test.db',bus=SimpleNamespace(emit=lambda *a:None),policy=policy,adapters=SimpleNamespace(get=lambda x:adapter),registry=SimpleNamespace(get=lambda x:None),receipts=receipts,orchestrator=SimpleNamespace(plan=lambda text:[Step('experiment','evaluation','autoresearch','benchmark',2)]))
        task=ExecutionEngine(app).run('isolated test')
        findings['isolated_policy_runs_adapter_in_process']={'reproduced':bool(calls),'policy':task['steps'][0]['policy'],'status':task['status'],'marker':task['steps'][0]['result'].get('policy_isolation'),'note':'Adapter was a harmless test double; no benchmark executed.'}
        windows=SimpleNamespace(execute_typed=lambda *a,**kw:{'ok':True,'test_double':True})
        first=PrivilegedBroker(temp,ROOT/'config/broker_allowlist.json',policy,windows,receipts)
        second=PrivilegedBroker(temp,ROOT/'config/broker_allowlist.json',policy,windows,receipts)
        request={'request_id':str(uuid.uuid4()),'nonce':uuid.uuid4().hex,'action_id':'system.processes.list','parameters':{}}
        a=first.handle(request);b=second.handle(request)
        findings['broker_replay_state_lost_on_recreation']={'reproduced':a['ok'] and b['ok'],'first':a['status'],'second':b['status'],'note':'No process list command executed; harmless executor double.'}
(OUT/'probe-results.json').write_text(json.dumps(findings,indent=2),encoding='utf-8')
print(json.dumps(findings,indent=2))
