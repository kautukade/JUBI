'use strict';

async function runCouncil(){
  const text=byId('council-input').value.trim();if(!text)return toast('Enter a problem for the Council','bad');
  const btn=byId('council-run');setBusy(btn,true,'Deliberating');
  try{
    const r=await API.post('/api/council/run',{text,task_type:byId('council-task-type').value||'auto',max_members:Number(byId('council-members').value||4),judge_provider:byId('council-judge').value||'auto'});
    byId('council-final').textContent=r.final||'No final answer.';
    byId('council-member-results').innerHTML=(r.members||[]).map(x=>`<div class="card" style="padding:14px"><div class="split between"><strong>${esc(x.label||('Member '+x.member))}</strong>${badge((x.provider||'')+' / '+(x.model||''),'info')}</div><div style="white-space:pre-wrap;margin-top:10px">${esc(x.answer)}</div></div>`).join('')||renderEmpty('No successful Council members.');
    jsonBox('council-route',{classification:r.classification,judge_route:r.judge_route,errors:r.errors,latency_ms:r.latency_ms});
    await loadCouncilHistory();
  }catch(e){byId('council-final').textContent='Error: '+e.message}finally{setBusy(btn,false)}
}

async function supervisorAction(run){
  const text=byId('supervisor-input').value.trim();if(!text)return toast('Describe the complex task','bad');
  const btn=byId(run?'supervisor-run':'supervisor-plan');setBusy(btn,true,run?'Supervising':'Planning');
  try{
    const r=await API.post(run?'/api/supervisor/run':'/api/supervisor/plan',{text,task_type:'auto',provider:byId('supervisor-provider').value||'auto'});
    jsonBox('supervisor-output',r);
    if(run && r.final)byId('supervisor-final').textContent=r.final;
    await loadSupervisorHistory();
  }catch(e){jsonBox('supervisor-output','Error: '+e.message)}finally{setBusy(btn,false)}
}

async function loadCouncilHistory(){
  try{
    const rows=await API.get('/api/council?limit=20');
    byId('council-history').innerHTML=(rows||[]).map(x=>`<tr><td>${fmtDate(x.ts)}</td><td>${esc(x.task_type)}</td><td>${x.success_count}/${x.member_count}</td><td>${esc(x.judge_provider)}</td><td>${esc(x.judge_model)}</td><td>${fmtMs(x.latency_ms)}</td></tr>`).join('')||'<tr><td colspan="6">No Council runs yet.</td></tr>';
  }catch(e){console.error(e)}
}
async function loadSupervisorHistory(){
  try{
    const rows=await API.get('/api/supervisor?limit=20');
    byId('supervisor-history').innerHTML=(rows||[]).map(x=>`<tr><td>${fmtDate(x.ts)}</td><td>${badge(x.status,statusTone(x.status))}</td><td>${esc(x.task_type)}</td><td>${x.step_count}</td><td>${esc(x.provider)}</td><td>${esc(x.model)}</td><td>${fmtMs(x.latency_ms)}</td></tr>`).join('')||'<tr><td colspan="7">No Supervisor runs yet.</td></tr>';
  }catch(e){console.error(e)}
}

document.addEventListener('DOMContentLoaded',()=>{
  const a=byId('council-run');if(a)a.onclick=runCouncil;
  const p=byId('supervisor-plan');if(p)p.onclick=()=>supervisorAction(false);
  const r=byId('supervisor-run');if(r)r.onclick=()=>supervisorAction(true);
  loadCouncilHistory();loadSupervisorHistory();
});
