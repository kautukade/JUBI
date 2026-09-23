'use strict';

let visionDataUri='';
let visionLastText='';
let voiceRuntime=null;
let voiceRecorder=null;
let voiceStream=null;
let voiceChunks=[];
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
  try{voiceRuntime=await API.get('/api/voice');}catch{voiceRuntime=null;}
  const local=!!voiceRuntime?.ready;
  const mic=!!(navigator.mediaDevices?.getUserMedia && window.MediaRecorder);
  document.getElementById('voice-status').textContent=local
    ? ('LOCAL STT/TTS'+(mic?' + MIC':''))
    : ((Recognition?'BROWSER STT ':'')+(speech?'BROWSER TTS':'')||'UNAVAILABLE');
}
let visionFileVersion=0;
function chooseVisionFile(){
 const version=++visionFileVersion;visionDataUri='';visionLastText='';
 const input=document.getElementById('vision-file'),img=document.getElementById('vision-preview');
 img.removeAttribute('src');img.style.display='none';document.getElementById('vision-no-preview').style.display='block';
 const file=input.files?.[0];if(!file)return;
 if(file.size>8*1024*1024||!['image/png','image/jpeg','image/webp'].includes(file.type)){input.value='';toast('Use a PNG, JPEG or WebP image of 8 MiB or smaller','bad');return;}
 const reader=new FileReader();reader.onload=()=>{if(version!==visionFileVersion)return;visionDataUri=String(reader.result||'');img.src=visionDataUri;img.style.display='block';document.getElementById('vision-no-preview').style.display='none';};
 reader.onerror=()=>{if(version===visionFileVersion)toast('Image could not be read','bad');};reader.readAsDataURL(file);
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
function blobDataUrl(blob){return new Promise((resolve,reject)=>{const r=new FileReader();r.onload=()=>resolve(String(r.result||''));r.onerror=reject;r.readAsDataURL(blob);});}
async function dictateVision(){
  const btn=document.getElementById('voice-dictate');
  if(voiceRecorder&&voiceRecorder.state==='recording'){voiceRecorder.stop();return;}
  if(voiceRuntime?.stt_ready && navigator.mediaDevices?.getUserMedia && window.MediaRecorder){
    try{
      voiceStream=await navigator.mediaDevices.getUserMedia({audio:true});
      const preferred=['audio/webm;codecs=opus','audio/webm','audio/ogg;codecs=opus'];
      const mime=preferred.find(x=>MediaRecorder.isTypeSupported?.(x))||'';
      voiceChunks=[];voiceRecorder=new MediaRecorder(voiceStream,mime?{mimeType:mime}:undefined);
      voiceRecorder.ondataavailable=e=>{if(e.data?.size)voiceChunks.push(e.data);};
      voiceRecorder.onerror=e=>{toast('Microphone recording failed: '+(e.error?.message||e.name),'bad');};
      voiceRecorder.onstop=async()=>{
        const stream=voiceStream;voiceStream=null;stream?.getTracks().forEach(t=>t.stop());
        const rec=voiceRecorder;voiceRecorder=null;btn.disabled=true;btn.innerHTML='<span class="loader"></span>Transcribing';
        try{
          const type=(rec?.mimeType||voiceChunks[0]?.type||'audio/webm').split(';')[0];
          const blob=new Blob(voiceChunks,{type});voiceChunks=[];
          if(blob.size>20*1024*1024)throw new Error('Audio clip is larger than 20 MiB');
          const audio=await blobDataUrl(blob);
          const r=await API.post('/api/voice/transcribe',{audio,mime:type});
          document.getElementById('vision-prompt').value=r.text||'';
          toast(r.wake_detected?'Hey Jubi detected · local transcription complete':'Local transcription complete','ok');
        }catch(e){toast('Local transcription: '+e.message,'bad');}
        finally{btn.disabled=false;btn.classList.remove('primary');btn.innerHTML='🎙 Dictate';}
      };
      voiceRecorder.start();btn.dataset.old=btn.innerHTML;btn.innerHTML='■ Stop & transcribe';btn.classList.add('primary');
      return;
    }catch(e){voiceStream?.getTracks().forEach(t=>t.stop());voiceStream=null;voiceRecorder=null;toast('Microphone: '+e.message,'bad');}
  }
  const Recognition=window.SpeechRecognition||window.webkitSpeechRecognition;if(!Recognition)return toast('Neither local VPS STT nor browser speech recognition is available','bad');
  const rec=new Recognition();rec.interimResults=false;rec.maxAlternatives=1;
  rec.onstart=()=>setBusy(btn,true,'Listening');rec.onerror=e=>{setBusy(btn,false);toast('Browser speech recognition: '+e.error,'bad');};rec.onend=()=>setBusy(btn,false);
  rec.onresult=e=>{const text=e.results?.[0]?.[0]?.transcript||'';if(text)document.getElementById('vision-prompt').value=text;};rec.start();
}
async function readVision(){
  const text=visionLastText||document.getElementById('vision-answer').textContent;if(!text)return;
  if(voiceRuntime?.tts_ready){
    const btn=document.getElementById('voice-read');setBusy(btn,true,'Speaking');
    try{
      const r=await API.post('/api/voice/synthesize',{text:text.slice(0,4000),language:'en'});
      const audio=new Audio(r.audio);audio.onended=()=>setBusy(btn,false);audio.onerror=()=>{setBusy(btn,false);toast('Local audio playback failed','bad');};await audio.play();return;
    }catch(e){setBusy(btn,false);toast('Local TTS: '+e.message,'bad');}
  }
  if(!window.speechSynthesis)return toast('Text-to-speech is not available','bad');
  window.speechSynthesis.cancel();const u=new SpeechSynthesisUtterance(text.slice(0,12000));window.speechSynthesis.speak(u);
}
document.addEventListener('DOMContentLoaded',async()=>{document.getElementById('vision-file').onchange=chooseVisionFile;document.getElementById('vision-analyze').onclick=analyzeVision;document.getElementById('voice-dictate').onclick=dictateVision;document.getElementById('voice-read').onclick=readVision;document.getElementById('vision-refresh').onclick=loadVisionStatus;try{await loadVisionStatus();}catch(e){toast('Vision page: '+e.message,'bad');}});
