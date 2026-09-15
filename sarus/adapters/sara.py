from __future__ import annotations
from .base import PromptCatalogAdapter,AdapterStatus
import json,os,urllib.request,urllib.parse
from sarus.core.provider_policy import _NoRedirect, ProviderPolicyError
class Adapter(PromptCatalogAdapter):
    name='sara'; label='SARA Local AI OS'; role='Windows UI, voice, vision, browser, local runtime'; preferred_kinds=['code','tool','doc']; task_type='general'
    def __init__(self,path):
        super().__init__(path); env={}
        for candidate in (path/'.env.local',path/'.env'):
            if candidate.exists():
                for raw in candidate.read_text(encoding='utf-8',errors='ignore').splitlines():
                    if '=' in raw and not raw.lstrip().startswith('#'): k,v=raw.split('=',1); env[k.strip()]=v.strip().strip('"').strip("'")
        port=env.get('SARA_AGENT_PORT','8765'); self.base=os.getenv('SARA_AGENT_URL',f'http://127.0.0.1:{port}').rstrip('/'); self.token=os.getenv('SARA_AGENT_TOKEN',env.get('SARA_AGENT_TOKEN','')).strip()
    def _call(self,path,body=None,timeout=10):
        # A local companion can itself forward inference to cloud. Its natural
        # language agent endpoint has no Jubi policy attestation and is disabled.
        url = urllib.parse.urlsplit(self.base)
        if (path != '/health' or body is not None or url.scheme != 'http'
                or url.hostname not in {'127.0.0.1', '::1'} or not url.port
                or url.username or url.password or url.query or url.fragment or url.path):
            raise ProviderPolicyError('Unattested SARA agent relay is disabled by Local Only')
        data=None if body is None else json.dumps(body).encode(); headers={'Content-Type':'application/json'} if data else {}
        if self.token: headers['X-SARA-Agent-Token']=self.token
        req=urllib.request.Request(self.base+path,data,headers)
        opener=urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
        with opener.open(req,timeout=timeout) as r: return json.load(r)
    def probe(self):
        online=False; detail='source-only; run native SARA installer for Windows execution'
        if self.token:
            try: self._call('/health',timeout=2); online=True; detail='SARA v7 API online'
            except Exception as e: detail='SARA API configured but offline: '+str(e)[:100]
        return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':online,'detail':detail,'api':self.base})
    def execute(self,request,app,step=None,capability_id=None,context=None):
        if step and step.agent == 'live-research':
            query = request.split('Original user request:', 1)[-1].strip()
            result = app.research.research(query)
            return {'ok': True, 'mode': 'public_web_research', 'source': self.name,
                    'tools_executed': True, 'output': result['answer'], 'evidence': result['sources']}
        err='SARA natural-language execution is disabled until its provider and tool boundaries are verified'
        if step and step.agent in {'computer','local-developer'}:
            return {'ok':False,'status':'blocked','mode':'runtime_required','source':self.name,
                    'tools_executed':False,'error':err,
                    'output':'Native SARA is required for natural-language computer/development execution. Use Computer Operator for supported typed workspace actions.'}
        out=super().execute(request,app,step,capability_id,context); out['native_fallback_reason']=err; return out
