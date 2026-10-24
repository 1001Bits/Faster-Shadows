"""Replace owned ESM file links with verified copies for native file discovery."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import psutil

ROOT=Path(__file__).resolve().parents[2]
def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda:source.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()

for runtime in ['og','ae']:
    setup_path=ROOT/'.runtime-tests'/runtime/'setup.json'
    setup=json.loads(setup_path.read_text());game=Path(setup['game'])
    if not game.resolve().is_relative_to(ROOT):raise RuntimeError('Lab outside workspace')
    for process in psutil.process_iter(['exe']):
        if process.info['exe'] and Path(process.info['exe']).parent==game:raise RuntimeError('Owned lab is running')
    copies=[]
    for entry in setup['immutable_archive_inputs']:
        source=Path(entry['source']);target=Path(entry['target'])
        if source.suffix.lower()!='.esm':continue
        if not target.absolute().is_relative_to(game) or target.parent.is_symlink():raise RuntimeError('Unexpected master target')
        digest=sha(source)
        if target.is_symlink():
            if target.resolve()!=source.resolve():raise RuntimeError('Unexpected master link')
            if shutil.disk_usage(ROOT).free<source.stat().st_size+2*1024**3:raise RuntimeError('Insufficient copy headroom')
            temporary=target.with_suffix('.esm.copy-tmp')
            if temporary.exists():raise RuntimeError('Unexpected temporary master')
            shutil.copy2(source,temporary)
            if sha(temporary)!=digest:raise RuntimeError('Master copy verification failed')
            os.replace(temporary,target)
        elif sha(target)!=digest:raise RuntimeError('Existing master differs from source')
        copies.append({'source':str(source),'target':str(target),'sha256':digest})
    setup['master_file_copies']=copies
    setup_path.write_text(json.dumps(setup,indent=2)+'\n')
    print(runtime,'verified regular master files:',len(copies),flush=True)
