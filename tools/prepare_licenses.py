"""Collect notices and corresponding dependency sources for this local build."""
import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--installed',type=Path,required=True)
    p.add_argument('--buildtrees',type=Path,required=True)
    a=p.parse_args()
    notices=ROOT/'licenses'
    notices.mkdir(exist_ok=True)
    shutil.copyfile(ROOT/'upstream/skyrimPerformanceSolution/LICENSE',ROOT/'LICENSE')
    shutil.copyfile(ROOT/'external/CommonLibF4/LICENSE',notices/'CommonLibF4.txt')
    records=[]
    for port in ['fmt','spdlog','rsm-mmio','rapidcsv']:
        installed=a.installed/'share'/port
        source_candidates=list((a.buildtrees/port/'src').glob('*.clean'))
        if len(source_candidates)!=1:
            raise RuntimeError(f'Expected one corresponding source for {port}: {source_candidates}')
        source=source_candidates[0]
        destination=ROOT/'external/dependencies'/port
        if not destination.exists():
            shutil.copytree(source,destination,ignore=shutil.ignore_patterns('.git','__pycache__'))
        shutil.copyfile(installed/'copyright',notices/(port+'.txt'))
        if (installed/'vcpkg.spdx.json').exists():
            shutil.copyfile(installed/'vcpkg.spdx.json',ROOT/'evidence'/('dependency-'+port+'.json'))
        records.append({'port':port,'source_directory':source.name,
                        'corresponding_source':'external/dependencies/'+port})
    (ROOT/'evidence/dependency-sources.json').write_text(json.dumps(records,indent=2)+'\n')
    print(json.dumps(records,indent=2))
