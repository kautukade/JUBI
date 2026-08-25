'use strict';

async function loadSemanticKnowledge(){
  try{
    const [s,docs,stats,exp]=await Promise.all([
      API.get('/api/knowledge/status'),
      API.get('/api/knowledge/documents?limit=100'),
      API.get('/api/experience/stats'),
      API.get('/api/experience?limit=30')
    ]);
    byId('semantic-docs').textContent=s.documents??0;
    byId('semantic-chunks').textContent=s.chunks??0;
    byId('semantic-model').textContent=s.embedding_model||'Missing';
    byId('experience-total').textContent=stats.total??0;
    byId('experience-rate').textContent=stats.success_rate==null?'—':Math.round(stats.success_rate*100)+'%';
    byId('knowledge-documents').innerHTML=(docs||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.title)}</div><div class="data-meta">${esc(x.namespace)} · ${esc(x.source)} · ${x.chunk_count} chunk(s) · ${fmtDate(x.ts)}</div></div><button class="btn small danger" data-knowledge-delete="${esc(x.id)}">Delete</button></div>`).join('')||renderEmpty('No semantic documents yet.');
    byId('experience-list').innerHTML=(exp||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(short(x.request,120))}</div><div class="data-meta">${esc(x.task_type)} · ${esc(x.provider||'local')} · ${esc(x.model||'—')} · ${x.success?'success':'failure'}<br>${esc(short(x.lesson||x.outcome,180))}</div></div>${badge(x.success?'Success':'Failure',x.success?'ok':'bad')}</div>`).join('')||renderEmpty('No experience records yet. Chat with Jubi to build local experience memory.');
    document.querySelectorAll('[data-knowledge-delete]').forEach(b=>b.onclick=()=>deleteKnowledge(b.dataset.knowledgeDelete));
  }catch(e){toast('Semantic knowledge status failed: '+e.message,'bad')}
}

async function ingestKnowledge(){
  const content=byId('semantic-content').value.trim();
  if(!content)return toast('Add knowledge content first','bad');
  const btn=byId('semantic-ingest');setBusy(btn,true,'Embedding');
  try{
    const r=await API.post('/api/knowledge/ingest',{
      title:byId('semantic-title').value||'Untitled knowledge',
      namespace:byId('semantic-ns').value||'general',
      source:byId('semantic-source').value||'manual',content
    });
    jsonBox('semantic-output',r);byId('semantic-content').value='';toast(`Embedded ${r.chunks} chunk(s)`,'ok');await loadSemanticKnowledge();
  }catch(e){jsonBox('semantic-output','Error: '+e.message)}finally{setBusy(btn,false)}
}

async function semanticSearch(){
  const q=byId('semantic-query').value.trim();if(!q)return toast('Enter a semantic query','bad');
  const btn=byId('semantic-search');setBusy(btn,true,'Searching');
  try{
    const r=await API.post('/api/knowledge/search',{query:q,namespace:byId('semantic-query-ns').value||'',limit:8});
    byId('semantic-results').innerHTML=(r||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.title)} · score ${Number(x.score).toFixed(3)}</div><div class="data-meta">${esc(x.namespace)} · ${esc(x.source)} · chunk ${x.ordinal+1}<br>${esc(short(x.text,420))}</div></div></div>`).join('')||renderEmpty('No semantic matches.');
  }catch(e){byId('semantic-results').innerHTML=renderEmpty('Error: '+e.message)}finally{setBusy(btn,false)}
}

async function askKnowledge(){
  const q=byId('rag-question').value.trim();if(!q)return toast('Enter a question','bad');
  const btn=byId('rag-ask');setBusy(btn,true,'Retrieving');
  try{
    const r=await API.post('/api/knowledge/ask',{question:q,namespace:byId('rag-ns').value||'',provider:byId('rag-provider').value||'auto',limit:6});
    byId('rag-answer').textContent=r.answer||'No answer returned.';
    byId('rag-sources').innerHTML=(r.sources||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">[${esc(x.ref)}] ${esc(x.title)}</div><div class="data-meta">${esc(x.source)} · score ${Number(x.score).toFixed(3)} · chunk ${x.ordinal+1}</div></div></div>`).join('')||renderEmpty('No supporting sources.');
    jsonBox('rag-route',r.provider_route||{});
  }catch(e){byId('rag-answer').textContent='Error: '+e.message}finally{setBusy(btn,false)}
}

async function similarExperience(){
  const q=byId('experience-query').value.trim();if(!q)return toast('Enter a task to compare','bad');
  try{
    const r=await API.get('/api/experience/similar?q='+encodeURIComponent(q)+'&limit=8');
    byId('experience-similar').innerHTML=(r||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(short(x.request,130))}</div><div class="data-meta">score ${Number(x.score).toFixed(3)} · ${x.success?'success':'failure'} · ${esc(x.provider||'local')} / ${esc(x.model||'—')}<br>${esc(short(x.lesson||x.outcome,220))}</div></div></div>`).join('')||renderEmpty('No similar experiences yet.');
  }catch(e){toast(e.message,'bad')}
}

async function deleteKnowledge(id){if(!confirm('Delete this semantic knowledge document and its vectors?'))return;try{await API.post('/api/knowledge/delete',{id});toast('Knowledge deleted','ok');await loadSemanticKnowledge()}catch(e){toast(e.message,'bad')}}

document.addEventListener('DOMContentLoaded',()=>{
  const bind=(id,fn)=>{const el=byId(id);if(el)el.onclick=fn};
  bind('semantic-ingest',ingestKnowledge);bind('semantic-search',semanticSearch);bind('rag-ask',askKnowledge);bind('experience-search',similarExperience);bind('semantic-refresh',loadSemanticKnowledge);
  loadSemanticKnowledge();
});
