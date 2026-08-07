from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='superpowers'; label='Superpowers'; role='coding process, TDD, debugging, review and verification'; preferred_kinds=['skill','doc']; task_type='coding'
    def probe(self):
        ok=self.path.exists(); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native':ok})
