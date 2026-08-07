from __future__ import annotations
import os,shutil
class NativeRuntimeManager:
    def __init__(self,app): self.app=app; self.root=app.root; self.sources=app.adapters.cfg
    def _source(self,name): return self.root/'sources'/self.sources[name]
    def _exe(self,venv,name): return venv/('Scripts' if os.name=='nt' else 'bin')/(name+'.exe' if os.name=='nt' else name)
    def status(self):
        native=self.root/'native'; sara=self.app.adapters.get('sara').probe(); hv=native/'hermes'; cv=native/'cai'; hermes=self._exe(hv,'hermes'); cai=self._exe(cv,'cai'); ecc_script=self._source('ecc')/'scripts/ecc.js'
        return {
            'sara':{'ready':bool(sara.details.get('native')),'mode':'SARA v7 local API','detail':sara.details},
            'hermes':{'ready':hermes.exists(),'mode':'optional native CLI + SARUS Ollama adapter','path':str(hermes)},
            'ecc':{'ready':bool(shutil.which('node')) and ecc_script.exists() and (self._source('ecc')/'node_modules').exists(),'mode':'native Node CLI + SARUS skill adapter','path':str(ecc_script)},
            'agency_agents':{'ready':True,'mode':'prompt/persona runtime through local Ollama'},
            'awesome_llm_apps':{'ready':True,'mode':'local workflow-pattern runtime; individual upstream apps may require external credentials'},
            'second_brain':{'ready':True,'mode':'local skill runtime + SARUS memory'},
            'superpowers':{'ready':True,'mode':'local coding-process skill runtime'},
            'fable_os':{'ready':True,'mode':'SARUS trusted receipt layer; bare-metal QEMU is optional lab-only'},
            'cai':{'ready':cai.exists(),'mode':'defensive analysis enabled; native active tooling isolated/disabled by default','path':str(cai)},
            'autoresearch':{'ready':True,'mode':'bounded experiment-design runtime; GPU training requires target GPU environment'},
        }
