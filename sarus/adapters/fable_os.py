from .base import PromptCatalogAdapter,AdapterStatus
class Adapter(PromptCatalogAdapter):
    name='fable_os'; label='Fable OS'; role='trusted action receipts and kernel/OS concepts'; preferred_kinds=['code','doc','tool']; task_type='general'
    def probe(self):
        return AdapterStatus(self.name,self.path.exists(),str(self.path),{'label':self.label,'role':self.role,'native':False,'runtime':'QEMU/bare-metal optional; SARUS uses receipt concepts natively'})
    def execute(self,request,app,step=None,capability_id=None,context=None):
        return {'ok':True,'mode':'native_sarus_receipt_layer','source':self.name,'output':'Receipt generation is handled by SARUS trusted receipt store.'}
