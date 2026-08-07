from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
@dataclass
class AdapterStatus:
    name:str; connected:bool; path:str; details:dict
class SourceAdapter:
    name='base'; role='source'; label='Source'; preferred_kinds=['skill','agent','command','doc']; task_type='general'
    def __init__(self,path:Path): self.path=path
    def probe(self): return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':False})
    def execute(self,request:str,app,step=None,capability_id=None,context=None):
        cap=app.registry.read(capability_id) if capability_id else None
        if not cap:
            best=app.registry.best(self.name,request,self.preferred_kinds); cap=app.registry.read(best['id']) if best else None
        source_text=(cap or {}).get('content',''); system=(f"You are a SARUS specialist backed by the original {self.label} repository. " f"Role: {self.role}. Use the supplied original capability faithfully. " "Do not claim tools/actions were executed unless evidence is supplied. " "Return a concrete result for the task.\n\nORIGINAL CAPABILITY:\n"+source_text[:18000]); prompt=request
        if context: prompt+='\n\nPrevious verified pipeline context:\n'+str(context)[-8000:]
        text=app.models.generate_text(prompt,self.task_type,system); return {'ok':True,'mode':'ollama_capability','source':self.name,'capability':cap and {k:cap[k] for k in ('id','path','kind','name')},'output':text}
class PromptCatalogAdapter(SourceAdapter): pass
