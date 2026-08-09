from .base import PromptCatalogAdapter, AdapterStatus


class Adapter(PromptCatalogAdapter):
    name = 'fable_os'
    label = 'Fable OS'
    role = 'verified execution, persistent capabilities, bounded autonomy and isolated OS research'
    preferred_kinds = ['tool', 'code', 'doc']
    task_type = 'general'

    def probe(self):
        basic = {
            'label': self.label,
            'role': self.role,
            'native': True,
            'runtime': 'SARUS Fable Intelligence Layer + isolated QEMU/bare-metal research source',
        }
        return AdapterStatus(self.name, self.path.exists(), str(self.path), basic)

    def execute(self, request, app, step=None, capability_id=None, context=None):
        request = str(request or '')
        integration = app.fable.status()
        source_cap = None
        if capability_id:
            source_cap = app.registry.read(capability_id)
        else:
            best = app.registry.best(self.name, request, self.preferred_kinds)
            source_cap = app.registry.read(best['id']) if best else None

        # Fable is both a real managed lab and a source of architecture context.
        # The model is never allowed to claim a lab command ran unless a SARUS
        # verified trace/receipt exists. Runtime actions are exposed by dedicated
        # /api/fable endpoints rather than generated free-form commands.
        source_text = (source_cap or {}).get('content', '')[:18000]
        system = (
            'You are the Fable specialist inside SARUS. Use the supplied original '
            'Fable source material for architecture reasoning and the SARUS Fable '
            'status for current runtime facts. Distinguish model explanation from '
            'verified execution evidence. Never claim QEMU, make, kernel, device or '
            'test execution occurred unless the provided integration status/trace says so. '
            'Do not invent unrestricted kernel primitives.\n\n'
            f'CURRENT SARUS FABLE STATUS:\n{integration}\n\n'
            f'ORIGINAL FABLE CAPABILITY/SOURCE EXCERPT:\n{source_text}'
        )
        prompt = request
        if context:
            prompt += '\n\nPrevious verified pipeline context:\n' + str(context)[-8000:]
        text = app.models.generate_text(prompt, self.task_type, system)
        return {
            'ok': True,
            'mode': 'native_fable_intelligence',
            'source': self.name,
            'integration': integration,
            'capability': source_cap and {k: source_cap[k] for k in ('id', 'path', 'kind', 'name')},
            'output': text,
        }
