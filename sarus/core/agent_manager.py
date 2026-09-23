from __future__ import annotations
import json, time, uuid
from .database import read_connection, transaction

class AgentManager:
    def __init__(self, app):
        self.app=app; self.db=app.db_path
        with transaction(self.db) as c:
            c.execute('CREATE TABLE IF NOT EXISTS agent_manager_runs(id TEXT PRIMARY KEY,ts REAL,request TEXT,task_type TEXT,status TEXT,plan TEXT,result TEXT,error TEXT)')
    def run(self, request, task_type='auto', project_path='', provider='ollama', model=None):
        request=str(request or '').strip()
        if not request: raise ValueError('agent request is required')
        run_id=str(uuid.uuid4()); started=time.time(); status='failed'; error=''; result={}; plan={}
        try:
            classification=self.app.brain.classify(request,task_type)
            actual=classification.get('task_type') or 'general'
            try: plan=self.app.supervisor.plan(request,actual,provider)
            except Exception as exc: plan={'status':'planning_fallback','error':str(exc)}
            if actual=='coding':
                if not str(project_path or '').strip(): raise ValueError('coding agent requires project_path inside an approved Jubi workspace')
                result=self.app.development.run(request,project_path,provider,model)
                status='completed' if result.get('status')=='completed' else 'failed'
            elif actual=='research':
                result=self.app.research.research(request,5,provider)
                status='completed' if result.get('answer') else 'failed'
            else:
                generated=self.app.providers.generate(request,task_type=actual,provider=provider,model=model,timeout=300)
                result={'answer':str(generated.get('response') or generated.get('output') or ''),'route':generated.get('jubi_provider_route') or generated.get('jubi_route') or {}}
                status='completed' if result['answer'] else 'failed'
        except Exception as exc: error=str(exc)
        record={'id':run_id,'status':status,'request':request,'task_type':locals().get('actual',task_type),'plan':plan,'result':result,'error':error,'elapsed_ms':round((time.time()-started)*1000,2)}
        with transaction(self.db) as c:
            c.execute('INSERT INTO agent_manager_runs VALUES(?,?,?,?,?,?,?,?)',(run_id,started,request,record['task_type'],status,json.dumps(plan,default=str),json.dumps(result,default=str),error[:4000]))
        self.app.bus.emit('AGENT_MANAGER_FINISHED',{'id':run_id,'status':status,'task_type':record['task_type']})
        return record
    def recent(self,limit=30):
        limit=max(1,min(int(limit),100))
        with read_connection(self.db) as c: rows=c.execute('SELECT id,ts,request,task_type,status,plan,result,error FROM agent_manager_runs ORDER BY ts DESC LIMIT ?',(limit,)).fetchall()
        return [{'id':r[0],'ts':r[1],'request':r[2],'task_type':r[3],'status':r[4],'plan':json.loads(r[5] or '{}'),'result':json.loads(r[6] or '{}'),'error':r[7] or ''} for r in rows]
