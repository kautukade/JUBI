from __future__ import annotations
from pathlib import Path
import hashlib, json, re, os
from dataclasses import dataclass, asdict
from typing import Callable


@dataclass(frozen=True)
class CapabilitySpec:
    id: str
    name: str
    source: str
    version: str
    category: str
    description: str
    platforms: tuple[str, ...]
    dependencies: tuple[str, ...]
    permissions: tuple[str, ...]
    privacy: str
    risk: int
    input_schema: dict
    output_schema: dict
    health_check: str
    executor: str
    timeout_seconds: int
    resource_requirements: dict
    isolation: str
    availability: str
    verification: str

TEXT_EXT={'.md','.txt','.py','.js','.ts','.tsx','.jsx','.json','.yaml','.yml','.toml','.ini','.cfg','.sh','.ps1','.bat','.c','.h','.cpp','.html','.css'}
AVAILABILITY = frozenset({'AVAILABLE', 'PARTIAL', 'DEPENDENCY_MISSING', 'DISABLED', 'EXPERIMENTAL', 'FAILED'})
class CapabilityRegistry:
    def __init__(self, root: Path, source_cfg: Path, out: Path):
        self.root = root; self.sources = json.loads(source_cfg.read_text(encoding='utf-8')); self.out = out
        self._executors: dict[str, tuple[CapabilitySpec, Callable]] = {}

    def register_executor(self, spec: CapabilitySpec, handler: Callable):
        if spec.id in self._executors:
            raise ValueError(f'duplicate executable capability: {spec.id}')
        if spec.source in {'cai', 'cybergym', 'cybergym-e2e', 'exploitgym'}:
            raise PermissionError('Cyber Lab capabilities cannot register in the normal runtime')
        if spec.privacy != 'local_only' or not callable(handler):
            raise ValueError('Only reviewed local executors may register in this release')
        if spec.availability not in AVAILABILITY:
            raise ValueError('Unknown capability availability state')
        self._executors[spec.id] = (spec, handler)

    def executors(self):
        return [asdict(spec) for spec, _ in self._executors.values()]

    def execute(self, capability_id: str, parameters: dict):
        # Indexed source IDs and caller-provided executor/import names have no
        # execution authority. Only startup-registered callables are reachable.
        if capability_id not in self._executors:
            raise PermissionError('This source entry has no registered executable capability')
        spec, handler = self._executors[capability_id]
        if spec.availability not in {'AVAILABLE', 'EXPERIMENTAL'}:
            raise PermissionError(f'Capability is {spec.availability}')
        if not isinstance(parameters, dict):
            raise ValueError('Capability input must be an object')
        properties = spec.input_schema.get('properties', {})
        if set(parameters) - set(properties):
            raise ValueError('Unknown capability parameters')
        if set(spec.input_schema.get('required', [])) - set(parameters):
            raise ValueError('Missing required capability parameters')
        for name, value in parameters.items():
            rule = properties[name]
            if rule.get('type') != 'string' or not isinstance(value, str):
                raise ValueError(f'{name} must be a string')
            if len(value) > rule.get('maxLength', 16000):
                raise ValueError(f'{name} exceeds its input limit')
            if 'enum' in rule and value not in rule['enum']:
                raise ValueError(f'{name} is outside the allowed values')
        return handler(**parameters)
    def build(self):
        records=[]
        for source, rel in self.sources.items():
            p=self.root/'sources'/rel
            files = []
            for directory, dirs, names in os.walk(p, followlinks=False):
                dirs[:] = [d for d in dirs if d not in {'__pycache__', '.git', '.pytest_cache', 'node_modules', '.venv'}]
                relative = os.path.relpath(directory, p)
                files.extend((Path(directory)/name, name if relative == '.' else relative+'/'+name) for name in names)
            for f, relative in files:
                rp=relative.replace('\\','/'); low='/'+rp.lower(); kind='asset'
                if f.name.lower() in {'skill.md','skills.md'} or '/skills/' in low: kind='skill'
                elif '/agents/' in low or 'agent' in f.stem.lower(): kind='agent'
                elif '/tools/' in low or 'tool' in f.stem.lower(): kind='tool'
                elif '/commands/' in low or 'command' in f.stem.lower(): kind='command'
                elif f.suffix.lower() in {'.py','.js','.ts','.tsx','.jsx','.c','.cpp','.rs'}: kind='code'
                elif f.suffix.lower() in {'.md','.txt'}: kind='doc'
                cid=hashlib.sha1(f'{source}:{rp}'.encode()).hexdigest()[:16]
                records.append({'id':cid,'source':source,'path':rp,'name':f.stem,'kind':kind,'size':f.stat().st_size,'text':f.suffix.lower() in TEXT_EXT})
        self.out.parent.mkdir(parents=True,exist_ok=True); self.out.write_text(json.dumps(records,ensure_ascii=False),encoding='utf-8'); return records
    def load(self):
        if not self.out.exists(): return self.build()
        rows=json.loads(self.out.read_text(encoding='utf-8'))
        if rows and 'id' not in rows[0]: return self.build()
        return rows
    def summary(self):
        out={}
        for r in self.load():
            x=out.setdefault(r['source'],{'files':0,'agents':0,'skills':0,'tools':0,'commands':0,'code':0,'docs':0}); x['files']+=1; k=r['kind']+'s'
            if k in x: x[k]+=1
        return out
    def search(self, q: str='', source: str|None=None, kinds: list[str]|None=None, limit=50):
        toks=[x for x in re.findall(r'[a-z0-9_+-]+',q.lower()) if len(x)>1]; scored=[]
        for r in self.load():
            if source and r['source'] != source: continue
            if kinds and r['kind'] not in kinds: continue
            hay=(r['name']+' '+r['path']).lower(); score=sum((4 if t in r['name'].lower() else 1) for t in toks if t in hay)
            if q and score==0: continue
            scored.append((score,r))
        scored.sort(key=lambda x:(-x[0],x[1]['path'])); return [r for _,r in scored[:limit]]
    def get(self, cid): return next((r for r in self.load() if r['id']==cid),None)
    def path_for(self, r): return self.root/'sources'/self.sources[r['source']]/r['path']
    def read(self, cid, max_chars=24000):
        r=self.get(cid)
        if not r: return None
        p=self.path_for(r); content=''
        if r.get('text'):
            try: content=p.read_text(encoding='utf-8',errors='replace')[:max_chars]
            except Exception: content=''
        return r|{'content':content}
    def best(self, source, q, kinds=None):
        rows=self.search(q,source,kinds,1); return rows[0] if rows else None
