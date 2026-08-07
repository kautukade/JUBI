from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='agency_agents'; label='Agency Agents'; role='specialist persona catalog'; preferred_kinds=['agent','skill','doc']; task_type='general'
    def probe(self):
        ok=self.path.exists(); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native':ok})
