'use strict';

function researchSourceRows(rows){
  return (rows||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${x.ref?'['+esc(x.ref)+'] ':''}${esc(x.title||x.url)}</div><div class="data-meta">${esc(x.url||'')}${x.chars?' · '+Number(x.chars).toLocaleString()+' chars':''}</div></div></div>`).join('')||renderEmpty('No sources yet.');
}

async function searchResearch(){
  const q=byId('research-query').value.trim();if(!q)return toast('Enter a research query','bad');
  const btn=byId('research-search');setBusy(btn,true,'Searching');
  try{
    const rows=await API.post('/api/research/search',{query:q,limit:10});
    byId('research-sources').innerHTML=researchSourceRows(rows);
    byId('research-answer').textContent='Search completed. Use Research + synthesize to read public sources and create an evidence-grounded answer.';
    jsonBox('research-route',{search_results:(rows||[]).length});
  }catch(e){byId('research-answer').textContent='Error: '+e.message}finally{setBusy(btn,false)}
}

async function runResearch(){
  const q=byId('research-query').value.trim();if(!q)return toast('Enter a research query','bad');
  const btn=byId('research-run');setBusy(btn,true,'Researching');
  try{
    const r=await API.post('/api/research/run',{query:q,max_sources:Number(byId('research-source-count').value||5),provider:byId('research-provider').value||'auto'});
    byId('research-answer').textContent=r.answer||'No answer returned.';
    byId('research-sources').innerHTML=researchSourceRows(r.sources||[]);
    jsonBox('research-route',{route:r.route,errors:r.errors,latency_ms:r.latency_ms});
    await loadResearchHistory();
  }catch(e){byId('research-answer').textContent='Error: '+e.message}finally{setBusy(btn,false)}
}

async function loadResearchHistory(){
  try{
    const rows=await API.get('/api/research?limit=30');
    byId('research-history').innerHTML=(rows||[]).map(x=>`<tr><td>${fmtDate(x.ts)}</td><td>${badge(x.status,statusTone(x.status))}</td><td>${x.source_count}</td><td>${esc(x.provider||'')}</td><td>${esc(x.model||'')}</td><td>${fmtMs(x.latency_ms)}</td></tr>`).join('')||'<tr><td colspan="6">No research runs yet.</td></tr>';
  }catch(e){console.error(e)}
}

document.addEventListener('DOMContentLoaded',()=>{
  const title=document.querySelector('.page-title');if(title)title.textContent='Web Research';
  const sub=document.querySelector('.page-subtitle');if(sub)sub.textContent='Public search, safe page reading and source-grounded synthesis';
  const nav=document.querySelector('.nav-scroll');if(nav && !nav.querySelector('[data-research-nav]')){
    const a=document.createElement('a');a.className='nav-link active';a.href='/research.html';a.dataset.researchNav='1';a.innerHTML='<span class="nav-icon">⌕</span><span>Web Research</span>';nav.appendChild(a);
  }
  const s=byId('research-search');if(s)s.onclick=searchResearch;
  const r=byId('research-run');if(r)r.onclick=runResearch;
  const f=byId('research-refresh');if(f)f.onclick=loadResearchHistory;
  loadResearchHistory();
});
