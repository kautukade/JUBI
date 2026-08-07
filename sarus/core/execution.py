from __future__ import annotations
from contextlib import closing
from dataclasses import asdict
import json, sqlite3, time, uuid, traceback
class ExecutionEngine:
    def __init__(self,app):
        self.app=app
        with closing(sqlite3.connect(app.root/'data/sarus.db')) as c:
            c.execute("CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY,ts REAL,request TEXT,status TEXT,result TEXT,source TEXT)"); c.execute("CREATE TABLE IF NOT EXISTS approvals(id TEXT PRIMARY KEY,ts REAL,task_id TEXT,step_id TEXT,action TEXT,status TEXT,payload TEXT)")
    def _save_task(self,tid,request,status,result=None,source='user'):
        with closing(sqlite3.connect(self.app.root/'data/sarus.db')) as c: c.execute("INSERT OR REPLACE INTO tasks VALUES(?,?,?,?,?,?)",(tid,time.time(),request,status,json.dumps(result,ensure_ascii=False) if result is not None else '',source))
    def create_approval(self,tid,step,payload):
        aid=str(uuid.uuid4())
        with closing(sqlite3.connect(self.app.root/'data/sarus.db')) as c: c.execute("INSERT INTO approvals VALUES(?,?,?,?,?,?,?)",(aid,time.time(),tid,step.id,step.task,'pending',json.dumps(payload,ensure_ascii=False)))
        return aid
    def approvals(self,status='pending'):
        with closing(sqlite3.connect(self.app.root/'data/sarus.db')) as c: rows=c.execute("SELECT id,ts,task_id,step_id,action,status,payload FROM approvals WHERE status=? ORDER BY ts DESC",(status,)).fetchall()
        return [{'id':r[0],'ts':r[1],'task_id':r[2],'step_id':r[3],'action':r[4],'status':r[5],'payload':json.loads(r[6])} for r in rows]
    def set_approval(self,aid,status):
        if status not in {'approved','rejected'}: raise ValueError('bad status')
        with closing(sqlite3.connect(self.app.root/'data/sarus.db')) as c: c.execute("UPDATE approvals SET status=? WHERE id=?",(status,aid))
        return {'id':aid,'status':status}
    def run(self,request:str,source='user',capability_id=None):
        tid=str(uuid.uuid4()); self._save_task(tid,request,'running',source=source); self.app.bus.emit('TASK_STARTED',{'task_id':tid,'request':request,'source':source}); steps=self.app.orchestrator.plan(request); results=[]; context=[]; cap_meta=self.app.registry.get(capability_id) if capability_id else None
        for step in steps:
            action='defensive_readonly' if step.source=='cai' else step.task; policy=self.app.policy.evaluate(action,step.risk,step.source); rec=asdict(step)|{'policy':policy}; self.app.bus.emit('STEP_STARTED',{'task_id':tid,**rec})
            if policy['decision']=='deny': out={'ok':False,'status':'denied','reason':policy['reason']}
            elif policy['decision']=='approval':
                aid=self.create_approval(tid,step,rec); out={'ok':False,'status':'approval_required','approval_id':aid}; results.append(rec|{'result':out}); self.app.bus.emit('APPROVAL_REQUIRED',{'task_id':tid,'approval_id':aid,**rec}); continue
            else:
                try:
                    adapter=self.app.adapters.get(step.source); selected=capability_id if cap_meta and cap_meta.get('source')==step.source else None; out=adapter.execute(step.task+'\n\nOriginal user request: '+request,self.app,step,selected,context[-3:]);
                    if policy['decision']=='isolated': out['policy_isolation']=True
                except Exception as e: out={'ok':False,'status':'error','error':str(e),'trace':traceback.format_exc(limit=3)}
            status='completed' if out.get('ok') else out.get('status','failed'); receipt=self.app.receipts.create(tid,step.id,step.source,status,out); item=rec|{'result':out,'receipt':receipt}; results.append(item); context.append({'source':step.source,'result':out.get('output',out)})
            if step.source=='second_brain' and out.get('ok'):
                try: self.app.memory.add(str(out.get('output','')), title='Pipeline knowledge: '+request[:100], namespace='pipeline', metadata={'task_id':tid,'source':'second_brain'})
                except Exception: pass
            self.app.bus.emit('STEP_FINISHED',{'task_id':tid,'step_id':step.id,'source':step.source,'status':status,'receipt':receipt['hash']})
        pending=any(x.get('result',{}).get('status')=='approval_required' for x in results); failed=any((not x.get('result',{}).get('ok',False)) and x.get('result',{}).get('status')!='approval_required' for x in results); status='waiting_approval' if pending else ('partial' if failed else 'completed'); payload={'task_id':tid,'request':request,'status':status,'steps':results}; self._save_task(tid,request,status,payload,source); self.app.bus.emit('TASK_FINISHED',{'task_id':tid,'status':status}); return payload
    def recent_tasks(self,limit=50):
        with closing(sqlite3.connect(self.app.root/'data/sarus.db')) as c: rows=c.execute("SELECT id,ts,request,status,result,source FROM tasks ORDER BY ts DESC LIMIT ?",(limit,)).fetchall()
        return [{'id':x[0],'ts':x[1],'request':x[2],'status':x[3],'result':json.loads(x[4]) if x[4] else None,'source':x[5]} for x in rows]
