"""Read-only JavaScript browser for the Linux VPS profile.

This is intentionally not a general remote-control browser. Jubi can render and
read public pages, but this runtime exposes no form submission, file download,
credential entry, arbitrary JavaScript evaluation, localhost access, or private
network access.
"""
from __future__ import annotations

import importlib.util
import re
from urllib.parse import urljoin

from .research import PublicWebResearch


class VPSBrowser:
    MAX_TEXT = 80_000
    MAX_LINKS = 200

    def __init__(self, app):
        self.app = app

    def status(self) -> dict:
        available = importlib.util.find_spec("playwright") is not None
        return {
            "ready": available,
            "mode": "read-only-public-js-browser",
            "engine": "playwright-chromium" if available else None,
            "network": "public-http-https-only",
            "external_actions": False,
            "downloads": False,
            "forms": False,
        }

    @staticmethod
    def _validate(url: str) -> str:
        return PublicWebResearch._normalize_public_url(url)

    def read(self, url: str, timeout=25, wait_ms=800) -> dict:
        target = self._validate(url)
        try:
            from playwright.sync_api import sync_playwright
        except ImportError as exc:
            raise RuntimeError(
                "VPS browser dependency is not installed; reinstall with --with-browser"
            ) from exc

        timeout_ms = max(3_000, min(int(timeout) * 1000, 60_000))
        wait_ms = max(0, min(int(wait_ms), 5_000))
        with sync_playwright() as pw:
            browser = pw.chromium.launch(
                headless=True,
                args=[
                    "--disable-dev-shm-usage",
                    "--disable-background-networking",
                    "--disable-sync",
                    "--no-first-run",
                ],
            )
            context = browser.new_context(
                accept_downloads=False,
                java_script_enabled=True,
                service_workers="block",
            )
            page = context.new_page()

            def route_handler(route):
                req_url = route.request.url
                if req_url.startswith(("data:", "blob:", "about:")):
                    return route.continue_()
                try:
                    self._validate(req_url)
                except Exception:
                    return route.abort("blockedbyclient")
                if route.request.resource_type in {"websocket", "media", "font"}:
                    return route.abort("blockedbyclient")
                return route.continue_()

            context.route("**/*", route_handler)
            try:
                response = page.goto(target, wait_until="domcontentloaded", timeout=timeout_ms)
                if wait_ms:
                    page.wait_for_timeout(wait_ms)
                final_url = self._validate(page.url)
                title = page.title()[:500]
                text = page.locator("body").inner_text(timeout=5_000)
                text = re.sub(r"\n{3,}", "\n\n", text).strip()[: self.MAX_TEXT]
                links = []
                seen = set()
                for anchor in page.locator("a[href]").all()[: self.MAX_LINKS]:
                    try:
                        href = anchor.get_attribute("href")
                        if not href:
                            continue
                        absolute = self._validate(urljoin(final_url, href))
                        if absolute in seen:
                            continue
                        seen.add(absolute)
                        label = re.sub(r"\s+", " ", anchor.inner_text(timeout=1_000)).strip()[:300]
                        links.append({"text": label, "url": absolute})
                    except Exception:
                        continue
                return {
                    "ok": True,
                    "url": final_url,
                    "title": title,
                    "text": text,
                    "links": links,
                    "status_code": response.status if response else None,
                    "mode": "read-only-public-js-browser",
                }
            finally:
                context.close()
                browser.close()
