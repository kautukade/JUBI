"""Regression tests for the functional audit. Model doubles are explicit here."""
from pathlib import Path
import concurrent.futures
import json
import os
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from sarus.core.conversations import ConversationStore
from sarus.core.database import transaction
from sarus.core.events import EventBus
from sarus.core.workflows import WorkflowScheduler
from sarus.core.receipts import ReceiptStore
from sarus.core.windows import WindowsBroker
from sarus.core.knowledge import SemanticKnowledge
from sarus.core.network import NetworkManager
from sarus.core.research import PublicWebResearch, _public_connection
from sarus.core.council import MultiAgentSupervisor
from sarus.adapters.sara import Adapter as SaraAdapter
from jubi import updater
import jubi_provider_manager_test as provider_tests
import jubi_council_test as council_tests


class TempTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / 'test.db'


class ChatTests(TempTest):
    def make_store(self, response='Test answer'):
        self.generate = Mock(return_value={'response': response, 'model': 'test-model'})
        return ConversationStore(self.db, SimpleNamespace(generate=self.generate))

    def test_follow_up_uses_persisted_context(self):
        store = self.make_store()
        first = store.send('Remember the project is called Tulip')
        reopened = ConversationStore(self.db, store.providers)
        reopened.send('What is the name?', first['conversation_id'])
        self.assertIn('Tulip', self.generate.call_args.args[0])
        self.assertEqual(len(reopened.history(first['conversation_id'])['messages']), 4)

    def test_new_conversation_has_no_other_conversation_context(self):
        store = self.make_store()
        store.send('private note in first conversation')
        store.send('Hello again')
        self.assertEqual(self.generate.call_args.args[0], 'Hello again')
        self.assertEqual(len(store.recent()), 2)

    def test_failed_turn_does_not_save_a_false_success(self):
        store = self.make_store('')
        with self.assertRaises(RuntimeError): store.send('Hello')
        self.assertEqual(store.recent(), [])

    def test_invalid_or_unknown_id_does_not_start_new_conversation(self):
        store = self.make_store()
        with self.assertRaises(ValueError): store.send('Hello', '../../bad')
        with self.assertRaises(KeyError): store.send('Hello', '00000000-0000-0000-0000-000000000001')
        self.generate.assert_not_called()

    def test_context_is_bounded(self):
        store = self.make_store('answer' * 1000)
        cid = store.send('question' * 1000)['conversation_id']
        for _ in range(5): store.send('more' * 1000, cid)
        self.assertLess(len(self.generate.call_args.args[0]), 30000)

    def test_concurrent_messages_keep_user_assistant_pairs(self):
        store = self.make_store()
        cid = store.send('Start')['conversation_id']
        with concurrent.futures.ThreadPoolExecutor(4) as pool:
            list(pool.map(lambda i: store.send(f'Message {i}', cid), range(4)))
        self.assertEqual([m['role'] for m in store.history(cid)['messages']], ['user','assistant'] * 5)


class ProviderModeTests(unittest.TestCase):
    def setUp(self):
        self.fixture = provider_tests.ProviderManagerTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.manager = self.fixture.manager

    def test_local_only_blocks_explicit_cloud(self):
        for provider in ('nvidia', 'openrouter', 'huggingface'):
            with self.assertRaises(PermissionError): self.manager.generate('Hello', provider=provider)
        self.assertTrue(all(not p.calls for p in self.fixture.cloud.values()))

    def test_local_only_preview_never_advertises_cloud_route(self):
        self.assertEqual(self.manager.route_preview('Hello', provider='nvidia')['provider_order'], ['ollama'])

    def test_sensitive_system_context_remains_local(self):
        self.manager._set_setting('mode', 'cloud_boost')  # legacy preference cannot grant network access
        result = self.manager.generate('Summarize', system='My API key is private-credential')
        self.assertFalse(result['jubi_provider_route']['cloud'])
        self.assertTrue(all(not p.calls for p in self.fixture.cloud.values()))


class WorkflowTests(TempTest):
    def scheduler(self, result):
        bus = EventBus(self.db)
        sched = WorkflowScheduler(self.db, Mock(return_value=result), bus)
        row = sched.add('Check', 'Run check', 60)
        with transaction(self.db) as c: c.execute('UPDATE automations SET next_run=0 WHERE id=?', (row['id'],))
        return sched, bus

    def test_failed_task_is_not_recorded_as_success(self):
        sched, bus = self.scheduler({'task_id':'task1', 'status':'failed'})
        sched.tick()
        self.assertEqual(sched.list()[0]['metadata']['last_status'], 'failed')
        self.assertNotIn('AUTOMATION_FINISHED', [e['kind'] for e in bus.recent(50)])

    def test_pending_approval_pauses_recurrence(self):
        sched, bus = self.scheduler({'task_id':'task1', 'status':'waiting_approval'})
        sched.tick(); sched.tick()
        self.assertFalse(sched.list()[0]['enabled'])
        self.assertEqual(sched.runner.call_count, 1)
        self.assertEqual(sched.list()[0]['metadata']['last_task_id'], 'task1')

    def test_missing_runner_result_is_failure(self):
        sched, bus = self.scheduler(None)
        sched.tick()
        self.assertEqual(sched.list()[0]['metadata']['last_status'], 'failed')

    def test_empty_automation_is_rejected(self):
        with self.assertRaises(ValueError): WorkflowScheduler(self.db, Mock()).add('name','',60)


class PersistenceAndOperatorTests(TempTest):
    def test_receipts_are_serialized_across_instances_and_clock_changes(self):
        with patch.dict(os.environ, {'SARUS_RECEIPT_SIGNING_KEY_FILE': str(self.root/'test.key')}):
            stores = [ReceiptStore(self.db), ReceiptStore(self.db)]
        with patch('sarus.core.receipts.time.time', return_value=1700000000.0):
            with concurrent.futures.ThreadPoolExecutor(6) as pool:
                list(pool.map(lambda i: stores[i % 2].create('task', str(i), 'test', 'completed', {}), range(30)))
        stores[0].create('task', 'clock', 'test', 'completed', {})
        self.assertTrue(stores[0].verify_chain()['ok'])
        self.assertEqual(stores[0].verify_chain()['count'], 31)

    def test_move_onto_itself_preserves_file(self):
        (self.root/'config').mkdir()
        (self.root/'config/broker_allowlist.json').write_text(json.dumps({'path_scopes':{'user_workspace':['workspace']}}))
        (self.root/'workspace').mkdir()
        file = self.root/'workspace/test.txt'; file.write_text('must survive')
        broker = WindowsBroker(self.root)
        with self.assertRaises(ValueError):
            broker.execute_typed('workspace.file.move', {'source_path':'workspace/test.txt','destination_path':'workspace/test.txt','overwrite':True})
        self.assertEqual(file.read_text(), 'must survive')

    def test_missing_native_runtime_never_reports_completed_actions(self):
        adapter = SaraAdapter(self.root)
        adapter.token = ''
        result = adapter.execute('Create an app', SimpleNamespace(root=self.root), step=SimpleNamespace(agent='local-developer'))
        self.assertFalse(result['ok'])
        self.assertFalse(result['tools_executed'])


class KnowledgeAndNetworkTests(TempTest):
    def knowledge(self):
        self.models = SimpleNamespace(choose=Mock(return_value='embed-a'), embed=Mock(return_value=[1.0,0.0]))
        return SemanticKnowledge(self.db, self.models, SimpleNamespace(generate=Mock()))

    def test_different_embedding_models_are_not_compared(self):
        k = self.knowledge(); k.ingest('A local document about deployment')
        self.models.choose.return_value = 'embed-b'
        self.assertEqual(k.search('deployment'), [])

    def test_nonfinite_embedding_is_rejected_before_storage(self):
        k = self.knowledge(); self.models.embed.return_value = [float('nan'), 1]
        with self.assertRaises(RuntimeError): k.ingest('Document')
        self.assertEqual(k.documents(), [])

    def test_embedding_change_during_ingestion_rolls_back_all_chunks(self):
        k = self.knowledge(); self.models.choose.side_effect = ['embed-a','embed-b']
        with self.assertRaises(RuntimeError): k.ingest('Long document. ' * 400)
        self.assertEqual(k.documents(), [])

    def test_unreachable_service_is_not_green_health(self):
        network = NetworkManager(self.db)
        device = network.register('127.0.0.1', services=[{'name':'local-test','port':55555}])
        with patch('sarus.core.network.socket.create_connection', side_effect=OSError('refused')):
            result = network.check(device['id'])
        self.assertFalse(result['ok'])
        self.assertEqual(network.recent_observations()[0]['status'], 'error')

    def test_binary_page_is_rejected(self):
        research = PublicWebResearch(self.db, None)
        research._request = Mock(return_value=(b'%PDF file', 'application/pdf', 'https://example.com/'))
        with self.assertRaises(RuntimeError): research.fetch('https://example.com/')

    def test_dns_rebinding_is_rejected_before_connection(self):
        with patch('sarus.core.research.socket.getaddrinfo', return_value=[(2,1,6,'',('127.0.0.1',0))]), patch('sarus.core.research.socket.create_connection') as connect:
            with self.assertRaises(PermissionError): _public_connection(('example.com', 443), 1)
            connect.assert_not_called()

    def test_search_challenge_is_not_silent_zero_results(self):
        research = PublicWebResearch(self.db, None)
        research._request = Mock(return_value=(b'<div class="anomaly-modal">challenge</div>','text/html','https://example.com/'))
        with self.assertRaisesRegex(RuntimeError, 'verification'): research.search('test')


class SupervisorAndUpdateTests(TempTest):
    def supervisor(self, steps):
        providers = council_tests.FakeProviders()
        providers.generate = Mock(return_value={'response':json.dumps({'steps':steps})})
        return MultiAgentSupervisor(self.db,council_tests.FakeBrain(),providers,council_tests.FakeKnowledge(),council_tests.FakeExperience())

    def test_invalid_planner_steps_fail_cleanly(self):
        for steps in ([], [None], [{'task':'x','id':'S1','depends_on':['S1']}], [{'task':'x','id':'S1'},{'task':'y','id':'S1'}]):
            with self.assertRaises(RuntimeError): self.supervisor(steps).plan('test')

    def test_dependencies_are_ordered(self):
        sup = self.supervisor([{'id':'B','task':'second','depends_on':['A']},{'id':'A','task':'first'}])
        self.assertEqual([s['id'] for s in sup.plan('test')['plan']['steps']], ['A','B'])

    def test_powershell_bom_build_identity_is_read(self):
        path=self.root/'build-info.json'; path.write_text(json.dumps({'commit_sha':'a'*40,'build_epoch':123}), encoding='utf-8-sig')
        with patch.object(updater,'BUILD_INFO_PATH',path):
            self.assertEqual(updater._current_commit(), 'a'*40)
            self.assertEqual(updater._current_epoch(), 123)


if __name__ == '__main__': unittest.main(verbosity=2)
