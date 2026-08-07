from __future__ import annotations
from dataclasses import dataclass,asdict
import uuid

@dataclass
class Step:
    id:str
    agent:str
    source:str
    task:str
    risk:int=0
    status:str='queued'

class Orchestrator:
    def __init__(self,bus,models,policy):
        self.bus=bus; self.models=models; self.policy=policy
    def plan(self,text):
        t=text.lower(); steps=[]
        def add(agent,source,task,risk=0): steps.append(Step(str(uuid.uuid4())[:8],agent,source,task,risk))
        add('planner','hermes','decompose and coordinate request')
        if any(x in t for x in ['lead','sales','client','business','market','research']):
            add('research-method','awesome_llm_apps','select and apply the best research/workflow pattern'); add('specialist','agency_agents','apply the best business/research specialist persona'); add('live-research','sara','collect live browser evidence when internet research is required; otherwise inspect approved local sources',2); add('review','ecc','verify research evidence, duplicates, confidence and quality')
        if any(x in t for x in ['code','bug','website','project','develop','fix','build']):
            add('developer-method','superpowers','create a TDD/debugging implementation plan and verification criteria',1); add('implementation-review','ecc','review the proposed implementation against source rules and safety',1); add('local-developer','sara','execute the approved coding/project work inside the SARUS workspace and run tests',2); add('code-review','ecc','review the resulting implementation and test evidence',1)
        if any(x in t for x in ['screen','windows','laptop','app','file','browser','voice','camera','gesture']): add('computer','sara','perform permission-controlled local action',2)
        if any(x in t for x in ['memory','remember','knowledge','sop','document','presentation','brand']): add('knowledge','second_brain','retrieve or apply structured knowledge')
        if any(x in t for x in ['security','audit','vulnerability','defensive']): add('security','cai','defensive read-only analysis',1)
        if any(x in t for x in ['improve','benchmark','experiment','learn','optimize']): add('evaluation','autoresearch','run isolated measurable experiment design',2)
        add('receipt','fable_os','generate trusted action and evidence receipt'); add('verifier','ecc','final verification')
        self.bus.emit('PLAN_CREATED',{'request':text,'steps':[asdict(s) for s in steps]}); return steps
    def execute_dry(self,text):
        out=[]
        for s in self.plan(text):
            action='defensive_readonly' if s.source=='cai' else s.task; p=self.policy.evaluate(action,s.risk,s.source); s.status='ready' if p['decision']=='allow' else p['decision']; rec=asdict(s)|{'policy':p}; out.append(rec); self.bus.emit('STEP_EVALUATED',rec)
        return out
