"""Isolated HTTP fixture. Inference uses an explicitly named local test double."""
import json
import os
import shutil
import sys
import tempfile
import threading
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))

class RuntimeFixture:
    def __enter__(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        shutil.copytree(ROOT/'config',self.root/'config')
        for source in json.loads((self.root/'config/sources.json').read_text()).values():
            folder=self.root/'sources'/source;folder.mkdir(parents=True)
            (folder/'README.md').write_text('Controlled test source for the Jubi HTTP regression suite.')
        self.model_requests=[]
        owner=self
        class OllamaTestHandler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def reply(self,payload):
                raw=json.dumps(payload).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
            def do_GET(self):
                self.reply({'models':[{'name':m} for m in ['qwen2.5:7b','qwen2.5-coder:7b','qwen2.5vl:3b','nomic-embed-text-v2-moe:latest']]})
            def do_POST(self):
                body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))));owner.model_requests.append((self.path,body))
                if self.path=='/api/show': return self.reply({'details':{'format':'gguf'},'model_info':{'test.model':True}})
                if self.path=='/api/embed': return self.reply({'embeddings':[[1.0,0.5,0.1]]})
                prompt=body.get('prompt','')
                response='Controlled test inference response. This is not a live model quality test.'
                if 'Return ONLY valid JSON' in prompt:
                    response=json.dumps({'steps':[{'id':'S1','role':'general','task':'Inspect test task','depends_on':[]}]})
                self.reply({'response':response,'model':body.get('model'),'done':True})
        self.ollama=ThreadingHTTPServer(('127.0.0.1',0),OllamaTestHandler)
        self.ollama_thread=threading.Thread(target=self.ollama.serve_forever,daemon=True);self.ollama_thread.start()
        self.env=patch.dict(os.environ,{'JUBI_OLLAMA_URL':f'http://127.0.0.1:{self.ollama.server_port}',
                                     'SARUS_BROKER_APPROVAL_SECRET':'test-only-broker-secret-for-fixtures',
                                     'SARUS_RECEIPT_SIGNING_KEY_FILE':str(self.root/'receipt.key')})
        self.env.start()
        from sarus.core.app import Jubi
        self.app=Jubi(self.root)
        # The production server constructs its app at import time. Inject this
        # fixture during import so tests never open the checkout's user database.
        with patch('sarus.core.app.Jubi',return_value=self.app):
            import sarus.server as server
        server.Jubi=Jubi
        self.server=server;self.previous=server.APP;server.APP=self.app
        self.http=ThreadingHTTPServer(('127.0.0.1',0),server.H)
        self.thread=threading.Thread(target=self.http.serve_forever,daemon=True);self.thread.start()
        self.base=f'http://127.0.0.1:{self.http.server_port}'
        return self
    def __exit__(self,*args):
        self.http.shutdown();self.http.server_close();self.thread.join()
        self.app.shutdown();self.server.APP=self.previous
        self.ollama.shutdown();self.ollama.server_close();self.ollama_thread.join()
        self.env.stop();self.temp.cleanup()
