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
        }
        for name, needles in expected.items():
            text = (WEB / name).read_text(encoding='utf-8')
            for needle in needles:
                self.assertIn(needle, text, name)
            self.assertIn('/assets/styles.css', text)
            self.assertIn('/assets/app.js', text)
        self.assertTrue((WEB / 'assets/network.js').is_file())
        self.assertTrue((WEB / 'assets/vision.js').is_file())

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
