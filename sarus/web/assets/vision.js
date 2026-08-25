'use strict';

let visionDataUri='';
let visionLastText='';
function vEsc(v){return String(v??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));}
async function loadVisionStatus(){
  const s=await API.get('/api/vision');
  document.getElementById('vision-online').textContent=s.ollama_online?'ONLINE':'OFFLINE';
  document.getElementById('vision-online').className=s.ollama_online?'metric-value good':'metric-value danger-text';
  document.getElementById('vision-model-name').textContent=s.selected_model||'No local vision model';
  const sel=document.getElementById('vision-model');
  const rows=s.vision_models||[];
  sel.innerHTML=rows.length?rows.map(x=>`<option value="${vEsc(x.name)}" ${x.name===s.selected_model?'selected':''}>${vEsc(x.name)}</option>`).join(''):'<option value="">No vision model installed</option>';
  const Recognition=window.SpeechRecognition||window.webkitSpeechRecognition;
  const speech=!!window.speechSynthesis;
  document.getElementById('voice-status').textContent=(Recognition?'STT ':'')+(speech?'TTS':'')||'UNAVAILABLE';
}
function chooseVisionFile(){
  const file=document.getElementById('vision-file').files?.[0];
  if(!file){visionDataUri='';return;}
  if(file.size>8*1024*1024){toast('Image must be 8 MiB or smaller','bad');document.getElementById('vision-file').value='';return;}
  const allowed=['image/png','image/jpeg','image/webp'];if(!allowed.includes(file.type)){toast('Use PNG, JPEG or WebP','bad');return;}
  const r=new FileReader();r.onload=()=>{visionDataUri=String(r.result||'');const img=document.getElementById('vision-preview');img.src=visionDataUri;img.style.display='block';document.getElementById('vision-no-preview').style.display='none';};r.readAsDataURL(file);
}
async function analyzeVision(){
  if(!visionDataUri)return toast('Choose an image first','bad');
  const btn=document.getElementById('vision-analyze');setBusy(btn,true,'Analyzing');
  try{
    const r=await API.post('/api/vision/analyze',{image:visionDataUri,prompt:document.getElementById('vision-prompt').value.trim(),model:document.getElementById('vision-model').value||null});
    visionLastText=String(r.response||r.output||'');document.getElementById('vision-answer').textContent=visionLastText||'No text returned.';
    jsonBox('vision-details',{provider:r.provider,model:r.model,mime:r.mime,image_bytes:r.image_bytes,latency_ms:r.latency_ms});
  }catch(e){document.getElementById('vision-answer').textContent='Error: '+e.message;}finally{setBusy(btn,false);}
}
function dictateVision(){
  const Recognition=window.SpeechRecognition||window.webkitSpeechRecognition;if(!Recognition)return toast('Speech recognition is not available in this browser','bad');
  const btn=document.getElementById('voice-dictate');const rec=new Recognition();rec.interimResults=false;rec.maxAlternatives=1;
  rec.onstart=()=>setBusy(btn,true,'Listening');rec.onerror=e=>{setBusy(btn,false);toast('Speech recognition: '+e.error,'bad');};rec.onend=()=>setBusy(btn,false);
  rec.onresult=e=>{const text=e.results?.[0]?.[0]?.transcript||'';if(text)document.getElementById('vision-prompt').value=text;};rec.start();
}
function readVision(){if(!window.speechSynthesis)return toast('Text-to-speech is not available','bad');const text=visionLastText||document.getElementById('vision-answer').textContent;if(!text)return;window.speechSynthesis.cancel();const u=new SpeechSynthesisUtterance(text.slice(0,12000));window.speechSynthesis.speak(u);}
document.addEventListener('DOMContentLoaded',async()=>{document.getElementById('vision-file').onchange=chooseVisionFile;document.getElementById('vision-analyze').onclick=analyzeVision;document.getElementById('voice-dictate').onclick=dictateVision;document.getElementById('voice-read').onclick=readVision;document.getElementById('vision-refresh').onclick=loadVisionStatus;try{await loadVisionStatus();}catch(e){toast('Vision page: '+e.message,'bad');}});
