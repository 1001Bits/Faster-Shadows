"""Extend only owned, idle game labs with previously omitted vanilla videos."""
import json
import os
from pathlib import Path
import psutil

ROOT=Path(__file__).resolve().parents[2]
for runtime in ['og','ae']:
    setup_path=ROOT/'.runtime-tests'/runtime/'setup.json'
    setup=json.loads(setup_path.read_text());game=Path(setup['game'])
    if not game.resolve().is_relative_to(ROOT):raise RuntimeError('Lab outside workspace')
    for process in psutil.process_iter(['exe']):
        if process.info['exe'] and Path(process.info['exe']).parent==game:
            raise RuntimeError('Owned lab is running; assets must remain fixed')
    source=Path(setup['source']);added=[]
    for file in sorted((source/'Data/Video').glob('*.bk2')):
        target=game/'Data/Video'/file.name;target.parent.mkdir(parents=True,exist_ok=True)
        if target.exists():
            if not target.is_symlink() or target.resolve()!=file.resolve():raise RuntimeError('Unexpected existing video')
            continue
        os.symlink(file,target);stat=file.stat()
        setup['immutable_archive_inputs'].append({'source':str(file),'target':str(target),
            'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns})
        added.append(file.name)
    setup_path.write_text(json.dumps(setup,indent=2)+'\n')
    print(runtime,'added immutable video inputs:',added)
