'use strict';

async function typedOperator(action_id,parameters,target){
  try{
    const r=await API.post('/api/system/action',{action_id,parameters});
    jsonBox(target,r);
    if(r.status==='approval_required')toast('This action requires the broker approval proof.','bad');
    else if(r.status==='denied'||r.status==='invalid')toast(r.error||r.status,'bad');
  }catch(e){jsonBox(target,'Error: '+e.message);toast(e.message,'bad');}
}
function opPath(id){return String(document.getElementById(id)?.value||'').trim();}
document.addEventListener('DOMContentLoaded',()=>{
  const bind=(id,fn)=>{const el=document.getElementById(id);if(el)el.onclick=fn;};
  bind('path-stat',()=>typedOperator('workspace.path.stat',{path:opPath('file-path')},'file-output'));
  bind('dir-list',()=>typedOperator('workspace.directory.list',{path:opPath('file-path')},'file-output'));
  bind('dir-create',()=>typedOperator('workspace.directory.create',{path:opPath('file-path'),parents:true},'file-output'));
  bind('operator-copy',()=>typedOperator('workspace.file.copy',{source_path:opPath('operator-source'),destination_path:opPath('operator-destination'),overwrite:false},'operator-file-output'));
  bind('operator-move',()=>typedOperator('workspace.file.move',{source_path:opPath('operator-source'),destination_path:opPath('operator-destination'),overwrite:false},'operator-file-output'));
  bind('operator-delete',()=>typedOperator('workspace.file.delete',{path:opPath('operator-source')},'operator-file-output'));
  bind('git-status',()=>typedOperator('development.git.status',{path:opPath('operator-project')},'operator-dev-output'));
  bind('git-log',()=>typedOperator('development.git.log',{path:opPath('operator-project'),limit:20},'operator-dev-output'));
  bind('app-launch',()=>typedOperator('app.launch',{resource_id:document.getElementById('operator-app').value,workspace_path:opPath('operator-project')},'operator-dev-output'));
});
