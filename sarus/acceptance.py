from __future__ import annotations
import argparse,json,os,time
from pathlib import Path
from .core.app import Sarus
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--full',action='store_true'); ap.add_argument('--json',action='store_true'); args=ap.parse_args(); root=Path(__file__).resolve().parents[1]; app=Sarus(root); checks=[]
    def check(name,fn,required=True):
        try: detail=fn(); ok=detail is not False; checks.append({'name':name,'ok':ok,'detail':detail,'required':required})
        except Exception as e: checks.append({'name':name,'ok':False,'detail':str(e),'required':required})
    check('10 source adapters',lambda: len(app.adapters.connect())==10 and all(x.connected for x in app.adapters.connect())); check('capability registry exact file count',lambda: sum(x['files'] for x in app.registry.summary().values())==17356); check('receipt chain',lambda: app.receipts.verify_chain()['ok'])
    def mem(): token='accept-'+str(time.time()); app.memory.add(token,'acceptance','test'); return bool(app.memory.search(token,'test',5))
    check('memory write/search',mem); check('policy approval gate',lambda: app.policy.evaluate('privileged_system_action',5,'core')['decision']=='approval'); check('CAI isolation',lambda: app.policy.evaluate('active_test',2,'cai')['decision']=='isolated'); doctor=app.doctor.run(); check('Ollama online',lambda: doctor['models'].get('online',False),required=True); installed=set(doctor['models'].get('models',[]))
    for m in ['qwen2.5:7b','qwen2.5-coder:7b','qwen2.5vl:3b','nomic-embed-text-v2-moe:latest']: check('model '+m,lambda m=m: m in installed,required=True)
    if args.full and doctor['models'].get('online'): check('Ollama generation',lambda: bool(app.models.generate_text('Reply exactly SARUS_OK','fast')[:100]))
    if os.name=='nt':
        check('Windows process broker',lambda: app.windows.action('list_processes').get('ok'),required=True); check('SARA v7 native API bridge',lambda: app.native.status()['sara']['ready'],required=True); check('ECC native runtime',lambda: app.native.status()['ecc']['ready'],required=False); check('Hermes native CLI',lambda: app.native.status()['hermes']['ready'],required=False)
    else: check('Windows target acceptance',lambda: 'run this acceptance suite on the target Windows laptop',required=False)
    ok=all(c['ok'] for c in checks if c['required']); out={'name':'SARUS v1 Acceptance','ok':ok,'checks':checks,'doctor':doctor}; print(json.dumps(out,ensure_ascii=False,indent=2)); raise SystemExit(0 if ok else 2)
if __name__=='__main__':main()
