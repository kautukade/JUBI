from __future__ import annotations
from .base import PromptCatalogAdapter,AdapterStatus
import json,os,urllib.request
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
        data=None if body is None else json.dumps(body).encode(); headers={'Content-Type':'application/json'} if data else {}
        if self.token: headers['X-SARA-Agent-Token']=self.token
        req=urllib.request.Request(self.base+path,data,headers)
        with urllib.request.urlopen(req,timeout=timeout) as r: return json.load(r)
    def probe(self):
        online=False; detail='source-only; run native SARA installer for Windows execution'
        if self.token:
            try: self._call('/health',timeout=2); online=True; detail='SARA v7 API online'
            except Exception as e: detail='SARA API configured but offline: '+str(e)[:100]
        return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':online,'detail':detail,'api':self.base})
    def execute(self,request,app,step=None,capability_id=None,context=None):
        if self.token:
            try:
                out=self._call('/v7/command',{'mode':'agent','command':request,'language':'hinglish','workspace':str(app.root),'auto_execute':True},300); return {'ok':True,'mode':'sara_v7_api','source':self.name,'output':out,'evidence':{'api':self.base}}
            except Exception as e: err=str(e)
        else: err='SARA_AGENT_TOKEN not configured yet'
        out=super().execute(request,app,step,capability_id,context); out['native_fallback_reason']=err; return out
