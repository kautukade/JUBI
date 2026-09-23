from __future__ import annotations
import importlib.util
import time
import uuid
from pathlib import Path

from .research import PublicWebResearch

class BrowserRuntime:
    def __init__(self, app):
        self.app=app; self.root=app.root.resolve(); self.output=(self.root/'outputs/browser').resolve()
    def status(self):
        installed=importlib.util.find_spec('playwright') is not None
        return {'ready':installed,'mode':'playwright-readonly-public-web','actions':['navigate','read','screenshot'] if installed else [],'external_form_submission':False}
    @staticmethod
    def _safe_url(url): return PublicWebResearch._normalize_public_url(str(url or ''))
    def browse(self,url,screenshot=False,timeout=30):
        target=self._safe_url(url); started=time.time()
        try: from playwright.sync_api import sync_playwright
        except ImportError: return {'ok':False,'status':'DEPENDENCY_MISSING','error':'Playwright is not installed','url':target}
        blocked=[]; links=[]; shot=None
        with sync_playwright() as p:
            browser=p.chromium.launch(headless=True)
            context=browser.new_context(accept_downloads=False)
            page=context.new_page()
            def route(req):
                try:
                    self._safe_url(req.request.url); req.continue_()
                except Exception:
                    blocked.append(req.request.url[:500]); req.abort()
            page.route('**/*',route)
            page.goto(target,wait_until='domcontentloaded',timeout=max(3000,min(int(timeout)*1000,60000)))
            final=self._safe_url(page.url)
            title=page.title()[:1000]
            text=page.locator('body').inner_text(timeout=10000)[:60000]
            for row in page.locator('a[href]').evaluate_all("els => els.slice(0,100).map(a => ({text:(a.innerText||'').trim(),href:a.href}))"):
                try: links.append({'text':str(row.get('text',''))[:500],'url':self._safe_url(row.get('href',''))})
                except Exception: pass
            if screenshot:
                self.output.mkdir(parents=True,exist_ok=True); path=self.output/(str(uuid.uuid4())+'.png')
                page.screenshot(path=str(path),full_page=True); shot=str(path.relative_to(self.root))
            context.close(); browser.close()
        return {'ok':True,'status':'completed','url':final,'title':title,'text':text,'links':links,'blocked_requests':len(blocked),'screenshot':shot,'elapsed_ms':round((time.time()-started)*1000,2)}
