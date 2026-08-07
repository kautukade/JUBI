from pathlib import Path
from .events import EventBus
from .models import OllamaRouter
from .policy import PolicyEngine
from .capabilities import CapabilityRegistry
from .adapters import AdapterManager
from .orchestrator import Orchestrator
from .memory import MemoryStore
from .receipts import ReceiptStore
from .windows import WindowsBroker
from .doctor import Doctor
from .execution import ExecutionEngine
from .workflows import WorkflowScheduler
from .native import NativeRuntimeManager

class Sarus:
    def __init__(self,root:Path):
        self.root=root
        self.bus=EventBus(root/'data/sarus.db')
        self.models=OllamaRouter(root/'config/models.json')
        self.policy=PolicyEngine(root/'config/policy.json')
        self.registry=CapabilityRegistry(root,root/'config/sources.json',root/'data/capabilities.json')
        self.adapters=AdapterManager(root,root/'config/sources.json')
        self.orchestrator=Orchestrator(self.bus,self.models,self.policy)
        self.memory=MemoryStore(root/'data/sarus.db')
        self.receipts=ReceiptStore(root/'data/sarus.db')
        self.windows=WindowsBroker(root)
        self.execution=ExecutionEngine(self)
        self.native=NativeRuntimeManager(self)
        self.doctor=Doctor(self)
        self.scheduler=WorkflowScheduler(root/'data/sarus.db',self.execution.run)
        self.scheduler.start()

    def status(self):
        ads=self.adapters.connect()
        return {
            'name':'SARUS','version':'1.0.0','runtime':'executable-adapter-v1',
            'adapters':[a.__dict__ for a in ads],
            'models':self.models.list_models(),
            'capabilities':self.registry.summary(),
            'receipt_chain':self.receipts.verify_chain(),
            'pending_approvals':len(self.execution.approvals()),
            'windows_broker':self.windows.available(),
            'native_runtimes':self.native.status(),
        }
