from .base import PromptCatalogAdapter, AdapterStatus
import shutil


class Adapter(PromptCatalogAdapter):
    name = 'hermes'
    label = 'Hermes Agent'
    role = 'orchestration, tools, memory, plugins and reusable skills'
    preferred_kinds = ['skill', 'tool', 'agent', 'command', 'doc']
    task_type = 'general'

    def probe(self):
        ok = self.path.exists()
        native = bool(shutil.which('hermes') or shutil.which('hermes-agent'))
        return AdapterStatus(
            self.name, ok, str(self.path),
            {'label': self.label, 'role': self.role, 'native_cli': native},
        )

    def execute(self, request, app, step=None, capability_id=None, context=None):
        if step and step.agent == 'local-developer':
            original = request.split('Original user request:', 1)[-1].strip()
            project = '.'
            marker = '[project='
            if marker in original:
                tail = original.split(marker, 1)[1]
                project = tail.split(']', 1)[0].strip() or '.'
            result = app.developer.run(original, project_path=project)
            result['source'] = self.name
            return result
        return super().execute(request, app, step, capability_id, context)
