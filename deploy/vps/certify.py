#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, os, sys, tempfile, time
from pathlib import Path
from urllib.request import Request, urlopen

def get(url):
    with urlopen(url, timeout=10) as r: return json.load(r)

def post(url, token, payload, timeout=360):
    data=json.dumps(payload).encode()
    req=Request(url,data=data,headers={'Content-Type':'application/json','X-JUBI-Token':token},method='POST')
    with urlopen(req,timeout=timeout) as r: return json.load(r)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--base',default='http://127.0.0.1:8877'); ap.add_argument('--live-model',action='store_true'); ap.add_argument('--browser-url',default='https://example.com/')
    args=ap.parse_args(); base=args.base.rstrip('/'); results=[]
    def check(name,fn,required=True):
        try:
            value=fn(); ok=True if not isinstance(value,dict) else value.get('ok',True) is not False
            results.append({'name':name,'ok':bool(ok),'required':required,'detail':value})
        except Exception as exc: results.append({'name':name,'ok':False,'required':required,'error':str(exc)})
    check('health',lambda:get(base+'/api/health'))
    session=get(base+'/api/session'); token=session['token']
    check('status',lambda:get(base+'/api/status'))
    check('doctor',lambda:get(base+'/api/doctor'))
    check('providers',lambda:get(base+'/api/providers'))
    check('memory-list',lambda:get(base+'/api/memory'))
    check('automations-list',lambda:get(base+'/api/automations'))
    check('browser-status',lambda:get(base+'/api/browser'))
    if args.live_model:
        check('live-chat',lambda:post(base+'/api/chat',token,{'text':'Reply with exactly JUBI-LIVE-OK','provider':'ollama'}))
        check('brain-route',lambda:post(base+'/api/brain/route',token,{'text':'Write a Python function that adds two integers','task_type':'coding'}))
    else:
        results.append({'name':'live-chat','ok':None,'required':False,'detail':'skipped; pass --live-model after installing Ollama model'})
    browser=get(base+'/api/browser')
    if browser.get('ready'): check('browser-live',lambda:post(base+'/api/browser/browse',token,{'url':args.browser_url,'screenshot':False,'timeout':30}),required=False)
    else: results.append({'name':'browser-live','ok':None,'required':False,'detail':'Playwright not installed'})
    failed=[x for x in results if x.get('required') and x.get('ok') is not True]
    print(json.dumps({'status':'PASS' if not failed else 'FAIL','results':results},indent=2,default=str))
    return 1 if failed else 0

if __name__=='__main__': raise SystemExit(main())
