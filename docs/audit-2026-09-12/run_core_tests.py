"""Audit harness: run existing tests against a disposable copy, no installs."""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
logs = OUT/'test-logs'
logs.mkdir(exist_ok=True)
scratch = Path(tempfile.mkdtemp(prefix='jubi-audit-'))
for folder in ('jubi','sarus','tests','scripts','installer','driver','.github','config','docs'):
    shutil.copytree(ROOT/folder, scratch/folder, ignore=shutil.ignore_patterns('__pycache__','audit-2026-09-12'))
for file in ROOT.iterdir():
    if file.is_file():
        shutil.copy2(file, scratch/file.name)
# Providers remain read-only original source trees. All mutable app state is temporary.
source_cfg=json.loads((ROOT/'config/sources.json').read_text(encoding='utf-8'))
(scratch/'config/sources.json').write_text(json.dumps({key:str(ROOT/'sources'/rel) for key,rel in source_cfg.items()}),encoding='utf-8')
env={k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR','COMSPEC','PATH','PATHEXT','TEMP','TMP','NUMBER_OF_PROCESSORS','PROCESSOR_ARCHITECTURE'}}
env.update({'LOCALAPPDATA':str(scratch/'localappdata'),'APPDATA':str(scratch/'appdata'),
            'USERPROFILE':str(scratch/'profile'),'PYTHONDONTWRITEBYTECODE':'1','PYTHONUTF8':'1',
            'PYTHONPATH':str(scratch),'JUBI_OLLAMA_URL':'http://127.0.0.1:9',
            'SARUS_BROKER_APPROVAL_SECRET':'audit-only-test-secret-at-least-32-characters',
            'SARUS_RECEIPT_SIGNING_KEY_FILE':str(scratch/'audit-receipt.key'),
            'NODE_PATH':r'C:\Users\hp\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules'})
results=[]
for script in sorted((scratch/'tests').glob('*_test.py')):
    (scratch/'config/sources.json').write_text(json.dumps(source_cfg if script.name == 'jubi_http_functional_test.py' else {key:str(ROOT/'sources'/rel) for key,rel in source_cfg.items()}),encoding='utf-8')
    start=time.monotonic()
    try:
        cp=subprocess.run([sys.executable,str(script)],cwd=scratch,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
        output=cp.stdout+cp.stderr
        match=re.search(r'Ran (\d+) tests?',output)
        row={'suite':script.name,'returncode':cp.returncode,'tests':int(match[1]) if match else None,'seconds':round(time.monotonic()-start,2)}
    except subprocess.TimeoutExpired as exc:
        output=str(exc.stdout or '')+'\n'+str(exc.stderr or '')
        row={'suite':script.name,'returncode':'timeout','seconds':round(time.monotonic()-start,2)}
    (logs/(script.name+'.log')).write_text(output,encoding='utf-8')
    results.append(row)
    print(json.dumps(row),flush=True)
    (OUT/'test-results.json').write_text(json.dumps({'fixture_root':str(scratch),'fixture_note':'Core copied unchanged; config/sources.json in disposable copy uses absolute original read-only source paths. Real external credentials removed from child environment. Ollama points to unavailable loopback port except explicit test doubles. No packages installed.','results':results},indent=2),encoding='utf-8')
for script in sorted((scratch/'sarus/web/assets').glob('*.js')):
    cp=subprocess.run(['node','--check',str(script)],cwd=scratch,env=env,capture_output=True,text=True)
    results.append({'suite':'node --check '+script.name,'returncode':cp.returncode})
    (logs/(script.name+'.syntax.log')).write_text(cp.stdout+cp.stderr,encoding='utf-8')
try:
    (scratch/'config/sources.json').write_text(json.dumps(source_cfg),encoding='utf-8')
    cp=subprocess.run([sys.executable,str(scratch/'tests/run_dashboard_runtime.py')],cwd=scratch,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=150)
    results.append({'suite':'dashboard_runtime_test.cjs','returncode':cp.returncode})
    (logs/'dashboard-runtime.log').write_text(cp.stdout+cp.stderr,encoding='utf-8')
except subprocess.TimeoutExpired:
    results.append({'suite':'dashboard_runtime_test.cjs','returncode':'timeout'})
summary=json.loads((OUT/'test-results.json').read_text(encoding='utf-8'))
summary['results']=results
(OUT/'test-results.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps({'done':True,'fixture_root':str(scratch),'results':results},indent=2),flush=True)
