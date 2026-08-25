from pathlib import Path

from .events import EventBus
from .models import OllamaRouter
from .brain import BrainRouter
from .providers import ProviderManager
from .knowledge import SemanticKnowledge
from .experience import ExperienceEngine
from .council import AICouncil, MultiAgentSupervisor
from .research import PublicWebResearch
from .network import NetworkManager
from .vision import VisionEngine
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
    """Jubi local-first AI runtime with bounded optional web/LAN capabilities."""

    VERSION = '0.1.0'
    FOUNDATION_VERSION = 'SARUS 1.3.1'

    def __init__(self, root: Path):
        self.root = root
        self.db_path = root / 'data/sarus.db'
        self.bus = EventBus(self.db_path)
        self.models = OllamaRouter(root / 'config/models.json')
        self.brain = BrainRouter(self.db_path, self.models, root / 'config/brain.json', self.bus)
        self.providers = ProviderManager(self.db_path, self.brain, root / 'config/providers.json', self.bus)
        self.knowledge = SemanticKnowledge(self.db_path, self.models, self.providers, self.bus)
        self.experience = ExperienceEngine(self.db_path, self.models, self.bus)
        self.council = AICouncil(self.db_path, self.brain, self.providers, self.bus)
        self.supervisor = MultiAgentSupervisor(
            self.db_path, self.brain, self.providers, self.knowledge, self.experience, self.bus
        )
        self.research = PublicWebResearch(self.db_path, self.providers, self.bus)
        self.network = NetworkManager(self.db_path, self.bus)
        self.vision = VisionEngine(self.models, self.bus)
        self.policy = PolicyEngine(root / 'config/policy.json')
        self.registry = CapabilityRegistry(root, root / 'config/sources.json', root / 'data/capabilities.json')
        self.adapters = AdapterManager(root, root / 'config/sources.json')
        self.orchestrator = Orchestrator(self.bus, self.models, self.policy)
        self.memory = MemoryStore(self.db_path)
        self.receipts = ReceiptStore(self.db_path)
        self.windows = WindowsBroker(root)
        self.privileged = PrivilegedBroker(root, root / 'config/broker_allowlist.json', self.policy, self.windows, self.receipts)
        self.execution = ExecutionEngine(self)
        self.fable = FableIntegration(self)
        self.native = NativeRuntimeManager(self)
        self.doctor = Doctor(self)
        self.scheduler = WorkflowScheduler(self.db_path, self.execution.run, event_bus=self.bus)
        self.scheduler.start()
        self.bus.emit(
            'JUBI_STARTED',
            {
                'version': self.VERSION,
                'foundation': self.FOUNDATION_VERSION,
                'brain': 'advanced-local-router-v1',
                'provider_manager': 'provider-manager-v1',
                'provider_mode': self.providers.mode(),
                'semantic_knowledge': 'local-embedding-rag-v1',
                'experience_memory': 'bounded-experience-v1',
                'council': 'multi-model-council-v1',
                'supervisor': 'reasoning-supervisor-v1',
                'web_research': 'public-web-untrusted-evidence-v1',
                'network': 'authorized-lan-passive-registry-v1',
                'vision': 'local-ollama-vision-v1',
            },
        )

    def shutdown(self):
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
        models = self.models.list_models()
        provider_status = self.providers.status(validate=False)
        knowledge_status = self.knowledge.status()
        experience_stats = self.experience.stats()
        return {
            'name': 'Jubi',
            'version': self.VERSION,
            'foundation': self.FOUNDATION_VERSION,
            'runtime': 'zero-trust-broker-v1+fable-intelligence-v1+advanced-local-brain-v1+provider-manager-v1+semantic-knowledge-v1+experience-v1+council-v1+supervisor-v1+web-research-v1+authorized-lan-v1+local-vision-v1',
            'adapters': [a.__dict__ for a in ads],
            'models': models,
            'brain': {
                'mode': self.brain.cfg.get('mode', 'local-first'),
                'automatic_cloud_escalation': self.providers.mode() != 'local_only',
                'tracked_model_task_pairs': len(self.brain.performance()),
            },
            'providers': {
                'mode': provider_status['mode'],
                'local': provider_status['local'],
                'cloud': provider_status['cloud'],
            },
            'knowledge': knowledge_status,
            'experience': {
                'total': experience_stats['total'],
                'success_rate': experience_stats['success_rate'],
                'embedded': experience_stats['embedded'],
            },
            'council': {'recent_runs': len(self.council.recent(20))},
            'supervisor': {'recent_runs': len(self.supervisor.recent(20)), 'tool_execution': False},
            'research': {
                'recent_runs': len(self.research.recent(20)),
                'network_scope': 'public-http-https-only',
                'web_content_trusted': False,
            },
            'network': self.network.status(),
            'vision': self.vision.status(),
            'capabilities': self.registry.summary(),
            'receipt_chain': self.receipts.verify_chain(),
            'pending_approvals': len(self.execution.approvals()),
            'windows_broker': self.windows.available(),
            'privileged_broker': self.privileged.status(),
            'fable': self.fable.status(),
            'native_runtimes': self.native.status(),
        }


Sarus = Jubi
