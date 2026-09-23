from .base import PromptCatalogAdapter,AdapterStatus
import shutil
class Adapter(PromptCatalogAdapter):
    name='hermes'; label='Hermes Agent'; role='orchestration, tools, memory, plugins and reusable skills'; preferred_kinds=['skill','tool','agent','command','doc']; task_type='general'
    def probe(self):
        ok=self.path.exists(); native=bool(shutil.which('hermes') or shutil.which('hermes-agent')); return AdapterStatus(self.name,ok,str(self.path),{'label':self.label,'role':self.role,'native_cli':native})

    def execute(self, request, app, step=None, capability_id=None, context=None):
        if step and step.agent == 'vps-developer':
            goal = request.split('Original user request:', 1)[-1].strip()
            result = app.development.run(goal)
            return {
                'ok': bool(result.get('ok')),
                'status': result.get('status', 'failed'),
                'mode': result.get('mode', 'vps_local_coding'),
                'source': self.name,
                'tools_executed': True,
                'output': result.get('output', ''),
                'evidence': result,
            }
        return super().execute(request, app, step, capability_id, context)
