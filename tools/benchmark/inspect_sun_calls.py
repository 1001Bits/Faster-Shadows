"""Read-only census of direct native calls to the verified sun updater."""
import json
from pathlib import Path
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from inspect_engine import Engine,GAMES

for profile in json.loads((ROOT/'evidence/engine-profiles.json').read_text()):
    engine=Engine(GAMES[profile['runtime']]);code=engine.code;start=engine.text.VirtualAddress
    for hook in ['sun_update','sun_append','list_append']:
        target=profile['hooks'][hook]['target'];calls=[];offset=0
        while True:
            offset=code.find(b'\xe8',offset)
            if offset<0 or offset+5>len(code):break
            rva=start+offset
            if rva+5+struct.unpack_from('<i',code,offset+1)[0]==target:calls.append(hex(rva))
            offset+=1
        print(profile['runtime'],hook,'target',hex(target),'direct calls',calls,flush=True)
