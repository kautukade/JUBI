from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.browser import VPSBrowser


class App:
    pass


class VPSBrowserTests(unittest.TestCase):
    def setUp(self):
        self.browser = VPSBrowser(App())

    def test_private_and_local_targets_are_blocked(self):
        for url in (
            "http://127.0.0.1/",
            "http://localhost/",
            "http://10.0.0.1/",
            "http://192.168.1.1/",
            "http://169.254.169.254/latest/meta-data/",
            "file:///etc/passwd",
        ):
            with self.subTest(url=url), self.assertRaises((PermissionError, ValueError)):
                self.browser._validate(url)

    def test_credentials_and_nonstandard_ports_are_blocked(self):
        for url in (
            "https://user:pass@example.com/",
            "https://example.com:8443/",
        ):
            with self.subTest(url=url), self.assertRaises(PermissionError):
                self.browser._validate(url)

    def test_status_is_truthful(self):
        status = self.browser.status()
        self.assertEqual(status["mode"], "read-only-public-js-browser")
        self.assertFalse(status["external_actions"])
        self.assertFalse(status["downloads"])
        self.assertFalse(status["forms"])

    @unittest.skipUnless(os.environ.get("JUBI_BROWSER_LIVE") == "1", "live Chromium smoke is opt-in")
    def test_live_chromium_reads_public_page(self):
        status = self.browser.status()
        self.assertTrue(status["ready"], status)
        page = self.browser.read("https://example.com/", timeout=30, wait_ms=0)
        self.assertTrue(page["ok"], page)
        self.assertEqual(page["status_code"], 200)
        self.assertIn("Example Domain", page["title"])
        self.assertIn("Example Domain", page["text"])
        self.assertEqual(page["mode"], "read-only-public-js-browser")


if __name__ == "__main__":
    unittest.main(verbosity=2)
