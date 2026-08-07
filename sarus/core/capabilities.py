from __future__ import annotations
from pathlib import Path
import hashlib, json, re
TEXT_EXT={'.md','.txt','.py','.js','.ts','.tsx','.jsx','.json','.yaml','.yml','.toml','.ini','.cfg','.sh','.ps1','.bat','.c','.h','.cpp','.html','.css'}
class CapabilityRegistry:
    def __init__(self, root: Path, source_cfg: Path, out: Path):
        self.root = root; self.sources = json.loads(source_cfg.read_text(encoding='utf-8')); self.out = out
    def build(self):
        records=[]
        for source, rel in self.sources.items():
            p=self.root/'sources'/rel
            for f in p.rglob('*'):
                if not f.is_file(): continue
                rp=str(f.relative_to(p)).replace('\\','/'); low='/'+rp.lower(); kind='asset'
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
