import sys
import unittest
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sarus.core.hardware import GIB, admission, memory_snapshot, _command


class HardwareTests(unittest.TestCase):
    def test_unknown_or_pressure_denies_admission(self):
        self.assertFalse(admission(None, {})['admitted'])
        self.assertFalse(admission(4 * GIB, {'available_bytes': 3 * GIB})['admitted'])

    def test_concurrency_is_bounded_by_ram_and_product_cap(self):
        self.assertEqual(admission(4 * GIB, {'available_bytes': 8 * GIB}, 12)['max_workers'], 1)
        result = admission(4 * GIB, {'available_bytes': 12 * GIB}, 12)
        self.assertLessEqual(result['max_workers'] * result['estimated_worker_bytes'], result['budget_bytes'])
        self.assertEqual(admission(4 * GIB, {'available_bytes': 64 * GIB}, 12)['max_workers'], 3)

    def test_paging_requires_explicit_opt_in_commit_and_single_worker(self):
        memory = {'available_bytes': 3 * GIB, 'total_bytes': 16 * GIB, 'available_commit_bytes': 12 * GIB}
        self.assertFalse(admission(4 * GIB, memory)['admitted'])
        self.assertEqual(admission(4 * GIB, memory, 12, allow_cpu_paging=True)['max_workers'], 1)
        memory['available_commit_bytes'] = 4 * GIB
        self.assertFalse(admission(4 * GIB, memory, allow_cpu_paging=True)['admitted'])

    def test_missing_probe_and_windows_utf16_output_degrade_gracefully(self):
        with patch('sarus.core.hardware.subprocess.run', side_effect=FileNotFoundError('missing')):
            self.assertEqual(_command(['missing'])['status'], 'FAILED')
        response = SimpleNamespace(returncode=0, stdout='WSL available'.encode('utf-16-le'), stderr=b'')
        with patch('sarus.core.hardware.subprocess.run', return_value=response):
            self.assertEqual(_command(['wsl'])['value'], 'WSL available')

    def test_live_memory_snapshot_is_consistent_when_available(self):
        info = memory_snapshot()
        if info['status'] == 'AVAILABLE':
            self.assertGreater(info['total_bytes'], 0)
            self.assertLessEqual(info['available_bytes'], info['total_bytes'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
