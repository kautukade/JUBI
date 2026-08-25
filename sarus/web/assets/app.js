'use strict';

const JUBI_NAV = [
  {section:'Workspace',items:[['overview','/','Overview','⌂'],['chat','/chat.html','AI Chat','✦'],['tasks','/tasks.html','Tasks & Planner','✓']]},
  {section:'Intelligence',items:[['brain','/brain.html','Brain & Router','◉'],['providers','/providers.html','Providers','☁'],['models','/models.html','Models','◈'],['agents','/agents.html','Agents & Capabilities','◎'],['development','/development.html','Development','⌘'],['knowledge','/knowledge.html','Knowledge','◇'],['fable','/fable.html','Fable Lab','⬡']]},
  {section:'Operations',items:[['automation','/automation.html','Automation','↻'],['computer','/computer.html','Computer','▣']]},
  {section:'Trust & System',items:[['security','/security.html','Security & Receipts','◆'],['health','/health.html','System Health','♡'],['activity','/activity.html','Activity','≡']]}
];

const PAGE_META = {
  overview:['Overview','Live runtime, tasks, sources and trust status'],
  chat:['AI Chat','Privacy-aware local and optional cloud conversations'],
  tasks:['Tasks & Planner','Plan, run and inspect persisted execution pipelines'],
  brain:['Brain & Router','Automatic task classification and adaptive local model ranking'],
  providers:['Provider Manager','Secure OpenRouter, NVIDIA NIM and Hugging Face routing'],
  models:['Models','Inspect and test models discovered from Ollama'],
  agents:['Agents & Capabilities','Search connected source capabilities and execute supported units'],
  development:['Development','Run the existing development-oriented agent pipeline'],
  knowledge:['Knowledge','Search and save local persistent memory'],
  fable:['Fable Lab','Learned capabilities, bounded agenda and isolated research tools'],
  automation:['Automation','Create and control persisted recurring workflows'],
  computer:['Computer','Typed allowlisted Windows and workspace operations'],
  security:['Security & Receipts','Approvals, broker posture and signed receipt evidence'],
  health:['System Health','Run Jubi Doctor and inspect runtime readiness'],
  activity:['Activity','Inspect the persistent local event bus']
};

let _session = '';
const API = {
  async json(url, options) {
    const r = await fetch(url, options);
    let x;
    try { x = await r.json(); } catch { throw new Error(`Invalid JSON from ${url}`); }
    if (!r.ok && r.status !== 423) throw new Error(x.error || `HTTP ${r.status}`);
    return x;
  },
  async token() {
    if (!_session) _session = (await this.json('/api/session')).token;
    return _session;
  },
  get(url) { return this.json(url); },
  async post(url, body, headers = {}) {
    return this.json(url, {
      method:'POST',
      headers:{'Content-Type':'application/json','X-JUBI-Token':await this.token(),...headers},
      body:JSON.stringify(body || {})
    });
  }
};

function esc(v){return String(v??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
function byId(id){return document.getElementById(id);}
function fmtDate(v){if(!v)return '—';try{const n=Number(v);return new Date(n>1000000000&&n<100000000000?n*1000:v).toLocaleString();}catch{return String(v);}}
function fmtBytes(v){const n=Number(v||0);if(!n)return '—';const u=['B','KB','MB','GB','TB'];let i=0,x=n;while(x>=1024&&i<u.length-1){x/=1024;i++;}return `${x.toFixed(i>1?2:1)} ${u[i]}`;}
function fmtMs(v){const n=Number(v);if(!Number.isFinite(n))return '—';return n>=1000?`${(n/1000).toFixed(2)} s`:`${n.toFixed(0)} ms`;}
function short(v,n=96){const s=String(v??'');return s.length>n?s.slice(0,n)+'…':s;}
function badge(text,tone='neutral'){return `<span class="badge ${tone}">${esc(text)}</span>`;}
function statusTone(s){s=String(s||'').toLowerCase();if(['ok','online','connected','completed','success','enabled','ready','running','verified'].some(x=>s.includes(x)))return 'ok';if(['fail','error','offline','missing','denied','rejected','invalid'].some(x=>s.includes(x)))return 'bad';if(['pending','waiting','approval','partial','setup','paused','configured'].some(x=>s.includes(x)))return 'warn';return 'info';}
function toast(msg,tone=''){let w=byId('toast-wrap');if(!w){w=document.createElement('div');w.id='toast-wrap';w.className='toast-wrap';document.body.appendChild(w);}const d=document.createElement('div');d.className=`toast ${tone}`;d.textContent=msg;w.appendChild(d);setTimeout(()=>d.remove(),3600);}
function setBusy(btn,busy,label='Working…'){if(!btn)return;if(busy){btn.dataset.old=btn.innerHTML;btn.disabled=true;btn.innerHTML=`<span class="loader"></span>${esc(label)}`;}else{btn.disabled=false;if(btn.dataset.old)btn.innerHTML=btn.dataset.old;}}
function jsonBox(id,data){const el=byId(id);if(el)el.textContent=typeof data==='string'?data:JSON.stringify(data,null,2);}
function renderEmpty(text='No data yet.'){return `<div class="empty">${esc(text)}</div>`;}

function mountShell(){
  const page=document.body.dataset.page||'overview';
  const meta=PAGE_META[page]||PAGE_META.overview;
  const shell=document.createElement('div');
  shell.className='app-shell';
  const existing=[...document.body.children];
  shell.innerHTML=`<aside class="sidebar"><div class="brandbox"><div class="brandrow"><div class="brandmark">J</div><div><div class="brandname">JUBI</div><div class="brandsub">Local AI Platform</div></div></div></div><nav class="nav-scroll">${JUBI_NAV.map(g=>`<div class="nav-section">${esc(g.section)}</div>${g.items.map(i=>`<a class="nav-link ${i[0]===page?'active':''}" href="${i[1]}"><span class="nav-icon">${i[3]}</span><span>${esc(i[2])}</span></a>`).join('')}`).join('')}</nav><div class="sidebar-foot"><div>Jubi Provider Manager</div><div style="margin-top:3px">Localhost-only • Privacy-first routing</div></div></aside><div class="mainwrap"><header class="topbar"><div class="top-left"><button class="mobile-toggle" id="mobile-toggle">☰</button><div><div class="page-title">${esc(meta[0])}</div><div class="page-subtitle">${esc(meta[1])}</div></div></div><div class="top-status"><span class="status-pill" id="global-mode"><span class="dot ok"></span>LOCAL ONLY</span><span class="status-pill" id="global-ollama"><span class="dot warn"></span>OLLAMA…</span></div></header><div id="page-slot"></div></div>`;
  document.body.innerHTML='';document.body.appendChild(shell);
  const slot=byId('page-slot');existing.forEach(n=>slot.appendChild(n));
  byId('mobile-toggle').onclick=()=>document.body.classList.toggle('nav-open');
  document.addEventListener('click',e=>{if(document.body.classList.contains('nav-open')&&!e.target.closest('.sidebar')&&!e.target.closest('#mobile-toggle'))document.body.classList.remove('nav-open');});
  loadGlobalStatus();
}

async function loadGlobalStatus(){
  const ollamaEl=byId('global-ollama'),modeEl=byId('global-mode');
  try{
    const [models,providers]=await Promise.all([API.get('/api/models'),API.get('/api/providers')]);
    if(ollamaEl){const on=!!models.online;ollamaEl.innerHTML=`<span class="dot ${on?'ok':'bad'}"></span>OLLAMA ${on?'ONLINE':'OFFLINE'}`;ollamaEl.title=on?`${(models.models||[]).length} model(s) detected`:'Local Ollama is not reachable';}
    if(modeEl){const mode=String(providers.mode||'local_only');const label=mode.replaceAll('_',' ').toUpperCase();modeEl.innerHTML=`<span class="dot ${mode==='local_only'?'ok':'warn'}"></span>${esc(label)}`;}
  }catch{
    if(ollamaEl)ollamaEl.innerHTML='<span class="dot bad"></span>BACKEND ERROR';
  }
}

async function initOverview(){
  const [s,t,e,a,r]=await Promise.all([API.get('/api/status'),API.get('/api/tasks?limit=8'),API.get('/api/events?limit=12'),API.get('/api/approvals?status=pending'),API.get('/api/receipts?limit=5')]);
  const adapters=s.adapters||[];let files=0,units=0;
  Object.values(s.capabilities||{}).forEach(x=>{files+=Number(x.files||0);units+=Number(x.agents||0)+Number(x.skills||0)+Number(x.tools||0)+Number(x.commands||0);});
  byId('metric-sources').textContent=`${adapters.filter(x=>x.connected).length}/${adapters.length}`;
  byId('metric-models').textContent=(s.models?.models||[]).length;
  byId('metric-units').textContent=units.toLocaleString();byId('metric-approvals').textContent=(a||[]).length;byId('metric-files').textContent=files.toLocaleString();byId('metric-chain').textContent=s.receipt_chain?.ok?'VERIFIED':'CHECK';
  byId('runtime-summary').innerHTML=`${badge(s.name+' '+s.version,'info')} ${badge(s.models?.online?'Ollama online':'Ollama offline',s.models?.online?'ok':'bad')} ${badge((s.providers?.mode||'local_only').replaceAll('_',' '),'info')} ${badge(s.windows_broker?'Windows broker ready':'Windows broker unavailable',s.windows_broker?'ok':'warn')} ${badge(s.receipt_chain?.ok?'Receipt chain verified':'Receipt chain issue',s.receipt_chain?.ok?'ok':'bad')}`;
  byId('sources-list').innerHTML=adapters.map(x=>`<div class="source-card"><div class="split between"><strong>${esc(x.details?.label||x.name)}</strong>${badge(x.connected?'Connected':'Missing',x.connected?'ok':'bad')}</div><div class="data-meta">${esc(x.details?.role||'Source adapter')}</div></div>`).join('')||renderEmpty('No source adapters found.');
  byId('recent-tasks').innerHTML=(t||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(short(x.request,100))}</div><div class="data-meta">${fmtDate(x.created_at||x.ts)} · ${esc(x.id||x.task_id||'')}</div></div>${badge(x.status,statusTone(x.status))}</div>`).join('')||renderEmpty('No tasks yet.');
  byId('recent-events').innerHTML=(e||[]).slice().reverse().map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.kind)}</div><div class="data-meta">${esc(short(JSON.stringify(x.payload||{}),150))}</div></div><span class="small muted nowrap">${fmtDate(x.created_at||x.ts)}</span></div>`).join('')||renderEmpty('No events yet.');
  byId('receipt-summary').textContent=`${r.chain?.ok?'Verified chain':'Chain check failed'} · ${(r.items||[]).length} recent receipt(s)`;
}

let chatTranscript=[];
let chatLocalModels=[];
let chatProviderStatus=null;
async function initChat(){
  const [local,providers]=await Promise.all([API.get('/api/models'),API.get('/api/providers')]);
  chatLocalModels=local.items||[];chatProviderStatus=providers;
  const psel=byId('chat-provider');
  if(psel){psel.value='auto';psel.onchange=loadChatModels;}
  const mode=String(providers.mode||'local_only');
  const modeEl=byId('chat-provider-mode');
  if(modeEl)modeEl.innerHTML=`Current mode: <b>${esc(mode.replaceAll('_',' ').toUpperCase())}</b>. High-privacy requests remain local by default.`;
  await loadChatModels();renderChat();
  byId('chat-send').onclick=sendChat;byId('chat-clear').onclick=()=>{chatTranscript=[];renderChat();};
  byId('chat-input').addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendChat();}});
}
async function loadChatModels(){
  const provider=byId('chat-provider')?.value||'auto';const sel=byId('chat-model');if(!sel)return;
  sel.innerHTML='<option value="">Smart auto-select</option>';
  if(provider==='auto'||provider==='ollama'){
    sel.innerHTML+=[...chatLocalModels].filter(x=>x.kind!=='embedding').map(x=>`<option value="${esc(x.name)}">${esc(x.name)} · ${esc(x.kind||'unknown')}</option>`).join('');
    return;
  }
  try{
    const data=await API.get('/api/providers/models?provider='+encodeURIComponent(provider));
    if(!data.configured){sel.innerHTML='<option value="">Provider not configured</option>';return;}
    const rows=(data.models||[]).slice(0,500);
    sel.innerHTML='<option value="">Provider smart default</option>'+rows.map(x=>`<option value="${esc(x.id)}">${esc(x.name||x.id)}${x.free?' · free':''}</option>`).join('');
  }catch(e){sel.innerHTML='<option value="">Model discovery unavailable</option>';}
}
function renderChat(){const el=byId('chat-messages');el.innerHTML=chatTranscript.length?chatTranscript.map(m=>`<div class="message ${m.role}">${esc(m.text)}</div>`).join(''):`<div class="message system">Jubi can stay fully local or use configured cloud providers according to Provider Manager mode, task complexity and privacy policy.</div>`;el.scrollTop=el.scrollHeight;}
async function sendChat(){
  const input=byId('chat-input'),text=input.value.trim();if(!text)return;const btn=byId('chat-send');
  chatTranscript.push({role:'user',text});renderChat();input.value='';setBusy(btn,true,'Thinking');
  try{
    const body={text,task_type:byId('chat-type').value||'auto',provider:byId('chat-provider')?.value||'auto'};
    if(byId('chat-model').value)body.model=byId('chat-model').value;
    const r=await API.post('/api/chat',body);chatTranscript.push({role:'assistant',text:r.response||r.output||JSON.stringify(r,null,2)});renderChat();
    const pr=r.jubi_provider_route||{},br=r.jubi_route||{};
    const provider=pr.provider||'ollama',model=pr.selected_model||br.selected_model||r.model||body.model||'Auto-selected';
    byId('chat-used-model').textContent=`${provider} · ${model}`;
    const routeInfo=byId('chat-route-info');if(routeInfo)routeInfo.textContent=`${pr.mode||chatProviderStatus?.mode||'local_only'} · ${pr.intent||br.intent||'general'} · complexity ${pr.complexity||br.complexity||1}/5 · ${pr.cloud?'cloud':'local'}`;
  }catch(e){chatTranscript.push({role:'system',text:'Error: '+e.message});renderChat();}
  finally{setBusy(btn,false);}
}

async function initTasks(){byId('task-plan').onclick=()=>taskAction(false);byId('task-run').onclick=()=>taskAction(true);byId('tasks-refresh').onclick=loadTaskLists;await loadTaskLists();}
async function taskAction(run){const text=byId('task-input').value.trim();if(!text)return toast('Enter a task first','bad');const btn=byId(run?'task-run':'task-plan');setBusy(btn,true,run?'Running':'Planning');try{const r=await API.post(run?'/api/task':'/api/plan',{text});jsonBox('task-output',r);toast(run?'Task submitted':'Plan created','ok');await loadTaskLists();}catch(e){jsonBox('task-output','Error: '+e.message);}finally{setBusy(btn,false);}}
async function loadTaskLists(){const [t,a]=await Promise.all([API.get('/api/tasks?limit=50'),API.get('/api/approvals?status=pending')]);byId('tasks-table').innerHTML=(t||[]).map(x=>`<tr><td>${badge(x.status,statusTone(x.status))}</td><td>${esc(short(x.request,150))}</td><td class="mono">${esc(x.id||x.task_id||'')}</td><td>${fmtDate(x.created_at||x.ts)}</td></tr>`).join('')||'<tr><td colspan="4">No tasks yet.</td></tr>';byId('task-approvals').innerHTML=(a||[]).map(x=>`<div class="data-row"><div><div class="data-title">${esc(x.action||x.step_id||'Pending step')}</div><div class="data-meta mono">${esc(x.id)}</div></div>${badge('Pending','warn')}</div>`).join('')||renderEmpty('No pending approvals.');}

async function initBrain(){byId('brain-refresh').onclick=loadBrain;byId('brain-route').onclick=inspectBrainRoute;await loadBrain();}
async function loadBrain(){const s=await API.get('/api/brain');byId('brain-mode').textContent=String(s.mode||'local-first').toUpperCase();byId('brain-models').textContent=s.detected_models??0;byId('brain-pairs').textContent=s.tracked_model_task_pairs??0;byId('brain-decisions').textContent=s.recorded_decisions??0;const perf=s.performance||[];byId('brain-performance').innerHTML=perf.map(x=>`<tr><td><b>${esc(x.model)}</b></td><td>${badge(x.task_type,'info')}</td><td>${x.attempts??0}</td><td>${x.success_rate==null?'—':(x.success_rate*100).toFixed(0)+'%'}</td><td>${fmtMs(x.avg_success_latency_ms)}</td><td>${badge(x.last_status||'—',statusTone(x.last_status))}</td></tr>`).join('')||'<tr><td colspan="6">No measured model outcomes yet. Use AI Chat to build local performance history.</td></tr>';const hist=s.recent_decisions||[];byId('brain-history').innerHTML=hist.map(x=>`<tr><td>${badge(x.status,statusTone(x.status))}</td><td>${esc(x.intent)}</td><td>${x.complexity}/5</td><td>${esc(x.selected_model||'—')}</td><td>${fmtMs(x.latency_ms)}</td><td>${fmtDate(x.ts)}</td></tr>`).join('')||'<tr><td colspan="6">No completed routing decisions yet.</td></tr>';}
async function inspectBrainRoute(){const text=byId('brain-route-text').value.trim();if(!text)return toast('Enter a task to analyze','bad');const btn=byId('brain-route');setBusy(btn,true,'Analyzing');try{const r=await API.post('/api/brain/route',{text,task_type:'auto'});jsonBox('brain-route-output',r);}catch(e){jsonBox('brain-route-output','Error: '+e.message);}finally{setBusy(btn,false);}}

async function initProviders(){
  byId('providers-refresh').onclick=()=>loadProviders(true);
  byId('provider-mode-save').onclick=saveProviderMode;
  byId('provider-model-refresh').onclick=()=>loadProviderModels(true);
  byId('provider-model-provider').onchange=()=>loadProviderModels(false);
  byId('provider-default-save').onclick=saveProviderDefault;
  document.querySelectorAll('[data-provider-save]').forEach(b=>b.onclick=()=>saveProviderCredential(b.dataset.providerSave,b));
  document.querySelectorAll('[data-provider-test]').forEach(b=>b.onclick=()=>validateProvider(b.dataset.providerTest,b));
  document.querySelectorAll('[data-provider-delete]').forEach(b=>b.onclick=()=>deleteProviderCredential(b.dataset.providerDelete,b));
  await loadProviders(false);
}
function providerModeNote(mode,threshold){if(mode==='local_only')return 'Local Only never transmits prompts to OpenRouter, NVIDIA or Hugging Face.';if(mode==='hybrid_auto')return `Hybrid Auto keeps simple requests local and may prefer cloud from complexity ${threshold}/5; failures can fall back across available routes.`;return 'Cloud Boost prefers configured cloud providers for non-sensitive prompts and falls back to local Ollama if needed.';}
async function loadProviders(validate){
  const s=await API.get('/api/providers?validate='+(validate?'1':'0'));
  byId('provider-mode').value=s.mode||'local_only';byId('provider-mode-note').textContent=providerModeNote(s.mode, s.hybrid_cloud_complexity_threshold||4);
  byId('provider-ollama-status').textContent=s.local?.online?'ONLINE':'OFFLINE';byId('provider-ollama-status').className=s.local?.online?'metric-value good':'metric-value danger-text';byId('provider-ollama-models').textContent=`${s.local?.models||0} local model(s)`;
  const cloud=s.cloud||[];byId('provider-cloud-count').textContent=cloud.filter(x=>x.configured).length;byId('provider-request-count').textContent=(s.recent_requests||[]).length;byId('provider-vault').textContent=s.credential_storage?.windows_dpapi?'DPAPI READY':'ENV ONLY';
  for(const p of cloud){const b=byId(`provider-${p.provider}-badge`),m=byId(`provider-${p.provider}-meta`);if(b){b.className=`badge ${statusTone(p.status)}`;b.textContent=p.status||'unknown';}if(m)m.textContent=`Credential: ${p.configured?'configured':'missing'} · source: ${p.credential_source||'none'}${p.models!=null?' · models: '+p.models:''}${p.latency_ms!=null?' · '+fmtMs(p.latency_ms):''}${p.error?' · '+short(p.error,100):''}`;}
  const perf=s.performance||[];byId('provider-performance').innerHTML=perf.map(x=>`<tr><td>${badge(x.provider,'info')}</td><td>${esc(short(x.model,45))}</td><td>${esc(x.task_type)}</td><td>${x.attempts||0}</td><td>${x.success_rate==null?'—':(x.success_rate*100).toFixed(0)+'%'}</td><td>${fmtMs(x.avg_success_latency_ms)}</td></tr>`).join('')||'<tr><td colspan="6">No cloud provider outcomes yet.</td></tr>';
  const hist=s.recent_requests||[];byId('provider-history').innerHTML=hist.map(x=>`<tr><td>${badge(x.status,statusTone(x.status))}</td><td>${esc(x.provider)}</td><td>${esc(short(x.model,38))}</td><td>${esc(x.task_type)}</td><td>${fmtMs(x.latency_ms)}</td><td>${fmtDate(x.ts)}</td></tr>`).join('')||'<tr><td colspan="6">No cloud provider routes yet.</td></tr>';
  await loadGlobalStatus();
}
async function saveProviderMode(){const btn=byId('provider-mode-save');setBusy(btn,true,'Saving');try{const r=await API.post('/api/providers/mode',{mode:byId('provider-mode').value});toast('Provider mode saved: '+r.mode,'ok');await loadProviders(false);}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function saveProviderCredential(provider,btn){const input=byId(`provider-key-${provider}`);let key=input?.value.trim()||'';if(!key)return toast('Enter the API key/token first','bad');setBusy(btn,true,'Encrypting');try{await API.post('/api/provider/credential',{provider,api_key:key});if(input)input.value='';key='';toast(`${provider} credential saved securely`,'ok');await loadProviders(true);}catch(e){if(input)input.value='';key='';toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function deleteProviderCredential(provider,btn){if(!confirm(`Remove the saved ${provider} credential from Jubi? Environment-provided credentials cannot be removed from this page.`))return;setBusy(btn,true,'Removing');try{const r=await API.post('/api/provider/credential/delete',{provider});toast(r.configured?`${provider} is still configured via ${r.source}`:`${provider} saved credential removed`,'ok');await loadProviders(false);}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function validateProvider(provider,btn){setBusy(btn,true,'Validating');try{const r=await API.post('/api/provider/validate',{provider});toast(`${provider}: ${r.status}`,r.online?'ok':'bad');await loadProviders(false);}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function loadProviderModels(force){const provider=byId('provider-model-provider').value;const btn=byId('provider-model-refresh');if(force)setBusy(btn,true,'Loading');try{const data=await API.get('/api/providers/models?provider='+encodeURIComponent(provider)+'&force='+(force?'1':'0'));const rows=(data.models||[]).slice(0,300);byId('provider-model-summary').textContent=data.online?`${data.count||rows.length} model(s) discovered from ${provider}. Showing up to ${rows.length}.`:`${provider}: ${data.error||'model discovery unavailable'}`;byId('provider-model-table').innerHTML=rows.map(x=>`<tr><td><b>${esc(x.name||x.id)}</b><div class="data-meta mono">${esc(x.id)}</div></td><td>${esc(x.context_length||'—')}</td><td>${x.free?badge('Yes','ok'):badge('No/unknown','neutral')}</td><td class="small">${esc(short(JSON.stringify(x.providers||x.architecture||{}),120))}</td></tr>`).join('')||'<tr><td colspan="4">No models available.</td></tr>';byId('provider-default-model').innerHTML='<option value="">Select discovered model</option>'+rows.map(x=>`<option value="${esc(x.id)}">${esc(x.name||x.id)}${x.free?' · free':''}</option>`).join('');}catch(e){byId('provider-model-summary').textContent='Error: '+e.message;}finally{if(force)setBusy(btn,false);}}
async function saveProviderDefault(){const provider=byId('provider-model-provider').value,task_type=byId('provider-default-task').value,model=byId('provider-default-model').value;if(!model)return toast('Select a discovered model first','bad');const btn=byId('provider-default-save');setBusy(btn,true,'Saving');try{await API.post('/api/provider/default-model',{provider,task_type,model});toast(`Default saved for ${provider} / ${task_type}`,'ok');}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}

async function initModels(){byId('models-refresh').onclick=loadModelsPage;byId('model-test').onclick=testModel;await loadModelsPage();}
async function loadModelsPage(){const s=await API.get('/api/models');const items=s.items||[];byId('model-count').textContent=items.length;byId('model-online').textContent=s.online?'ONLINE':'OFFLINE';byId('model-online').className=s.online?'metric-value good':'metric-value danger-text';byId('model-table').innerHTML=items.map(m=>`<tr><td><b>${esc(m.name)}</b></td><td>${badge(m.kind||'unknown','info')}</td><td>${fmtBytes(m.size)}</td><td>${fmtDate(m.modified_at)}</td></tr>`).join('')||'<tr><td colspan="4">No Ollama models detected.</td></tr>';byId('model-select').innerHTML='<option value="">Smart auto-select</option>'+items.filter(m=>m.kind!=='embedding').map(m=>`<option value="${esc(m.name)}">${esc(m.name)}</option>`).join('');}
async function testModel(){const text=byId('model-prompt').value.trim();if(!text)return toast('Enter a test prompt','bad');const btn=byId('model-test');setBusy(btn,true,'Testing');try{const body={text,task_type:byId('model-type').value,provider:'ollama'};if(byId('model-select').value)body.model=byId('model-select').value;const r=await API.post('/api/chat',body);jsonBox('model-output',r);}catch(e){jsonBox('model-output','Error: '+e.message);}finally{setBusy(btn,false);}}

async function initAgents(){byId('cap-search-btn').onclick=loadCapabilities;byId('cap-run-btn').onclick=runCapability;const s=await API.get('/api/status');byId('cap-source').innerHTML='<option value="">All sources</option>'+(s.adapters||[]).map(a=>`<option value="${esc(a.name)}">${esc(a.details?.label||a.name)}</option>`).join('');byId('agent-sources').innerHTML=(s.adapters||[]).map(a=>`<div class="source-card"><div class="split between"><strong>${esc(a.details?.label||a.name)}</strong>${badge(a.connected?'Connected':'Missing',a.connected?'ok':'bad')}</div><div class="data-meta">${esc(a.details?.role||'')}</div></div>`).join('');await loadCapabilities();}
let selectedCapability='';
async function loadCapabilities(){const qs=new URLSearchParams({q:byId('cap-query').value,source:byId('cap-source').value,limit:'100'});const rows=await API.get('/api/capabilities?'+qs);byId('cap-list').innerHTML=rows.map(x=>`<button class="data-row" style="width:100%;color:inherit;text-align:left" onclick="selectCapability('${esc(x.id)}')"><div class="data-main"><div class="data-title">${esc(x.name)}</div><div class="data-meta">${esc(x.source)} · ${esc(x.path)}</div></div>${badge(x.kind||'capability','info')}</button>`).join('')||renderEmpty('No capabilities matched.');byId('cap-results').textContent=rows.length;}
async function selectCapability(id){selectedCapability=id;const r=await API.get('/api/capability?id='+encodeURIComponent(id));byId('cap-detail').textContent=JSON.stringify(r,null,2);byId('cap-run-btn').disabled=false;}
async function runCapability(){if(!selectedCapability)return;const btn=byId('cap-run-btn');setBusy(btn,true,'Running');try{const r=await API.post('/api/capability/run',{id:selectedCapability,text:byId('cap-run-text').value||'Use this capability for its intended purpose.'});jsonBox('cap-run-output',r);}catch(e){jsonBox('cap-run-output','Error: '+e.message);}finally{setBusy(btn,false);}}
window.selectCapability=selectCapability;

async function initDevelopment(){byId('dev-plan').onclick=()=>devAction(false);byId('dev-run').onclick=()=>devAction(true);await loadDevTasks();}
async function devAction(run){const text=byId('dev-input').value.trim();if(!text)return toast('Describe the development task','bad');const prompt='Development task: '+text;const btn=byId(run?'dev-run':'dev-plan');setBusy(btn,true,run?'Running':'Planning');try{const r=await API.post(run?'/api/task':'/api/plan',{text:prompt});jsonBox('dev-output',r);await loadDevTasks();}catch(e){jsonBox('dev-output','Error: '+e.message);}finally{setBusy(btn,false);}}
async function loadDevTasks(){const t=await API.get('/api/tasks?limit=30');const rows=(t||[]).filter(x=>String(x.request||'').toLowerCase().startsWith('development task:')).slice(0,12);byId('dev-history').innerHTML=rows.map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(short(x.request,130))}</div><div class="data-meta">${fmtDate(x.created_at||x.ts)}</div></div>${badge(x.status,statusTone(x.status))}</div>`).join('')||renderEmpty('No development tasks yet.');}

async function initKnowledge(){byId('memory-search-btn').onclick=searchMemory;byId('memory-save').onclick=saveMemory;await searchMemory();}
async function searchMemory(){const q=byId('memory-q').value,ns=byId('memory-filter-ns').value;const r=await API.get('/api/memory?q='+encodeURIComponent(q)+'&namespace='+encodeURIComponent(ns)+'&limit=100');byId('memory-results').innerHTML=(r||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.title||'Untitled memory')}</div><div class="data-meta">${esc(x.namespace||'general')} · ${fmtDate(x.created_at||x.ts)}<br>${esc(short(x.content,220))}</div></div></div>`).join('')||renderEmpty('No memories matched.');}
async function saveMemory(){const content=byId('memory-content').value.trim();if(!content)return toast('Memory content is required','bad');const btn=byId('memory-save');setBusy(btn,true,'Saving');try{const r=await API.post('/api/memory',{title:byId('memory-title').value,namespace:byId('memory-ns').value||'general',content});toast('Memory saved: '+(r.id||''),'ok');byId('memory-content').value='';await searchMemory();}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}

async function initAutomation(){byId('automation-create').onclick=createAutomation;byId('automation-refresh').onclick=loadAutomations;await loadAutomations();}
async function loadAutomations(){const a=await API.get('/api/automations');byId('automation-list').innerHTML=(a||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.name)}</div><div class="data-meta">Every ${esc(x.interval_seconds)} sec · last: ${fmtDate(x.last_run)}<br>${esc(short(x.prompt,160))}</div></div><div class="data-actions">${badge(x.enabled?'Enabled':'Paused',x.enabled?'ok':'warn')}<button class="btn small" onclick="toggleAutomation('${esc(x.id)}',${!x.enabled})">${x.enabled?'Pause':'Enable'}</button></div></div>`).join('')||renderEmpty('No automations configured.');}
async function createAutomation(){const name=byId('automation-name').value.trim(),prompt=byId('automation-prompt').value.trim();if(!name||!prompt)return toast('Name and task are required','bad');const btn=byId('automation-create');setBusy(btn,true,'Creating');try{await API.post('/api/automation',{name,prompt,interval_seconds:Number(byId('automation-interval').value||3600),enabled:true});toast('Automation created','ok');await loadAutomations();}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function toggleAutomation(id,enabled){try{await API.post('/api/automation/toggle',{id,enabled});await loadAutomations();}catch(e){toast(e.message,'bad');}}
window.toggleAutomation=toggleAutomation;

async function initComputer(){byId('proc-btn').onclick=()=>computerAction('system.processes.list',{},'computer-output');byId('svc-btn').onclick=()=>computerAction('system.services.list',{},'computer-output');byId('ring-ping').onclick=()=>computerAction('ring0.ping',{},'ring-output');byId('ring-status').onclick=()=>computerAction('ring0.status',{},'ring-output');byId('service-query').onclick=()=>computerAction('service.query',{resource_id:'ollama'},'service-output');byId('file-read').onclick=()=>computerAction('workspace.file.read',{path:byId('file-path').value},'file-output');byId('file-write').onclick=()=>computerAction('workspace.file.write',{path:byId('file-path').value,content:byId('file-content').value},'file-output');byId('url-open').onclick=()=>computerAction('url.open',{url:byId('url-value').value},'url-output');const b=await API.get('/api/broker');byId('broker-actions').innerHTML=(b.configured_actions||[]).map(x=>badge(x,'info')).join(' ');byId('broker-secret').textContent=b.approval_secret_configured?'Configured':'Not configured';}
async function computerAction(action_id,parameters,target){try{const r=await API.post('/api/system/action',{action_id,parameters});jsonBox(target,r);if(r.status==='approval_required')toast('This high-risk action requires the external trusted approval proof.','bad');}catch(e){jsonBox(target,'Error: '+e.message);}}

async function initSecurity(){byId('security-refresh').onclick=loadSecurity;await loadSecurity();}
async function loadSecurity(){const [a,r,b]=await Promise.all([API.get('/api/approvals?status=pending'),API.get('/api/receipts?limit=80'),API.get('/api/broker')]);byId('security-approvals-count').textContent=(a||[]).length;byId('security-chain').textContent=r.chain?.ok?'VERIFIED':'FAILED';byId('security-secret').textContent=b.approval_secret_configured?'READY':'MISSING';byId('security-approvals').innerHTML=(a||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.action||x.step_id||'Pipeline approval')}</div><div class="data-meta mono">${esc(x.id)}<br>${esc(short(JSON.stringify(x),180))}</div></div><div class="data-actions"><button class="btn small success" onclick="resolveApproval('${esc(x.id)}','approved')">Approve</button><button class="btn small danger" onclick="resolveApproval('${esc(x.id)}','rejected')">Reject</button></div></div>`).join('')||renderEmpty('No pending pipeline approvals.');byId('receipts-table').innerHTML=(r.items||[]).map(x=>`<tr><td>${fmtDate(x.created_at||x.ts)}</td><td>${esc(x.source||'')}</td><td>${badge(x.status,statusTone(x.status))}</td><td class="mono">${esc(short(x.id,24))}</td><td class="mono">${esc(short(x.hash,18))}</td></tr>`).join('')||'<tr><td colspan="5">No receipts.</td></tr>';byId('broker-posture').textContent=JSON.stringify(b,null,2);}
async function resolveApproval(id,status){try{const r=await API.post('/api/approval',{id,status});toast(`Approval ${status}`,'ok');jsonBox('approval-result',r);await loadSecurity();}catch(e){toast(e.message,'bad');}}
window.resolveApproval=resolveApproval;

async function initHealth(){byId('doctor-run').onclick=loadDoctor;await loadDoctor();}
async function loadDoctor(){const btn=byId('doctor-run');setBusy(btn,true,'Checking');try{const r=await API.get('/api/doctor');const entries=Object.entries(r||{});byId('doctor-grid').innerHTML=entries.map(([k,v])=>{const raw=typeof v==='object'?JSON.stringify(v):String(v);const tone=statusTone(raw);return `<div class="card metric-card span-3"><div class="metric-label">${esc(k.replaceAll('_',' '))}</div><div style="margin-top:9px">${badge(short(raw,34),tone)}</div><div class="metric-note">${esc(short(raw,120))}</div></div>`;}).join('');jsonBox('doctor-raw',r);}catch(e){jsonBox('doctor-raw','Error: '+e.message);}finally{setBusy(btn,false);}}

async function initActivity(){byId('activity-refresh').onclick=loadActivity;byId('activity-filter').addEventListener('input',loadActivity);await loadActivity();}
async function loadActivity(){const rows=await API.get('/api/events?limit=300'),q=byId('activity-filter').value.trim().toLowerCase();const filtered=(rows||[]).filter(x=>!q||String(x.kind).toLowerCase().includes(q)||JSON.stringify(x.payload||{}).toLowerCase().includes(q)).reverse();byId('activity-count').textContent=filtered.length;byId('activity-list').innerHTML=filtered.map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.kind)}</div><div class="data-meta mono">${esc(short(JSON.stringify(x.payload||{}),320))}</div></div><span class="small muted nowrap">${fmtDate(x.created_at||x.ts)}</span></div>`).join('')||renderEmpty('No matching events.');}

async function initFable(){byId('fable-refresh').onclick=loadFable;byId('fable-save-cap').onclick=saveFableCap;byId('fable-add-agenda').onclick=addFableAgenda;document.querySelectorAll('[data-fable-action]').forEach(b=>b.onclick=()=>fableLab(b.dataset.fableAction,b));await loadFable();}
async function loadFable(){const [s,c,a,t,tail]=await Promise.all([API.get('/api/fable'),API.get('/api/fable/capabilities?limit=100'),API.get('/api/fable/agenda'),API.get('/api/fable/traces?limit=80'),API.get('/api/fable/lab/tail?limit=160')]);const src=s.source||{};byId('fable-source').textContent=src.source_complete?'COMPLETE':'INCOMPLETE';byId('fable-runtime').textContent=src.running?'RUNNING':(src.runtime_ready?'READY':'SETUP');byId('fable-caps-count').textContent=s.learned_capabilities??c.length;byId('fable-agenda-count').textContent=a.length;byId('fable-cap-list').innerHTML=c.map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.name)} v${esc(x.version)}</div><div class="data-meta">${esc(x.description)} · success ${x.success_count} / fail ${x.failure_count}</div></div><div class="data-actions">${badge(x.enabled?'Enabled':'Disabled',x.enabled?'ok':'warn')}<button class="btn small" onclick="runFableCap('${esc(x.id)}')">Run</button><button class="btn small" onclick="toggleFableCap('${esc(x.id)}',${!x.enabled})">${x.enabled?'Disable':'Enable'}</button></div></div>`).join('')||renderEmpty('No learned capabilities.');byId('fable-cap-select').innerHTML='<option value="">Select capability</option>'+c.filter(x=>x.enabled).map(x=>`<option value="${esc(x.id)}">${esc(x.name)} v${x.version}</option>`).join('');byId('fable-agenda-list').innerHTML=a.map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.name)}</div><div class="data-meta">${esc(x.when)} · runs ${x.run_count}/${x.max_runs} · failures ${x.consecutive_failures}</div></div><div class="data-actions">${badge(x.enabled?'Enabled':'Paused',x.enabled?'ok':'warn')}<button class="btn small" onclick="toggleFableAgenda('${esc(x.id)}',${!x.enabled})">${x.enabled?'Pause':'Enable'}</button></div></div>`).join('')||renderEmpty('No agenda items.');byId('fable-traces').innerHTML=t.map(x=>`<div class="data-row"><div class="data-main"><div class="data-title">${esc(x.event)}</div><div class="data-meta">${badge(x.kind,statusTone(x.kind))} ${esc(short(JSON.stringify(x.payload||{}),220))}</div></div></div>`).join('')||renderEmpty('No traces.');jsonBox('fable-tail',(tail.classified||[]).map(x=>`[${x.kind}] ${x.line}`).join('\n')||'No lab output.');jsonBox('fable-status-raw',s);}
async function fableLab(action,btn){setBusy(btn,true,action);try{const r=await API.post('/api/fable/lab',{action,timeout:3600});jsonBox('fable-lab-output',r);await loadFable();}catch(e){jsonBox('fable-lab-output','Error: '+e.message);}finally{setBusy(btn,false);}}
async function saveFableCap(){const name=byId('fable-cap-name').value.trim(),prompt=byId('fable-cap-prompt').value.trim();if(!name||!prompt)return toast('Capability name and prompt are required','bad');try{await API.post('/api/fable/capability/save',{name,description:byId('fable-cap-desc').value,prompt,permissions:[]});toast('Capability saved','ok');await loadFable();}catch(e){toast(e.message,'bad');}}
async function runFableCap(id){try{const r=await API.post('/api/fable/capability/run',{id});jsonBox('fable-lab-output',r);await loadFable();}catch(e){toast(e.message,'bad');}}
async function toggleFableCap(id,enabled){try{await API.post('/api/fable/capability/toggle',{id,enabled});await loadFable();}catch(e){toast(e.message,'bad');}}
async function addFableAgenda(){const cid=byId('fable-cap-select').value;if(!cid)return toast('Select a capability','bad');try{await API.post('/api/fable/agenda/add',{name:byId('fable-agenda-name').value||'Jubi agenda item',capability_id:cid,when:byId('fable-when').value,period_seconds:Number(byId('fable-period').value||3600),max_runs:Number(byId('fable-max-runs').value||1)});toast('Agenda item added','ok');await loadFable();}catch(e){toast(e.message,'bad');}}
async function toggleFableAgenda(id,enabled){try{await API.post('/api/fable/agenda/toggle',{id,enabled});await loadFable();}catch(e){toast(e.message,'bad');}}
window.runFableCap=runFableCap;window.toggleFableCap=toggleFableCap;window.toggleFableAgenda=toggleFableAgenda;

const PAGE_INIT={overview:initOverview,chat:initChat,tasks:initTasks,brain:initBrain,providers:initProviders,models:initModels,agents:initAgents,development:initDevelopment,knowledge:initKnowledge,fable:initFable,automation:initAutomation,computer:initComputer,security:initSecurity,health:initHealth,activity:initActivity};
document.addEventListener('DOMContentLoaded',async()=>{mountShell();const page=document.body.dataset.page||'overview';try{if(PAGE_INIT[page])await PAGE_INIT[page]();}catch(e){console.error(e);toast('Page initialization failed: '+e.message,'bad');}});
