'use strict';

(function(){
  const advanced=[
    ['research','/research.html','Web Research','⌕'],
    ['network','/network.html','Authorized LAN','⌁'],
    ['vision','/vision.html','Vision & Voice','◐']
  ];
  const meta={
    research:['Web Research','Public-web evidence with SSRF and prompt-injection boundaries'],
    network:['Authorized LAN Manager','Passive discovery, explicit device registry and registered-service health'],
    vision:['Vision & Voice','Local Ollama image analysis with optional browser speech controls']
  };
  function extend(){
    const nav=document.querySelector('.nav-scroll');
    if(nav&&!document.querySelector('[data-jubi-advanced-nav]')){
      const wrap=document.createElement('div');wrap.setAttribute('data-jubi-advanced-nav','1');
      wrap.innerHTML=`<div class="nav-section">Advanced</div>${advanced.map(i=>`<a class="nav-link ${document.body.dataset.page===i[0]?'active':''}" href="${i[1]}"><span class="nav-icon">${i[3]}</span><span>${i[2]}</span></a>`).join('')}`;
      nav.appendChild(wrap);
    }
    const page=document.body.dataset.page||'';
    if(meta[page]){
      const title=document.querySelector('.page-title'),sub=document.querySelector('.page-subtitle');
      if(title)title.textContent=meta[page][0];if(sub)sub.textContent=meta[page][1];
    }
    const foot=document.querySelector('.sidebar-foot');
    if(foot)foot.innerHTML='<div>Jubi Local AI Platform</div><div style="margin-top:3px">Localhost UI • Permission-bounded tools</div>';
  }
  document.addEventListener('DOMContentLoaded',extend);
})();
