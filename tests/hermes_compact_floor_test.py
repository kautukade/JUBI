import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.provider_policy import ProviderPolicyError
from sarus.integrations.hermes_transport import _apply_jubi_compact_context_floor


class HermesCompactFloorTests(unittest.TestCase):
    def _modules(self, minimum=64000):
        package = types.ModuleType('agent')
        package.__path__ = []
        metadata = types.ModuleType('agent.model_metadata')
        metadata.MINIMUM_CONTEXT_LENGTH = minimum
        package.model_metadata = metadata
        return package, metadata

    def test_tool_worker_uses_real_8k_floor_without_faking_capacity(self):
        package, metadata = self._modules()
        with patch.dict(sys.modules, {'agent': package, 'agent.model_metadata': metadata}):
            result = _apply_jubi_compact_context_floor(8192, tool_mode=True)
        self.assertEqual(result['upstream_minimum'], 64000)
        self.assertEqual(result['effective_minimum'], 8192)
        self.assertEqual(metadata.MINIMUM_CONTEXT_LENGTH, 8192)

    def test_analysis_worker_can_use_4k_but_tool_worker_cannot(self):
        package, metadata = self._modules()
        with patch.dict(sys.modules, {'agent': package, 'agent.model_metadata': metadata}):
            result = _apply_jubi_compact_context_floor(4096, tool_mode=False)
            self.assertEqual(result['effective_minimum'], 4096)
            with self.assertRaises(ProviderPolicyError):
                _apply_jubi_compact_context_floor(4096, tool_mode=True)

    def test_never_raises_above_upstream_default(self):
        package, metadata = self._modules()
        with patch.dict(sys.modules, {'agent': package, 'agent.model_metadata': metadata}):
            result = _apply_jubi_compact_context_floor(131072, tool_mode=True)
        self.assertEqual(result['effective_minimum'], 64000)
        self.assertEqual(metadata.MINIMUM_CONTEXT_LENGTH, 64000)


if __name__ == '__main__':
    unittest.main(verbosity=2)
