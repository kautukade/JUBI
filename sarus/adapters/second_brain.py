from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='second_brain'; label='Second Brain Skills'; role='knowledge, SOP, brand, document and presentation skills'; preferred_kinds=['skill','doc']; task_type='general'
    def probe(self):
        ok=self.path.exists(); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native':ok})
