from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='ecc'; label='ECC'; role='agents, skills, commands, rules and verification'; preferred_kinds=['agent','skill','command','doc']; task_type='general'
    def probe(self):
        ok=self.path.exists(); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native':ok})
