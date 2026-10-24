"""Validate complete, consecutive world-frame captures; never certify visual quality."""
import hashlib
import json
import math
from pathlib import Path
import statistics


def validate_sequence(directory, expected, *, enabled=None, require_motion=None, max_waiting=1):
    directory=Path(directory).resolve()
    rows=[json.loads(line) for line in (directory/'frames.jsonl').read_text(encoding='utf-8').splitlines() if line]
    if len(rows)!=expected+2 or rows[0].get('type')!='metadata' or rows[-1].get('type')!='terminal':
        raise ValueError('Incomplete sequence journal')
    meta,terminal=rows[0],rows[-1]
    frequency=meta.get('qpc_frequency',0)
    if meta.get('schema')!=1 or meta.get('performance_valid') is not False or not isinstance(frequency,int) or frequency<=0:
        raise ValueError('Invalid sequence metadata')
    if meta.get('expected')!=expected or terminal.get('success') is not True or terminal.get('written')!=expected or terminal.get('expected')!=expected:
        raise ValueError('Sequence did not complete successfully')
    frames=rows[1:-1];previous=None;shape=None;hashes={};capture_ms=[];period_ms=[]
    positions=[];rotations=[];reuse=0
    for index,row in enumerate(frames):
        if row.get('type')!='frame' or row.get('index')!=index:raise ValueError('Frame index gap')
        if not all(isinstance(row.get(k),int) and row[k]>0 for k in ('world_id','present_id')):
            raise ValueError('Invalid frame identity')
        if previous and (row['world_id']!=previous['world_id']+1 or row['present_id']<=previous['present_id']):
            raise ValueError('Nonconsecutive world frames or duplicate Present')
        ticks=[row.get(k) for k in ('world_begin_qpc','world_end_qpc','capture_begin_qpc','capture_end_qpc','forward_qpc','returned_qpc')]
        if not all(isinstance(v,int) and v>0 for v in ticks) or ticks!=sorted(ticks):raise ValueError('Invalid frame timing')
        if previous:
            if ticks[0]<previous['returned_qpc']:raise ValueError('Overlapping frame timing')
            period_ms.append((ticks[0]-previous['world_begin_qpc'])*1000/frequency)
        capture_ms.append((ticks[3]-ticks[2])*1000/frequency)
        if row.get('hresult') not in (0,0x087A0001) or row.get('all_zero') is not False:
            raise ValueError('Failed Present or blank capture')
        for stage in ('camera_begin','camera_end'):
            camera=row.get(stage,{})
            for key,count in (('eye',3),('rotation',9),('world_to_cam',16),('frustum',6)):
                values=camera.get(key,[])
                if len(values)!=count or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in values):
                    raise ValueError('Missing or invalid rendered camera')
        positions.append(row['camera_end']['eye']);rotations.append(row['camera_end']['rotation'])
        skip=row.get('planned_skip_mask',0)
        # Cascade 0 always renders; SunShadows.CadenceMaxWaiting bounds how many others may wait.
        if not isinstance(skip,int) or skip<0 or skip>14 or skip & 1 or skip.bit_count()>max_waiting:
            raise ValueError('Invalid cascade reuse mask')
        if enabled is not None:
            if row.get('cadence_known') is not True or row.get('enabled')!=int(enabled):raise ValueError('Candidate state not verified')
            if skip and (not enabled or not row.get('cadence')):raise ValueError('Unexpected reuse while disabled')
        reuse+=bool(skip)
        name=f'frame-{index:03}.ppm'
        if row.get('file')!=name:raise ValueError('Unexpected frame filename')
        path=directory/name
        if path.is_symlink() or path.resolve().parent!=directory:raise ValueError('Frame outside sequence directory')
        width,height=row.get('width',0),row.get('height',0)
        if not isinstance(width,int) or not isinstance(height,int) or width<=0 or height<=0 or width*height*3>16*1024**2:
            raise ValueError('Invalid image dimensions')
        if shape and shape!=(width,height):raise ValueError('Image dimensions changed')
        shape=(width,height)
        data=path.read_bytes();header=f'P6\n{width} {height}\n255\n'.encode()
        if not data.startswith(header) or len(data)!=len(header)+width*height*3 or not any(data[len(header):]):
            raise ValueError('Missing, truncated or blank image')
        hashes[name]=hashlib.sha256(data).hexdigest();previous=row
    travel=max(math.dist(positions[0],p) for p in positions)
    angles=[]
    for rotation in rotations:
        cosine=(sum(a*b for a,b in zip(rotations[0],rotation))-1)/2
        angles.append(math.degrees(math.acos(max(-1,min(1,cosine)))))
    turn=max(angles)
    if require_motion=='pan' and turn<.2:raise ValueError('Requested pan not observed in rendered camera')
    if require_motion in ('strafe','walk') and travel<2:raise ValueError(f'Requested {require_motion} not observed in rendered camera')
    return {'valid':True,'frame_count':expected,'first_world_id':frames[0]['world_id'],'last_world_id':frames[-1]['world_id'],
            'camera_travel_units':travel,'camera_turn_degrees':turn,'planned_reuse_frames':reuse,
            'capture_ms_median':statistics.median(capture_ms),'capture_ms_max':max(capture_ms),
            'world_period_ms_median':statistics.median(period_ms),'world_period_ms_max':max(period_ms),
            'frame_sha256':hashes,'performance_valid':False,'visual_quality_accepted':False}
