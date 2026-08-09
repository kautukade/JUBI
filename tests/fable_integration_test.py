from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.fable import FableIntegration, FableLabManager, FableTraceStore
from sarus.core.receipts import ReceiptStore


class FakeRegistry:
    def __init__(self):
        self.sources = {'fable_os': 'fable'}


class FakeExecution:
    def __init__(self):
        self.calls = []

    def run(self, prompt, source='test', capability_id=None):
        self.calls.append((prompt, source, capability_id))
        return {'id': f'task-{len(self.calls)}', 'status': 'completed', 'request': prompt, 'steps': []}


class FakeApp:
    def __init__(self, root: Path):
        self.root = root
        self.registry = FakeRegistry()
        self.receipts = ReceiptStore(root / 'data/sarus.db')
        self.execution = FakeExecution()


class FableIntegrationUnit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        src = self.root / 'sources/fable'
        for d in ('core', 'tools', 'vm', 'compiler'):
            (src / d).mkdir(parents=True, exist_ok=True)
        for f in ('README.md', 'README.os.md', 'AGENTS.md', 'Makefile'):
            (src / f).write_text(f'# {f}\n', encoding='utf-8')
        (src / 'tools/a.c').write_text('/* a */', encoding='utf-8')
        (src / 'tools/b.c').write_text('/* b */', encoding='utf-8')
        (self.root / 'config').mkdir(parents=True, exist_ok=True)
        (self.root / 'config/online_sources.json').write_text(json.dumps([
            {'key': 'fable_os', 'repo': 'robiot/fable-os', 'sha': 'abc123'}
        ]), encoding='utf-8')
        self.app = FakeApp(self.root)
        self.fable = FableIntegration(self.app)

    def tearDown(self):
        self.fable.agenda.stop_evt.set()
        self.tmp.cleanup()

    def test_01_source_probe_and_pin(self):
        s = self.fable.lab.status()
        self.assertTrue(s['integrated'])
        self.assertTrue(s['source_present'])
        self.assertTrue(s['source_complete'])
        self.assertEqual(s['pinned_sha'], 'abc123')
        self.assertEqual(s['tool_source_files'], 2)
        self.assertFalse(s['arbitrary_shell'])
        self.assertFalse(s['arbitrary_qemu_args'])

    def test_02_verified_trace_is_receipted_but_imported_text_is_not_promoted(self):
        row = self.fable.traces.verified('unit.event', {'answer': 42})
        self.assertEqual(row['kind'], 'verified')
        self.assertTrue(row['receipt']['signature']['value'])
        imported = self.fable.traces.import_serial('[vfs_write -> ok]\nmodel says done')
        self.assertEqual(imported[0]['kind'], 'kernel_candidate')
        self.assertEqual(imported[1]['kind'], 'prose')
        self.assertFalse(imported[0]['receipt_id'])
        self.assertTrue(self.app.receipts.verify_chain()['ok'])

    def test_03_learned_capability_versions_and_runs_through_sarus_execution(self):
        a = self.fable.capabilities.save('Health Check', 'first', 'check system health')
        b = self.fable.capabilities.save('Health Check', 'second', 'check system health and report')
        self.assertEqual(a['version'], 1)
        self.assertEqual(b['version'], 2)
        self.assertNotEqual(a['definition_hash'], b['definition_hash'])
        out = self.fable.run_capability(b['id'])
        self.assertTrue(out['ok'])
        self.assertEqual(out['capability']['success_count'], 1)
        self.assertEqual(self.app.execution.calls[-1][1], 'fable_capability')
        self.assertEqual(out['trace']['kind'], 'verified')

    def test_04_agenda_once_runs_one_capability_then_disables(self):
        cap = self.fable.capabilities.save('One Shot', 'once', 'perform one bounded task')
        item = self.fable.agenda.add('Run once', 'once', cap['id'], max_runs=5)
        self.assertTrue(item['enabled'])
        result = self.fable.agenda.tick()
        self.assertEqual(len(result), 1)
        after = self.fable.agenda.get(item['id'])
        self.assertEqual(after['run_count'], 1)
        self.assertFalse(after['enabled'])
        self.assertEqual(after['last_status'], 'success')

    def test_05_agenda_enforces_min_period_and_active_item_limit(self):
        cap = self.fable.capabilities.save('Periodic', 'periodic', 'periodic health task')
        first = self.fable.agenda.add('periodic-0', 'every', cap['id'], period_seconds=1, max_runs=256)
        self.assertEqual(first['period_seconds'], 60)
        for i in range(1, self.fable.agenda.MAX_ITEMS):
            self.fable.agenda.add(f'periodic-{i}', 'every', cap['id'], 60, 2)
        with self.assertRaises(ValueError):
            self.fable.agenda.add('too-many', 'every', cap['id'], 60, 2)

    def test_06_lab_rejects_free_form_actions_before_execution(self):
        with self.assertRaises(ValueError):
            self.fable.lab.run_action('make anything-user-wants')
        allowed = set(self.fable.lab.status()['allowed_actions'])
        self.assertTrue({'build', 'test_host', 'test_qemu', 'test_all', 'iso', 'clean', 'start', 'stop', 'tail'}.issubset(allowed))

    def test_07_integration_status_exposes_all_four_native_layers(self):
        s = self.fable.status()
        self.assertTrue(s['integrated'])
        self.assertEqual(s['name'], 'Fable Intelligence Layer')
        self.assertIn('source', s)
        self.assertIn('learned_capabilities', s)
        self.assertIn('agenda', s)
        self.assertIn('trace', s)
        self.assertTrue(s['trace']['model_prose_is_not_proof'])
        self.assertFalse(s['direct_kernel_replacement'])


class RepositoryFableIntegration(unittest.TestCase):
    def test_20_pinned_fable_source_is_configured_and_materialized(self):
        source_cfg = json.loads((ROOT / 'config/sources.json').read_text(encoding='utf-8'))
        online_cfg = json.loads((ROOT / 'config/online_sources.json').read_text(encoding='utf-8'))
        self.assertIn('fable_os', source_cfg)
        pin = next(x for x in online_cfg if x['key'] == 'fable_os')
        self.assertEqual(pin['repo'], 'robiot/fable-os')
        self.assertEqual(pin['sha'], '1cfe17c4baa77fac128008621721823913a1335c')
        source = ROOT / 'sources' / source_cfg['fable_os']
        self.assertTrue(source.is_dir(), source)
        for required in ('README.md', 'AGENTS.md', 'README.os.md', 'Makefile', 'core', 'tools', 'vm', 'compiler'):
            self.assertTrue((source / required).exists(), required)

    def test_21_real_source_probe_does_not_require_qemu_to_prove_integration(self):
        source_cfg = json.loads((ROOT / 'config/sources.json').read_text(encoding='utf-8'))
        source = ROOT / 'sources' / source_cfg['fable_os']
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            (tmp / 'config').mkdir(parents=True)
            (tmp / 'config/online_sources.json').write_text(json.dumps([
                {'key': 'fable_os', 'repo': 'robiot/fable-os', 'sha': '1cfe17c4baa77fac128008621721823913a1335c'}
            ]), encoding='utf-8')
            receipts = ReceiptStore(tmp / 'data/sarus.db')
            traces = FableTraceStore(tmp / 'data/sarus.db', receipts)
            lab = FableLabManager(tmp, source, traces)
            status = lab.status()
            self.assertTrue(status['source_complete'])
            self.assertGreater(status['tool_source_files'], 0)
            self.assertFalse(status['arbitrary_shell'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
