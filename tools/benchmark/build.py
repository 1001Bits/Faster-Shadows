"""Build matching private candidate/probe artifacts and record their inputs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from provenance import source_inputs

ROOT=Path(__file__).resolve().parents[2]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

env={key.upper():value for key,value in os.environ.items()}
subprocess.run([sys.executable,'-B',str(ROOT/'tools/benchmark/prepare_support.py')],cwd=ROOT,env=env,check=True)
support=ROOT/'.runtime-tests/support'
def support_inputs():
    return {path.relative_to(support).as_posix():sha(path) for path in sorted(support.rglob('*'))
        if path.is_file() and not any(part in {'.git','__pycache__'} for part in path.parts)}
inputs=source_inputs();prepared=support_inputs()
subprocess.run(['cmake','-S','tools/benchmark','-B','build-benchmark','-G','Visual Studio 17 2022','-A','x64'],
    cwd=ROOT,env=env,check=True)
subprocess.run(['cmake','--build','build-benchmark','--config','Release','--target',
    'FPSBenchmarkCandidate','FPSBenchProbe','FPSFolderGuard','DTLBenchLoader','--parallel','1','--','/nr:false'],cwd=ROOT,env=env,check=True)
binaries=['FasterShadows.dll','DTLBenchFO4.dll','FPSFolderGuard.dll','DTLBenchLoader.exe']
if inputs!=source_inputs() or prepared!=support_inputs():
    raise RuntimeError('Private compilation inputs changed during the build')
manifest={'protocol':4,'sources':inputs,'support':prepared,
    'binaries':{name:sha(ROOT/'build-benchmark/Release'/name) for name in binaries}}
(ROOT/'build-benchmark/manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Matched private diagnostics built; protocol',manifest['protocol'])
