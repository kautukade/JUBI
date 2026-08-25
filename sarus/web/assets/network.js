'use strict';

function networkEsc(v){return String(v??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
function networkDate(v){if(!v)return '—';try{return new Date(Number(v)*1000).toLocaleString();}catch{return String(v);}}
function networkServices(raw){
  const text=String(raw||'').trim();if(!text)return [];
  return text.split(',').map(x=>x.trim()).filter(Boolean).map(item=>{
    const parts=item.split(':');if(parts.length<2)throw new Error('Use service:port format, for example ssh:22,http:80');
    const port=Number(parts.pop()),name=parts.join(':').trim();
    if(!Number.isInteger(port)||port<1||port>65535)throw new Error('Every service port must be 1-65535');
    return {name:name||`tcp/${port}`,port};
  });
}
async function loadNetwork(){
  const [status,devices,obs]=await Promise.all([API.get('/api/network'),API.get('/api/network/devices'),API.get('/api/network/observations?limit=80')]);
  document.getElementById('network-device-count').textContent=status.registered_devices??devices.length;
  document.getElementById('network-discovery-mode').textContent=status.active_scan?'ACTIVE':'PASSIVE';
  const body=document.getElementById('network-devices');
  body.innerHTML=(devices||[]).map(d=>`<tr><td><b>${networkEsc(d.label||d.host)}</b><div class="data-meta">${networkEsc(d.notes||'')}</div></td><td class="mono">${networkEsc(d.host)}</td><td>${(d.services||[]).map(s=>`<span class="badge info">${networkEsc(s.name)}:${s.port}</span>`).join(' ')||'—'}</td><td>${networkDate(d.updated)}</td><td><div class="actions"><button class="btn small" data-net-check="${networkEsc(d.id)}">Check</button><button class="btn small danger" data-net-delete="${networkEsc(d.id)}">Remove</button></div></td></tr>`).join('')||'<tr><td colspan="5">No authorized devices registered.</td></tr>';
  body.querySelectorAll('[data-net-check]').forEach(b=>b.onclick=()=>checkNetworkDevice(b.dataset.netCheck,b));
  body.querySelectorAll('[data-net-delete]').forEach(b=>b.onclick=()=>deleteNetworkDevice(b.dataset.netDelete,b));
  document.getElementById('network-observations').innerHTML=(obs||[]).map(o=>`<div class="data-row"><div class="data-main"><div class="data-title">${networkEsc(o.kind)} · ${networkEsc(o.host||o.ip||'')}</div><div class="data-meta">${networkDate(o.ts)} · ${networkEsc(o.status)}${o.mac?` · ${networkEsc(o.mac)}`:''}</div></div></div>`).join('')||'<div class="empty">No observations yet.</div>';
}
async function discoverNetwork(){
  const btn=document.getElementById('network-discover');setBusy(btn,true,'Reading');
  try{
    const r=await API.post('/api/network/discover',{});
    const el=document.getElementById('network-neighbors');
    el.innerHTML=(r.devices||[]).map(x=>`<div class="data-row"><div class="data-main"><div class="data-title mono">${networkEsc(x.ip)}</div><div class="data-meta">${networkEsc(x.mac||'no MAC')} · untrusted passive metadata</div></div><span class="badge warn">UNREGISTERED</span></div>`).join('')||'<div class="empty">No neighbors were present in the host cache.</div>';
    await loadNetwork();
  }catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}
}
async function registerNetwork(){
  const btn=document.getElementById('network-register');setBusy(btn,true,'Registering');
  try{
    const body={host:document.getElementById('network-host').value.trim(),label:document.getElementById('network-label').value.trim(),services:networkServices(document.getElementById('network-services').value),notes:document.getElementById('network-notes').value.trim()};
    const r=await API.post('/api/network/device',body);jsonBox('network-register-output',r);toast('Authorized device registered','ok');await loadNetwork();
  }catch(e){jsonBox('network-register-output','Error: '+e.message);toast(e.message,'bad');}finally{setBusy(btn,false);}
}
async function checkNetworkDevice(id,btn){setBusy(btn,true,'Checking');try{const r=await API.post('/api/network/check',{id});toast(`Checked ${r.host}`,'ok');jsonBox('network-register-output',r);await loadNetwork();}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
async function deleteNetworkDevice(id,btn){setBusy(btn,true,'Removing');try{await API.post('/api/network/delete',{id});toast('Device removed','ok');await loadNetwork();}catch(e){toast(e.message,'bad');}finally{setBusy(btn,false);}}
document.addEventListener('DOMContentLoaded',async()=>{document.getElementById('network-refresh').onclick=loadNetwork;document.getElementById('network-discover').onclick=discoverNetwork;document.getElementById('network-register').onclick=registerNetwork;try{await loadNetwork();}catch(e){toast('Network page: '+e.message,'bad');}});
