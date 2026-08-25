from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'sarus' / 'web'


class ResearchUITests(unittest.TestCase):
    def test_research_page_is_real_and_linked_from_agents(self):
        page = (WEB / 'research.html').read_text(encoding='utf-8')
        agents = (WEB / 'agents.html').read_text(encoding='utf-8')
        js = (WEB / 'assets/research.js').read_text(encoding='utf-8')
        self.assertIn('/research.html', agents)
        self.assertIn('/assets/app.js', page)
        self.assertIn('/assets/research.js', page)
        self.assertNotRegex(page, r'<script[^>]+src=["\']https?://')
        ids = set(re.findall(r'id="([^"]+)"', page))
        required = {
            'research-query', 'research-source-count', 'research-provider', 'research-search', 'research-run',
            'research-answer', 'research-sources', 'research-route', 'research-refresh', 'research-history'
        }
        self.assertEqual(required - ids, set())
        for endpoint in ('/api/research/search', '/api/research/run', '/api/research'):
            self.assertIn(endpoint, js)

    def test_server_has_research_routes_and_public_research_is_separate_from_system_broker(self):
        server = (ROOT / 'sarus/server.py').read_text(encoding='utf-8')
        core = (ROOT / 'sarus/core/research.py').read_text(encoding='utf-8')
        for endpoint in ("'/api/research'", "'/api/research/search'", "'/api/research/fetch'", "'/api/research/run'"):
            self.assertIn(endpoint, server)
        self.assertIn('ip.is_private', core)
        self.assertIn('UNTRUSTED WEB CONTENT BEGIN', core)
        self.assertIn("port not in {80, 443}", core)
        self.assertNotIn('APP.privileged', core)


if __name__ == '__main__':
    unittest.main(verbosity=2)
