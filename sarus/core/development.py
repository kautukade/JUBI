from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import time
import uuid
from pathlib import Path

from .database import read_connection, transaction

TEXT_EXTENSIONS = {'.py','.js','.mjs','.cjs','.json','.md','.html','.css','.yml','.yaml','.toml','.ini','.cfg','.txt'}

class DevelopmentAgent:
    MAX_FILES = 60
    MAX_FILE_CHARS = 24000
    MAX_CONTEXT_CHARS = 140000
    MAX_EDIT_FILES = 6
    MAX_EDIT_CHARS = 180000

    def __init__(self, app):
        self.app = app
        self.root = app.root.resolve()
        self.db = app.db_path
        cfg = json.loads((self.root / 'config/broker_allowlist.json').read_text(encoding='utf-8'))
        self.allowed_roots = tuple((self.root / x).resolve() for x in cfg.get('path_scopes',{}).get('user_workspace',[]))
        with transaction(self.db) as c:
            c.execute('CREATE TABLE IF NOT EXISTS development_runs(id TEXT PRIMARY KEY,ts REAL,request TEXT,project TEXT,status TEXT,changed_files TEXT,verification TEXT,review TEXT,route TEXT,error TEXT)')

    def _project(self, value):
        p = Path(str(value or '').strip()).expanduser()
        p = (p if p.is_absolute() else self.root / p).resolve()
        if not self.allowed_roots or not any(p == b or b in p.parents for b in self.allowed_roots):
            raise PermissionError('Development project must be inside an approved Jubi workspace')
        if not p.is_dir():
            raise NotADirectoryError(str(p))
        return p

    def _target(self, project, value):
        rel = Path(str(value or '').replace('\\','/'))
        if rel.is_absolute() or '..' in rel.parts or not rel.parts:
            raise ValueError('edit path must stay inside the project')
        p = (project / rel).resolve()
        if not p.is_relative_to(project) or p.suffix.lower() not in TEXT_EXTENSIONS:
            raise PermissionError('unsupported edit path or file type')
        return p

    def _context(self, project):
        rows = []
        ignored = {'.git','node_modules','.venv','venv','__pycache__','.pytest_cache','dist','build'}
        for base, dirs, names in os.walk(project, followlinks=False):
            dirs[:] = [d for d in dirs if d not in ignored]
            for name in sorted(names):
                p = Path(base) / name
                if p.suffix.lower() in TEXT_EXTENSIONS:
                    try: rows.append((p.stat().st_size, p.relative_to(project).as_posix(), p))
                    except OSError: pass
                if len(rows) >= self.MAX_FILES: break
            if len(rows) >= self.MAX_FILES: break
        parts, total = [], 0
        for _, rel, p in sorted(rows, key=lambda x:(x[0],x[1])):
            try: text = p.read_text(encoding='utf-8', errors='replace')[:self.MAX_FILE_CHARS]
            except OSError: continue
            block = '\n--- FILE ' + rel + ' ---\n' + text + '\n--- END FILE ---\n'
            if total + len(block) > self.MAX_CONTEXT_CHARS: break
            parts.append(block); total += len(block)
        return ''.join(parts)

    @staticmethod
    def _json(text):
        raw = str(text or '').strip()
        raw = re.sub(r'^```(?:json)?\s*|\s*```$', '', raw, flags=re.I|re.S).strip()
        try: value = json.loads(raw)
        except json.JSONDecodeError:
            a,b = raw.find('{'), raw.rfind('}')
            if a < 0 or b <= a: raise ValueError('model did not return JSON')
            value = json.loads(raw[a:b+1])
        if not isinstance(value, dict): raise ValueError('model JSON must be an object')
        return value

    def _generate_json(self, prompt, system, provider, model, timeout):
        result = self.app.providers.generate(prompt, task_type='coding', provider=provider, model=model, system=system, timeout=timeout)
        body = str(result.get('response') or result.get('output') or '')
        return self._json(body), result.get('jubi_provider_route') or result.get('jubi_route') or {}

    def _verify(self, project, changed):
        checks = []
        py = [x for x in changed if x.endswith('.py')]
        js = [x for x in changed if Path(x).suffix.lower() in {'.js','.mjs','.cjs'}]
        if py:
            checks.append(self._run([os.sys.executable,'-m','py_compile',*py], project, 60))
        node = shutil.which('node')
        for rel in js if node else []:
            checks.append(self._run([node,'--check',rel], project, 30))
        for rel in [x for x in changed if x.endswith('.json')]:
            try:
                json.loads((project/rel).read_text(encoding='utf-8'))
                checks.append({'ok':True,'exit_code':0,'stdout':rel+': valid JSON','stderr':''})
            except Exception as exc:
                checks.append({'ok':False,'exit_code':1,'stdout':'','stderr':str(exc)})
        if not checks: checks.append({'ok':True,'exit_code':0,'stdout':'bounded text edit','stderr':''})
        return {'ok': all(x['ok'] for x in checks), 'checks': checks}

    @staticmethod
    def _run(argv, cwd, timeout):
        try:
            cp = subprocess.run(argv,cwd=str(cwd),capture_output=True,text=True,shell=False,timeout=timeout,encoding='utf-8',errors='replace')
            return {'ok':cp.returncode==0,'exit_code':cp.returncode,'stdout':cp.stdout[-30000:],'stderr':cp.stderr[-30000:],'argv':argv}
        except (OSError,subprocess.TimeoutExpired) as exc:
            return {'ok':False,'exit_code':None,'stdout':'','stderr':str(exc),'argv':argv}

    def _diff(self, project, changed):
        git = shutil.which('git')
        if git and (project/'.git').exists():
            out = self._run([git,'diff','--',*changed], project, 30)
            if out['ok']: return out['stdout'][-90000:]
        return '\n'.join('CHANGED '+x for x in changed)

    def run(self, request, project_path, provider='ollama', model=None):
        request = str(request or '').strip()
        if not request: raise ValueError('development request is required')
        project = self._project(project_path); run_id = str(uuid.uuid4()); started = time.time()
        changed, backups, verification, review, route, error = [], {}, {}, {}, {}, ''
        status = 'failed'
        try:
            context = self._context(project)
            prompt = 'User request:\n'+request+'\n\nReturn ONLY JSON {"summary":"...","edits":[{"path":"relative/path","content":"complete replacement"}]}. No shell commands. Keep edits minimal.\n'+context
            proposal, coding_route = self._generate_json(prompt, 'You are Jubi Development. Return strict JSON only.', provider, model, 300)
            edits = proposal.get('edits')
            if not isinstance(edits,list) or not edits or len(edits)>self.MAX_EDIT_FILES: raise ValueError('invalid edit set')
            total = 0; prepared = []
            for row in edits:
                if not isinstance(row,dict) or not isinstance(row.get('content'),str): raise ValueError('invalid edit')
                target = self._target(project,row.get('path')); rel = target.relative_to(project).as_posix(); content = row['content']; total += len(content)
                if total > self.MAX_EDIT_CHARS: raise ValueError('edit payload too large')
                old = target.read_text(encoding='utf-8',errors='replace') if target.is_file() else ''
                if old != content: prepared.append((target,rel,target.exists(),old,content))
            if not prepared: raise ValueError('model proposed no actual changes')
            for target,rel,existed,old,content in prepared:
                backups[rel]=(existed,old); target.parent.mkdir(parents=True,exist_ok=True); target.write_text(content,encoding='utf-8'); changed.append(rel)
            verification = self._verify(project,changed); diff = self._diff(project,changed)
            review_prompt = 'Original request:\n'+request+'\n\nObserved diff:\n'+diff+'\n\nVerification:\n'+json.dumps(verification)+'\nReturn ONLY JSON {"approved":true,"reason":"..."} or approved false.'
            review, review_route = self._generate_json(review_prompt, 'You are an independent Jubi code reviewer. Return strict JSON only.', provider, model, 240)
            route={'coding':coding_route,'review':review_route}
            if verification.get('ok') and review.get('approved') is True: status='completed'
            else: error=str(review.get('reason') or 'verification/review rejected changes')
        except Exception as exc:
            error=str(exc)
        if status != 'completed':
            for rel,(existed,old) in backups.items():
                target=project/rel
                try:
                    if existed: target.write_text(old,encoding='utf-8')
                    elif target.exists(): target.unlink()
                except OSError: pass
            changed=[]
        record={'id':run_id,'status':status,'request':request,'project':str(project.relative_to(self.root)),'changed_files':changed,'verification':verification,'review':review,'route':route,'error':error,'elapsed_ms':round((time.time()-started)*1000,2)}
        with transaction(self.db) as c:
            c.execute('INSERT INTO development_runs VALUES(?,?,?,?,?,?,?,?,?,?)',(run_id,started,request,record['project'],status,json.dumps(changed),json.dumps(verification),json.dumps(review),json.dumps(route),error[:4000]))
        self.app.bus.emit('DEVELOPMENT_RUN_FINISHED',{'id':run_id,'status':status,'changed_files':changed})
        return record

    def recent(self, limit=30):
        limit=max(1,min(int(limit),100))
        with read_connection(self.db) as c:
            rows=c.execute('SELECT id,ts,request,project,status,changed_files,verification,review,route,error FROM development_runs ORDER BY ts DESC LIMIT ?',(limit,)).fetchall()
        return [{'id':r[0],'ts':r[1],'request':r[2],'project':r[3],'status':r[4],'changed_files':json.loads(r[5] or '[]'),'verification':json.loads(r[6] or '{}'),'review':json.loads(r[7] or '{}'),'route':json.loads(r[8] or '{}'),'error':r[9] or ''} for r in rows]
