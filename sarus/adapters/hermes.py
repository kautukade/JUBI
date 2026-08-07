from .base import PromptCatalogAdapter,AdapterStatus
import shutil
class Adapter(PromptCatalogAdapter):
    name='hermes'; label='Hermes Agent'; role='orchestration, tools, memory, plugins and reusable skills'; preferred_kinds=['skill','tool','agent','command','doc']; task_type='general'
    def probe(self):
        ok=self.path.exists(); native=bool(shutil.which('hermes') or shutil.which('hermes-agent')); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native_cli':native})
