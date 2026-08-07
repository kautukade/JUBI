from pathlib import Path
import json, importlib
class AdapterManager:
    def __init__(self,root:Path,cfg:Path):
        self.root=root; self.cfg=json.loads(cfg.read_text(encoding='utf-8')); self.adapters={}
    def connect(self):
        out=[]
        for key,rel in self.cfg.items():
            if key not in self.adapters:
                mod=importlib.import_module('sarus.adapters.'+key); self.adapters[key]=mod.Adapter(self.root/'sources'/rel)
            out.append(self.adapters[key].probe())
        return out
    def get(self,name):
        if name not in self.adapters: self.connect()
        return self.adapters[name]
