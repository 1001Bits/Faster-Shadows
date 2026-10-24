"""One explicitly handed-off CPU build/test attempt. No game starts or deployment."""
import argparse,json,os,sys,shutil
from pathlib import Path
from datetime import datetime,timezone
from slot import Slot,ROOT,atomic
sys.path.insert(0,str(ROOT/'tools'))
from provenance import source_inputs,sha

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--handoff',type=Path,required=True)
p.add_argument('--request',type=Path,required=True)
p.add_argument('--coexist',action='store_true',
    help='Untimed build: no shared lease or lock; other projects may build or play meanwhile (the CPU micro-benchmark is then noisy)')
a=p.parse_args()
label='faster-shadows-cpu-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
out=ROOT/'evidence'/label;out.mkdir(exist_ok=False)
env={key.upper():value for key,value in os.environ.items()}
request=a.request.resolve()
targets=['FasterShadows','ShadowPolicyTests','ShadowOptimizationTests','ShadowCpuBenchmark',
         'FPSBenchmarkCandidate','FPSBenchProbe','FPSFolderGuard','DTLBenchLoader']
slot=Slot(a.handoff,label,request_path=request,minutes=10,minimum_ram=2,targets=targets,exclusive=not a.coexist)
inputs=source_inputs()
try:
    with slot:
        for name in ('verification.json','build-validation.json'):
            old=ROOT/'evidence'/name;archive=ROOT/'evidence/historical-pre-020'/name
            if old.exists() and not archive.exists():
                archive.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(old,archive)
        slot.command([sys.executable,'-B','-m','unittest','discover','-s','tests','-p','test_*.py','-v'],out/'runner-tests.log',env)
        slot.command([sys.executable,'-B','tools/build.py','--dependencies',r'C:\Development\Texture Downscaler FO4\build\vcpkg_installed\x64-windows-static-md','--workers','1'],out/'build.log',env)
        slot.command([sys.executable,'-B','tools/verify_port.py'],out/'verify.log',env)
        slot.command([sys.executable,'-B','tools/benchmark/build.py'],out/'private-build.log',env)
        import psutil
        available=psutil.Process().cpu_affinity();core=2 if 2 in available else available[0]
        slot.command([str(ROOT/'build-release/Release/ShadowCpuBenchmark.exe')],out/'cpu-raw.json',env,affinity=core)
        if inputs!=source_inputs():raise RuntimeError('Build/test source inputs changed during the attempt')
        validation={'schema':2,'project':'Faster Shadows','version':'1.0','passed':True,
            'game_cpu_frametime_win_proven':False,'shared_machine':a.coexist,'source_sha256':inputs,'evidence':str(out),
            'dll_sha256':sha(ROOT/'build-release/Release/FasterShadows.dll'),'commands':slot.receipt['commands']}
        atomic(out/'build-validation.json',validation);atomic(ROOT/'evidence/build-validation.json',validation)
    print(json.dumps({'passed':True,'evidence':str(out),'terminal':slot.receipt}))
except BaseException as error:
    # Admission refusal never overwrites a paused request or schedules a retry.
    slot.receipt['error']=repr(error)
    # Slot.__exit__ already released after a failure inside the with block; release only an
    # admission failure that never reached it, so a retained claim is never released twice.
    if slot.record is not None and not slot.receipt.get('terminal') and 'finished' not in slot.receipt:slot.release()
    atomic(out/'terminal.json',slot.receipt)
    print(json.dumps({'passed':False,'evidence':str(out),'terminal':slot.receipt}));raise
finally:
    atomic(out/'terminal.json',slot.receipt)
