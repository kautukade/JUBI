from __future__ import annotations
import os, tempfile, unittest
from pathlib import Path
from types import SimpleNamespace
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from sarus.core.browser import BrowserRuntime

class BrowserRuntimeTest(unittest.TestCase):
    def test_private_and_loopback_urls_are_blocked(self):
        with self.assertRaises(PermissionError): BrowserRuntime._safe_url('http://127.0.0.1/')
        with self.assertRaises(PermissionError): BrowserRuntime._safe_url('http://localhost/')
    @unittest.skipUnless(os.environ.get('JUBI_TEST_PLAYWRIGHT')=='1','Playwright smoke is CI opt-in')
    def test_actual_chromium_launch_and_render(self):
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True)
            page=browser.new_page(); page.set_content('<html><title>Jubi Browser</title><body>browser-ok</body></html>')
            self.assertEqual(page.title(),'Jubi Browser'); self.assertIn('browser-ok',page.locator('body').inner_text())
            browser.close()

if __name__=='__main__': unittest.main(verbosity=2)
