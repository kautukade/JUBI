import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.integrations.hermes_tool_compat import extract_text_tool_call, promote_text_tool_call


TOOLS = [{
    'type': 'function',
    'function': {
        'name': 'jubi_workspace',
        'description': 'Approved workspace',
        'parameters': {'type': 'object'},
    },
}]


class HermesTextToolCompatibilityTests(unittest.TestCase):
    def test_promotes_observed_qwen_textual_tool_call(self):
        message = {
            'role': 'assistant',
            'content': 'Let\'s start by reading the necessary files.\n\n'
                       '{"name": "jubi_workspace", "arguments": '
                       '{"operation": "read", "path": "README.md"}}',
        }
        self.assertTrue(promote_text_tool_call(message, TOOLS))
        self.assertEqual(message['tool_calls'][0]['function']['name'], 'jubi_workspace')
        self.assertEqual(message['tool_calls'][0]['function']['arguments'],
                         {'operation': 'read', 'path': 'README.md'})

    def test_promotes_observed_reviewer_read_shorthand_only_to_workspace(self):
        message = {
            'role': 'assistant',
            'content': '```json\n{"name":"read","arguments":{"path":"pricing.py"}}\n```',
        }
        self.assertTrue(promote_text_tool_call(message, TOOLS))
        function = message['tool_calls'][0]['function']
        self.assertEqual(function['name'], 'jubi_workspace')
        self.assertEqual(function['arguments'], {'operation': 'read', 'path': 'pricing.py'})

    def test_workspace_operation_alias_requires_workspace_to_be_advertised(self):
        content = '{"name":"read","arguments":{"path":"pricing.py"}}'
        other = [{'type': 'function', 'function': {'name': 'something_else', 'parameters': {}}}]
        self.assertIsNone(extract_text_tool_call(content, other))

    def test_workspace_alias_cannot_override_an_explicit_operation(self):
        content = '{"name":"read","arguments":{"operation":"write","path":"pricing.py"}}'
        self.assertIsNone(extract_text_tool_call(content, TOOLS))

    def test_unknown_tool_is_never_promoted(self):
        content = '{"name":"terminal","arguments":{"command":"whoami"}}'
        self.assertIsNone(extract_text_tool_call(content, TOOLS))

    def test_malformed_extra_keys_and_non_object_arguments_are_rejected(self):
        cases = [
            '{"name":"jubi_workspace","arguments":{},"extra":true}',
            '{"name":"jubi_workspace","arguments":"read README.md"}',
            '{"name":"jubi_workspace"}',
            'not json',
        ]
        for content in cases:
            with self.subTest(content=content):
                self.assertIsNone(extract_text_tool_call(content, TOOLS))

    def test_trailing_non_fence_text_is_rejected(self):
        content = '{"name":"jubi_workspace","arguments":{"operation":"read"}} then run it'
        self.assertIsNone(extract_text_tool_call(content, TOOLS))

    def test_existing_native_tool_call_is_not_rewritten(self):
        original = [{'function': {'name': 'jubi_workspace', 'arguments': {'operation': 'read'}}}]
        message = {'content': '{"name":"jubi_workspace","arguments":{}}',
                   'tool_calls': original}
        self.assertFalse(promote_text_tool_call(message, TOOLS))
        self.assertIs(message['tool_calls'], original)


if __name__ == '__main__':
    unittest.main(verbosity=2)
