"""Read vanilla CELL editor IDs to select reproducible benchmark locations."""
import json
from pathlib import Path
import struct
import zlib

path=Path(r'C:\Games\Fallout.4 1.10.163\Data\Fallout4.esm')
matches=[]
with path.open('rb') as file:
    end=path.stat().st_size;offset=0
    while offset<end:
        file.seek(offset);header=file.read(24)
        tag,size=struct.unpack_from('<4sI',header)
        if tag==b'GRUP':offset+=24;continue
        if tag==b'CELL':
            flags,form=struct.unpack_from('<II',header,8)
            data=file.read(size)
            if flags&0x40000:data=zlib.decompress(data[4:])
            sub=0;editor=None
            while sub+6<=len(data):
                kind,length=struct.unpack_from('<4sH',data,sub);sub+=6
                if kind==b'EDID':editor=data[sub:sub+length].rstrip(b'\0').decode('utf-8');break
                sub+=length
            if editor and any(s.lower() in editor.lower() for s in ['DiamondCity','Goodneighbor','FinancialDistrict','Fens']):
                matches.append({'editor_id':editor,'form_id':form,'hex':hex(form)})
        offset+=24+size
print(json.dumps(matches,indent=2))
