from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'sarus' / 'web'

PAGES = {
    'index.html': 'overview',
    'chat.html': 'chat',
    'tasks.html': 'tasks',
    'models.html': 'models',
    'agents.html': 'agents',
    'development.html': 'development',
    'knowledge.html': 'knowledge',
    'fable.html': 'fable',
    'automation.html': 'automation',
    'computer.html': 'computer',
    'security.html': 'security',
    'health.html': 'health',
    'activity.html': 'activity',
}

REQUIRED_IDS = {
    'overview': {'metric-sources', 'metric-models', 'metric-units', 'metric-approvals', 'metric-files', 'metric-chain', 'sources-list', 'recent-tasks', 'recent-events'},
    'chat': {'chat-messages', 'chat-input', 'chat-model', 'chat-type', 'chat-send'},
    'tasks': {'task-input', 'task-plan', 'task-run', 'task-output', 'tasks-table', 'task-approvals'},
    'models': {'model-count', 'model-online', 'model-table', 'model-select', 'model-test'},
    'agents': {'agent-sources', 'cap-query', 'cap-source', 'cap-list', 'cap-detail', 'cap-run-btn'},
    'development': {'dev-input', 'dev-plan', 'dev-run', 'dev-output', 'dev-history'},
    'knowledge': {'memory-title', 'memory-ns', 'memory-content', 'memory-save', 'memory-q', 'memory-results'},
    'fable': {'fable-source', 'fable-runtime', 'fable-caps-count', 'fable-agenda-count', 'fable-cap-list', 'fable-agenda-list', 'fable-traces', 'fable-tail'},
    'automation': {'automation-name', 'automation-interval', 'automation-prompt', 'automation-create', 'automation-list'},
    'computer': {'broker-actions', 'proc-btn', 'svc-btn', 'ring-ping', 'ring-status', 'file-path', 'file-read', 'file-write', 'url-open'},
    'security': {'security-approvals-count', 'security-chain', 'security-secret', 'security-approvals', 'receipts-table', 'broker-posture'},
    'health': {'doctor-run', 'doctor-grid', 'doctor-raw'},
    'activity': {'activity-count', 'activity-refresh', 'activity-filter', 'activity-list'},
}


class UnifiedDashboardTests(unittest.TestCase):
    def test_all_feature_pages_exist_and_use_one_design_system(self):
        self.assertTrue((WEB / 'assets/styles.css').is_file())
        self.assertTrue((WEB / 'assets/app.js').is_file())
        for filename, page in PAGES.items():
            path = WEB / filename
            self.assertTrue(path.is_file(), filename)
            text = path.read_text(encoding='utf-8')
            self.assertIn(f'data-page="{page}"', text, filename)
            self.assertIn('/assets/styles.css', text, filename)
            self.assertIn('/assets/app.js', text, filename)
            self.assertNotIn('SARUS R&D', text, filename)
            self.assertNotIn('data-view=', text, filename)
            self.assertNotRegex(text, r'<script[^>]+src=["\']https?://', filename)
            self.assertNotRegex(text, r'<link[^>]+href=["\']https?://', filename)

    def test_page_controls_match_client_runtime(self):
        for filename, page in PAGES.items():
            text = (WEB / filename).read_text(encoding='utf-8')
            ids = set(re.findall(r'id="([^"]+)"', text))
            missing = REQUIRED_IDS[page] - ids
            self.assertEqual(missing, set(), f'{filename} missing controls: {sorted(missing)}')

    def test_client_is_real_api_wired_and_not_placeholder_navigation(self):
        js = (WEB / 'assets/app.js').read_text(encoding='utf-8')
        for endpoint in (
            '/api/status', '/api/models', '/api/chat', '/api/plan', '/api/task',
            '/api/tasks', '/api/capabilities', '/api/capability/run', '/api/memory',
            '/api/automations', '/api/automation/toggle', '/api/system/action',
            '/api/approvals', '/api/approval', '/api/receipts', '/api/broker',
            '/api/doctor', '/api/events', '/api/fable', '/api/fable/lab',
            '/api/fable/capability/save', '/api/fable/agenda/add',
        ):
            self.assertIn(endpoint, js)
        self.assertIn('X-JUBI-Token', js)
        self.assertNotIn('example response', js.lower())
        self.assertNotIn('fake response', js.lower())

    def test_navigation_covers_every_feature_page(self):
        js = (WEB / 'assets/app.js').read_text(encoding='utf-8')
        for filename in PAGES:
            if filename == 'index.html':
                continue
            self.assertIn('/' + filename, js, filename)

    def test_server_still_serves_unified_web_root(self):
        server = (ROOT / 'sarus/server.py').read_text(encoding='utf-8')
        self.assertIn("directory=str(ROOT / 'sarus/web')", server)
        self.assertIn("'X-JUBI-Token'", server)


if __name__ == '__main__':
    unittest.main(verbosity=2)
