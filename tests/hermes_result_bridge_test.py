import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.integrations.hermes_transport import _normalize_jubi_structured_tool_result


class HermesResultBridgeTests(unittest.TestCase):
    def test_plain_jubi_dict_becomes_json_string(self):
        seen = []

        def original(name, result):
            seen.append((name, result))
            return 'ORIGINAL'

        value = {'content': 'hello', 'exit_code': 0}
        result = _normalize_jubi_structured_tool_result(original, 'jubi_workspace', value)
        self.assertIsInstance(result, str)
        self.assertEqual(json.loads(result), value)
        self.assertEqual(seen, [])

    def test_existing_string_contract_is_unchanged(self):
        def original(name, result):
            return result

        self.assertEqual(
            _normalize_jubi_structured_tool_result(original, 'jubi_workspace', 'ok'),
            'ok',
        )

    def test_multimodal_envelope_stays_with_hermes_contract(self):
        marker = object()

        def original(name, result):
            self.assertEqual(name, 'jubi_workspace')
            self.assertTrue(result['_multimodal'])
            return marker

        envelope = {'_multimodal': True, 'content': []}
        self.assertIs(
            _normalize_jubi_structured_tool_result(original, 'jubi_workspace', envelope),
            marker,
        )


if __name__ == '__main__':
    unittest.main(verbosity=2)
