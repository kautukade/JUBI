import json
from pathlib import Path
class PolicyEngine:
    def __init__(self,path:Path): self.cfg=json.loads(path.read_text(encoding='utf-8'))
    def evaluate(self,action:str,level:int=0,source:str='core'):
        if action in self.cfg.get('never_auto',[]): return {'decision':'deny','reason':'never_auto'}
        if action in self.cfg.get('always_require_approval',[]) or level>=4: return {'decision':'approval','reason':'high_risk'}
        if source=='cai' and action!='defensive_readonly': return {'decision':'isolated','reason':'cai_isolation'}
        if source=='autoresearch' and level>0: return {'decision':'isolated','reason':'experiment_isolation'}
        if source=='fable_os' and 'kernel' in action.lower(): return {'decision':'isolated','reason':'kernel_isolation'}
        return {'decision':'allow','reason':'policy_ok'}
