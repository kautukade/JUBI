from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='cai'; label='CAI'; role='isolated defensive cybersecurity analysis'; preferred_kinds=['agent','tool','doc']; task_type='general'
    def probe(self):
        return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':False,'isolation':'required'})
    def execute(self,request,app,step=None,capability_id=None,context=None):
        safe='Perform defensive, read-only cybersecurity analysis only. Do not run exploitation, persistence, credential theft, destructive, or lateral-movement actions. '+request
        out=super().execute(safe,app,step,capability_id,context); out['isolation']='analysis-only'; return out
