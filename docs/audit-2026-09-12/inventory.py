"""Audit-only inventory; never imports or executes source providers."""
from pathlib import Path
import ast
import collections
import csv
import hashlib
import json
import os

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
EXCLUDED = {'.git'}
rows, errors, summaries, symbols = [], [], {}, []
for parent, directories, files in os.walk(ROOT, followlinks=False):
    directories[:] = sorted(d for d in directories if d not in EXCLUDED and (Path(parent)/d).resolve() != OUT)
    for name in sorted(files):
        path = Path(parent)/name
        rel = path.relative_to(ROOT).as_posix()
        try:
            raw = path.read_bytes()
            group = '/'.join(path.relative_to(ROOT).parts[:2]) if rel.startswith('sources/') else path.relative_to(ROOT).parts[0]
            ext = path.suffix.lower() or '(none)'
            rows.append({'path':rel, 'bytes':len(raw), 'sha256':hashlib.sha256(raw).hexdigest(), 'extension':ext, 'group':group})
            s = summaries.setdefault(group, {'files':0,'bytes':0,'extensions':collections.Counter()})
            s['files'] += 1
            s['bytes'] += len(raw)
            s['extensions'][ext] += 1
            if path.suffix == '.py' and rel.split('/')[0] in {'jubi','sarus','scripts','tests'}:
                try:
                    tree = ast.parse(raw, filename=rel)
                    definitions = [{'name':n.name, 'line':n.lineno, 'kind':type(n).__name__} for n in ast.walk(tree) if isinstance(n,(ast.ClassDef,ast.FunctionDef,ast.AsyncFunctionDef))]
                    symbols.append({'path':rel,'definitions':definitions,'syntax':'pass'})
                except Exception as exc:
                    symbols.append({'path':rel,'syntax':'fail','error':str(exc)})
        except Exception as exc:
            errors.append({'path':rel,'error':str(exc)})
with (OUT/'file-inventory.csv').open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,fieldnames=['path','bytes','sha256','extension','group'])
    writer.writeheader(); writer.writerows(rows)
report={'scope':'All files recursively, including hidden source directories. Excludes .git metadata and this audit output directory. Hash inventory is not a claim of line-by-line semantic review.', 'files':len(rows),'bytes':sum(r['bytes'] for r in rows),'errors':errors,'groups':summaries}
(OUT/'inventory-summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
(OUT/'core-symbols.json').write_text(json.dumps(symbols,indent=2),encoding='utf-8')
print(json.dumps({'files':report['files'],'bytes':report['bytes'],'errors':errors,'groups':{k:{'files':v['files'],'bytes':v['bytes']} for k,v in summaries.items()},'core_python_files':len(symbols)},indent=2))
