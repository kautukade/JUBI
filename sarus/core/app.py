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
from .privileged_broker import PrivilegedBroker
from .doctor import Doctor
from .execution import ExecutionEngine
from .workflows import WorkflowScheduler
from .native import NativeRuntimeManager
from .fable import FableIntegration


class Jubi:
    """Jubi local AI runtime.

    Jubi v0.1.0 evolves the proven SARUS v1.3.1 foundation. Internal ``sarus``
    module paths are intentionally retained during this stabilization phase so
    installer, source-adapter and controlled-driver compatibility is not broken
    by a cosmetic package rename.
    """

    VERSION = '0.1.0'
    FOUNDATION_VERSION = 'SARUS 1.3.1'

    def __init__(self, root: Path):
        self.root = root
        # Keep the existing database filename for Phase 0 compatibility. The
        # database schema/state is Jubi-owned from this point forward; a later
        # explicit migration can rename the physical file after target testing.
        self.db_path = root / 'data/sarus.db'
        self.bus = EventBus(self.db_path)
        self.models = OllamaRouter(root / 'config/models.json')
        self.policy = PolicyEngine(root / 'config/policy.json')
        self.registry = CapabilityRegistry(root, root / 'config/sources.json', root / 'data/capabilities.json')
        self.adapters = AdapterManager(root, root / 'config/sources.json')
        self.orchestrator = Orchestrator(self.bus, self.models, self.policy)
        self.memory = MemoryStore(self.db_path)
        self.receipts = ReceiptStore(self.db_path)
        self.windows = WindowsBroker(root)
        self.privileged = PrivilegedBroker(
            root,
            root / 'config/broker_allowlist.json',
            self.policy,
            self.windows,
            self.receipts,
        )
        self.execution = ExecutionEngine(self)
        self.fable = FableIntegration(self)
        self.native = NativeRuntimeManager(self)
        self.doctor = Doctor(self)
        self.scheduler = WorkflowScheduler(self.db_path, self.execution.run, event_bus=self.bus)
        self.scheduler.start()
        self.bus.emit(
            'JUBI_STARTED',
            {'version': self.VERSION, 'foundation': self.FOUNDATION_VERSION},
        )

    def shutdown(self):
        """Stop Jubi background workers cleanly."""
        try:
            self.scheduler.stop()
        except Exception as exc:
            self.bus.emit('JUBI_SHUTDOWN_WARNING', {'component': 'scheduler', 'error': str(exc)[:1000]})
        try:
            agenda = getattr(getattr(self, 'fable', None), 'agenda', None)
            if agenda is not None:
                agenda.stop_evt.set()
                thread = getattr(agenda, 'thread', None)
                if thread and thread.is_alive():
                    thread.join(timeout=5)
        except Exception as exc:
            self.bus.emit('JUBI_SHUTDOWN_WARNING', {'component': 'fable_agenda', 'error': str(exc)[:1000]})
        self.bus.emit('JUBI_SHUTDOWN', {'version': self.VERSION})

    def status(self):
        ads = self.adapters.connect()
        return {
            'name': 'Jubi',
            'version': self.VERSION,
            'foundation': self.FOUNDATION_VERSION,
            'runtime': 'zero-trust-broker-v1+fable-intelligence-v1+jubi-stabilization-v1',
            'adapters': [a.__dict__ for a in ads],
            'models': self.models.list_models(),
            'capabilities': self.registry.summary(),
            'receipt_chain': self.receipts.verify_chain(),
            'pending_approvals': len(self.execution.approvals()),
            'windows_broker': self.windows.available(),
            'privileged_broker': self.privileged.status(),
            'fable': self.fable.status(),
            'native_runtimes': self.native.status(),
        }


# Backward-compatible import for the SARUS-era installer/tests while Jubi is
# being stabilized. There is one runtime implementation, not two forks.
Sarus = Jubi
