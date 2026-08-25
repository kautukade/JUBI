from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'sarus' / 'web'


class NetworkVisionUITests(unittest.TestCase):
    def test_pages_and_assets_exist(self):
        expected = {
            'network.html': ('data-page="network"', '/assets/network.js', 'Authorized LAN Manager'),
            'vision.html': ('data-page="vision"', '/assets/vision.js', 'Vision & Voice'),
            'research.html': ('data-page="research"', '/assets/research.js', 'Public Web Research'),
        }
        for name, needles in expected.items():
            text = (WEB / name).read_text(encoding='utf-8')
            for needle in needles:
                self.assertIn(needle, text, name)
            self.assertIn('/assets/styles.css', text)
            self.assertIn('/assets/app.js', text)
            self.assertIn('/assets/shell-extension.js', text)
        for asset in ('network.js', 'vision.js', 'operator.js', 'shell-extension.js'):
            self.assertTrue((WEB / 'assets' / asset).is_file(), asset)

    def test_advanced_shell_exposes_real_feature_pages(self):
        shell = (WEB / 'assets' / 'shell-extension.js').read_text(encoding='utf-8')
        for path in ('/research.html', '/network.html', '/vision.html'):
            self.assertIn(path, shell)
        for page in ('research', 'network', 'vision'):
            self.assertIn(page, shell)

    def test_client_uses_real_network_and_vision_apis(self):
        network = (WEB / 'assets/network.js').read_text(encoding='utf-8')
        for endpoint in (
            '/api/network', '/api/network/devices', '/api/network/observations',
            '/api/network/discover', '/api/network/device', '/api/network/check', '/api/network/delete',
        ):
            self.assertIn(endpoint, network)
        self.assertNotIn('scan all ports', network.lower())
        vision = (WEB / 'assets/vision.js').read_text(encoding='utf-8')
        self.assertIn('/api/vision', vision)
        self.assertIn('/api/vision/analyze', vision)
        self.assertIn('SpeechRecognition', vision)
        self.assertIn('speechSynthesis', vision)

    def test_computer_page_wires_typed_operator_controls(self):
        page = (WEB / 'computer.html').read_text(encoding='utf-8')
        self.assertIn('/assets/operator.js', page)
        for control in ('path-stat', 'dir-list', 'dir-create', 'operator-copy', 'operator-move', 'operator-delete', 'git-status', 'git-log', 'app-launch'):
            self.assertIn(f'id="{control}"', page)
        js = (WEB / 'assets/operator.js').read_text(encoding='utf-8')
        for action in (
            'workspace.path.stat', 'workspace.directory.list', 'workspace.directory.create',
            'workspace.file.copy', 'workspace.file.move', 'workspace.file.delete',
            'development.git.status', 'development.git.log', 'app.launch',
        ):
            self.assertIn(action, js)
        self.assertIn('/api/system/action', js)

    def test_server_exposes_bounded_endpoints(self):
        server = (ROOT / 'sarus' / 'server.py').read_text(encoding='utf-8')
        for endpoint in (
            "'/api/network'", "'/api/network/devices'", "'/api/network/observations'",
            "'/api/network/discover'", "'/api/network/device'", "'/api/network/check'",
            "'/api/network/delete'", "'/api/vision'", "'/api/vision/analyze'",
        ):
            self.assertIn(endpoint, server)
        self.assertIn('MAX_VISION_BODY', server)
        self.assertIn("host == '0.0.0.0'", server)


if __name__ == '__main__':
    unittest.main(verbosity=2)
