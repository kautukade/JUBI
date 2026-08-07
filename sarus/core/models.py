from __future__ import annotations
import json, urllib.request
from pathlib import Path

class OllamaRouter:
    def __init__(self, config: Path, base_url='http://127.0.0.1:11434'):
        self.cfg=json.loads(config.read_text(encoding='utf-8'))
        self.base=base_url.rstrip('/')

    def _json(self, path, body=None, timeout=10):
        data=None if body is None else json.dumps(body).encode()
        req=urllib.request.Request(self.base+path,data,{'Content-Type':'application/json'} if data else {})
        with urllib.request.urlopen(req,timeout=timeout) as r:
            return json.load(r)

    def list_models(self):
        try:
            return {'online':True,'models':[m['name'] for m in self._json('/api/tags',timeout=3).get('models',[])]}
        except Exception as e:
            return {'online':False,'models':[],'error':str(e)}

    def choose(self, task_type='general'):
        installed=set(self.list_models().get('models',[]))
        candidates=self.cfg.get(task_type,self.cfg.get('general',[]))
        for m in candidates:
            if m in installed:
                return m
        return candidates[0] if candidates else None

    def generate(self,prompt:str,task_type='general',system='You are SARUS, a local AI orchestrator.',model=None,timeout=300):
        model=model or self.choose(task_type)
        if not model:
            raise RuntimeError('No model configured')
        return self._json('/api/generate',{'model':model,'prompt':prompt,'system':system,'stream':False},timeout)

    def generate_text(self,*a,**kw):
        return str(self.generate(*a,**kw).get('response','')).strip()

    def embed(self,text,model=None):
        model=model or self.choose('embedding')
        d=self._json('/api/embed',{'model':model,'input':text},120)
        return (d.get('embeddings') or [None])[0]
