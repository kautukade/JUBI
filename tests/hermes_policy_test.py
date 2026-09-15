import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class HermesPolicyTests(unittest.TestCase):
    def test_actual_hermes_worker_nested_delegation_and_raw_network_are_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            sources = json.loads((ROOT/'config/sources.json').read_text(encoding='utf-8'))
            source = ROOT/'sources'/sources['hermes']
            (home/'config.yaml').write_text(json.dumps({
                'model': {'context_length': 131072}, 'agent': {'api_max_retries': 1},
                'delegation': {'model':'qwen:cloud','max_spawn_depth':1,'max_concurrent_children':1,
                               'max_iterations':1,'child_timeout_seconds':30},
                'memory': {'memory_enabled':False}, 'plugins': {'enabled':False}}))
            env = {k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','PATH','TEMP','TMP'}}
            env.update(HERMES_HOME=str(home), HOME=str(home), USERPROFILE=str(home),
                       APPDATA=str(home), LOCALAPPDATA=str(home), PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
            result = subprocess.run([sys.executable, str(ROOT/'tests/hermes_policy_probe.py'), str(ROOT), str(home), str(source)],
                                    cwd=home, env=env, capture_output=True, text=True, encoding='utf-8', timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr[-5000:])
            data = json.loads((home/'result.json').read_text())
            self.assertEqual(data['approved_command_exit'], 7)
            self.assertTrue(data['modified_command_denied'])
            self.assertTrue(data['explicit_worker_cloud_blocked'])
            self.assertTrue(data['direct_network_denied'])
            self.assertIn('depth limit', json.dumps(data['nested_result']).lower())
            self.assertIn('Local Only', json.dumps(data['child_result']))
            self.assertNotIn('"status": "completed"', json.dumps(data['child_result']))


if __name__ == '__main__':
    unittest.main(verbosity=2)
