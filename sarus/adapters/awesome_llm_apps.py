from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='awesome_llm_apps'; label='Awesome LLM Apps'; role='AI workflow and application catalog'; preferred_kinds=['doc','code','agent','skill']; task_type='general'
    def probe(self):
        ok=self.path.exists(); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native':ok})
