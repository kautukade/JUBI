from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='autoresearch'; label='Autoresearch'; role='isolated measurable evaluation and improvement lab'; preferred_kinds=['code','doc']; task_type='coding'
    def probe(self):
        return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':False,'isolation':'required','gpu':'optional'})
    def execute(self,request,app,step=None,capability_id=None,context=None):
        out=super().execute('Design a bounded experiment; do not modify production. '+request,app,step,capability_id,context); out['isolation']='proposal-only-until-accepted'; return out
