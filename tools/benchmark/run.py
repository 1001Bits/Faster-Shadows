"""Bounded private-desktop Fallout shadow benchmarks with raw validation traces."""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
import uuid
from datetime import datetime,timezone

import psutil

ROOT=Path(__file__).resolve().parents[2]
SUPPORT=ROOT/'.runtime-tests/support'
from prepare_game import ini,values,write
from slot import Slot,OWNER,atomic as atomic_json
from temporal import validate_sequence
import scheduling
import telemetry
sys.path.insert(0,str(ROOT/'tools'))
from provenance import verify_sources

request=qpc=rt=None
HOST_EXIT_RECEIPT_SECONDS=45
CAPTURE_DISK_FLOOR=2  # GiB free that a consecutive-frame capture never goes below

def load_support(manifest):
    global request,qpc,rt,HOST_EXIT_RECEIPT_SECONDS
    current={p.relative_to(SUPPORT).as_posix():sha(p) for p in SUPPORT.rglob('*')
             if p.is_file() and not any(part in {'.git','__pycache__'} for part in p.parts)}
    if manifest['support']!=current:raise RuntimeError('Private support inputs differ from the tested build')
    sys.path.insert(0,str(SUPPORT))
    desktop=importlib.import_module('private_desktop')
    request,qpc,HOST_EXIT_RECEIPT_SECONDS=desktop.request,desktop.qpc,desktop.HOST_EXIT_RECEIPT_SECONDS
    rt=importlib.import_module('rendered_throughput')

SHARED=Path(r'C:\Development\Realistic Reflections FO4 - Glass\.tmp\launch-coordination\priority-window.json')
# Cell form IDs from Fallout4.esm, so a coc that lands elsewhere fails instead of measuring the wrong place.
SCENES={'DiamondCityExt':0xE50C,'DiamondCityOrigin':0xFEE,'GoodneighborOrigin':0x54BE9,
        'SanctuaryExt':0xDD60,'CorvegaAssemblyPlantExt':0xDF02,'LexingtonExt':0xDEBF}

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path,attempts=40):
    # The desktop host replaces its status files while the runner polls them; Windows denies a
    # read that meets the replace, so retry briefly instead of failing the launch.
    for attempt in range(attempts):
        try:return json.loads(Path(path).read_text(encoding='utf-8-sig'))
        except PermissionError:
            if attempt+1==attempts:raise
            time.sleep(.05)

def replace(source,target,attempts=40):
    # The in-game probe reads commands.cmd while the runner replaces it; Windows denies the
    # replace for that moment (seen 2026-10-22 on a busy shared machine), so retry briefly.
    for attempt in range(attempts):
        try:return os.replace(source,target)
        except PermissionError:
            if attempt+1==attempts:raise
            time.sleep(.05)

def stage_private_artifacts(game,manifest,baseline=False):
    """Refresh reused labs and verify the files the private host will actually load."""
    game=Path(game).resolve()
    artifacts=[(ROOT/'build-benchmark/Release'/name,relative,manifest['binaries'][name])
        for name,relative in (
            ('DTLBenchLoader.exe','DTLBenchLoader.exe'),
            ('FPSFolderGuard.dll','FPSFolderGuard.dll'),
            ('DTLBenchFO4.dll','Data/F4SE/Plugins/DTLBenchFO4.dll'))]
    guard='private-loader/build/DTLBenchInputGuard.dll'
    artifacts.append((SUPPORT/guard,'DTLBenchInputGuard.dll',manifest['support'][guard]))
    candidate=game/'Data/F4SE/Plugins/FasterShadows.dll'
    if candidate.is_symlink() or not candidate.resolve().is_relative_to(game):
        raise RuntimeError('Candidate must be a private lab copy')
    if not baseline:
        name='FasterShadows.dll'
        artifacts.append((ROOT/'build-benchmark/Release'/name,'Data/F4SE/Plugins/'+name,manifest['binaries'][name]))
    recorded={}
    for source,relative,expected in artifacts:
        target=game/relative
        if target.is_symlink() or not target.resolve().is_relative_to(game):
            raise RuntimeError('Private artifact target is redirected: '+relative)
        temporary=target.with_name(target.name+'.'+uuid.uuid4().hex+'.tmp')
        try:
            # Replace the directory entry, so a stale hard link cannot modify another lab.
            shutil.copy2(source,temporary)
            if sha(temporary)!=expected:raise RuntimeError('Mismatched staged private artifact: '+relative)
            os.replace(temporary,target)
        finally:
            temporary.unlink(missing_ok=True)
        recorded[relative]=sha(target)
        if recorded[relative]!=expected:raise RuntimeError('Changed private artifact after staging: '+relative)
    if baseline:candidate.unlink(missing_ok=True)
    return recorded

def game_timescale(value):
    """Zero freezes the scene; 20 exercises the ordinary advancing game clock."""
    try:result=float(value)
    except (TypeError,ValueError):raise argparse.ArgumentTypeError('Timescale must be a finite non-negative number') from None
    if not math.isfinite(result) or result<0:
        raise argparse.ArgumentTypeError('Timescale must be a finite non-negative number')
    return result

SEQUENCE_MOTIONS=('still','pan','strafe','walk')


def sequence_motions(value):
    """The temporal check's motions, comma-separated, each once: still, pan, strafe, walk."""
    motions=tuple(value.split(','))
    if len(set(motions))!=len(motions) or not set(motions)<=set(SEQUENCE_MOTIONS):
        raise argparse.ArgumentTypeError('Sequence motions are a comma list of '+', '.join(SEQUENCE_MOTIONS))
    return motions


def shadow_distance(value):
    """fDirShadowDistance for the run; 0 keeps the lab's own. A short one (e.g. 3000) leaves far
    cascades without casters, which SkipEmptyCascades needs to show anything."""
    try:result=float(value)
    except (TypeError,ValueError):raise argparse.ArgumentTypeError('Shadow distance must be 0 or 1000-30000') from None
    if not (result==0 or 1000<=result<=30000):raise argparse.ArgumentTypeError('Shadow distance must be 0 or 1000-30000')
    return result

def game_prefs_configuration(user,distance=0):
    """The lab's Fallout4Prefs.ini for one run: the lab's original prefs, plus fDirShadowDistance when
    the run shortens it. fDirShadowDistance is an INIPrefSettingCollection setting (OG 0x2B4FCF1), so
    only Fallout4Prefs.ini sets it, and the engine copies it into BSShadowLight once, in
    BSShaderManager::Initialize (OG 0x27D59BC): neither Fallout4Custom.ini nor the console's setini
    reaches it. The original is kept beside the prefs on first use and every start rebuilds the prefs
    from it, so no override outlives its run."""
    documents=user/'Documents/My Games/Fallout4'
    original=documents/'Fallout4Prefs.original.ini'
    if not original.exists():shutil.copy2(documents/'Fallout4Prefs.ini',original)
    configuration=ini(original.read_text(errors='replace'))
    if distance:values(configuration,'Display',fDirShadowDistance=f'{distance:.4f}')
    return configuration

def pose_matches(state, position, yaw):
    """Movement is deferred: its command acknowledgement can still contain the old pose."""
    if not state.get('world_ready') or state.get('loading') or not state.get('camera_first_person'):
        return False
    actual=state.get('position',[]);angles=state.get('angles_radians',[])
    if len(actual)!=3 or len(angles)!=3:return False
    if not all(math.isfinite(v) for v in [*actual,*angles,*position,yaw]):return False
    turn=math.remainder(angles[2]-math.radians(yaw),math.tau)
    return all(abs(a-b)<=.1 for a,b in zip(actual,position)) and abs(turn)<=math.radians(.05)


def measurement_plan(matrix=False,baseline=False,mask=14):
    if mask not in range(32):raise ValueError('Optimization mask must be in [0,31]')
    if matrix:
        if baseline:raise ValueError('A matrix needs a loaded candidate')
        return [(True,value,f'bit-{bit}-{index+1:02d}-mask-{value}')
                for bit in (1,2,4,8,16) for index,value in enumerate((0,bit,bit,0))]
    order=[False]*4 if baseline else [False,True,True,False,True,False,False,True]
    return [(enabled,mask,f'{index+1:02d}-'+('on' if enabled else 'off')) for index,enabled in enumerate(order)]

def expected_hook_mask(configuration):
    """Sun-state mask: bits 0-2 vtable sites (sun update, point begin, point append),
    bits 3-5 the spot, sun and rain-occlusion registration entries, bit 6 the sun Render site
    (staggered cascades)."""
    def flag(section,key):return configuration.getboolean(section,key,fallback=True)
    actors=flag('ActorShadows','Enabled')
    sun=flag('SunShadows','Enabled') or actors
    point=flag('PointShadows','Enabled') or (actors and flag('ActorShadows','PointLights'))
    spot=point and flag('PointShadows','SpotLights')
    rain=flag('RainOcclusion','Enabled')
    cadence=configuration.getboolean('SunShadows','CascadeCadence',fallback=False)
    sun=sun or cadence  # the cadence plans in the sun update and drops at the sun registration entry
    # The sun cull worker and sun-glare poll are call-site changes that follow the runtime
    # toggle, which the sun update polls: they keep that vtable hook (bit 0) on their own.
    engine=(configuration.getboolean('SunShadows','CullOnWorkerThread',fallback=False) or
            configuration.getboolean('Optimizations','SunGlareNoFlush',fallback=False))
    return (1 if sun or rain or engine else 0)|(6 if point else 0)|(8 if spot else 0)|(16 if sun else 0)|(32 if rain else 0)|(64 if cadence else 0)

def plugin_configuration(dry_run=False,diagnostics=True,overrides=()):
    # DryRun=1 with diagnostics evaluates and counts candidates without removal.
    # With Diagnostics=0 caster policies forward directly; cadence is independent.
    configuration=ini((ROOT/'package/FasterShadows.ini').read_text())
    values(configuration,'General',DryRun=int(dry_run),Diagnostics=int(diagnostics),ToggleKey=0,ReportSeconds=600)
    for item in overrides:
        # Section.Key=Value, for keys the shipped INI already has (no silent typos).
        name,_,value=item.partition('=');section,_,key=name.partition('.')
        if not value or not configuration.has_option(section,key):raise ValueError('Unknown INI override '+item)
        values(configuration,section,**{key:value})
    return configuration

def original_user_files():
    # Read-only before/after evidence supplements process-local redirection.
    documents=Path.home()/'Documents/My Games/Fallout4'
    local=Path(os.environ['LOCALAPPDATA'])/'Fallout4'
    paths=[documents/('Fallout4'+suffix+'.ini') for suffix in ['', 'Custom', 'Prefs']]
    paths += [local/'plugins.txt',local/'loadorder.txt']
    return {str(path):sha(path) if path.is_file() else None for path in paths}

class Runner:
    def __init__(self,runtime,label,smoke=False,baseline=False,minimum_ram=5,*,slot,lab,mask=14,dry_run=False,diagnostics=True,overrides=(),scheduling_mode='default',culling_batch=False,shadow_distance=0):
        if not re.fullmatch(r'[A-Za-z0-9_-]+',label):raise ValueError('Simple fresh label required')
        self.shadow_distance=globals()['shadow_distance'](shadow_distance)
        if scheduling_mode not in scheduling.MODES:raise ValueError('Unknown scheduling mode')
        # FO4's batched culling (bCullingBatch) follows the plugin's Enabled switch; needs the private DLL.
        if culling_batch and baseline:raise ValueError('A plugin-absent control cannot toggle culling batches')
        self.culling_batch=culling_batch
        self.scheduling_mode=scheduling_mode;self.sampler=None
        self.runtime=runtime;self.smoke=smoke;self.baseline=baseline;self.label=label
        self.minimum_ram=2.5 if smoke else minimum_ram
        if mask not in range(32):raise ValueError('Optimization mask must be in [0,31]')
        self.slot=slot;self.mask=mask;self.overrides=list(overrides)
        self.dry_run=dry_run;self.diagnostics=diagnostics
        if self.overrides:
            configuration=plugin_configuration(dry_run,diagnostics,self.overrides)
            self.dry_run=configuration.getboolean('General','DryRun')
            self.diagnostics=configuration.getboolean('General','Diagnostics')
        self.lab=Path(lab).resolve()
        if not self.lab.is_relative_to((ROOT/'.runtime-tests/labs').resolve()):
            raise RuntimeError('A fresh versioned lab under .runtime-tests/labs is required')
        self.setup=read(self.lab/'setup.json')
        if self.setup.get('schema')!=4 or self.setup.get('plugin_name')!='FasterShadows.dll' or self.setup['runtime']!=runtime:
            raise RuntimeError('Historical/foreign fixtures cannot be migrated in place')
        self.game=Path(self.setup['game']).resolve();self.user=Path(self.setup['user']).resolve()
        if not all(path.is_relative_to(self.lab) for path in (self.game,self.user)):
            raise RuntimeError('Lab game and user folders must be private workspace copies')
        if any(p.name.casefold()=='falloutperformancesolution.dll' for p in (self.game/'Data/F4SE/Plugins').iterdir()):
            raise RuntimeError('Legacy candidate DLL would cause duplicate hooks; prepare a fresh lab')
        self.run=ROOT/'.runtime-tests/runs'/label
        self.run.mkdir(parents=True,exist_ok=False)
        self.probe=self.run/'probe';self.probe.mkdir()
        self.sequence=0;self.host=None;self.pid=None;self.results=[]
        self.case={'runtime':runtime,'label':label,'smoke_only':smoke,'plugin_absent_baseline':baseline,'dry_run':self.dry_run,'diagnostics':self.diagnostics,'ini_overrides':list(overrides),'setup':self.setup,
            'rendered_throughput_requested':True,'metric':'completed_offscreen_world_renders_per_second',
            'displayed_fps_valid':False,'gameplay_visual_review_pending':True,
            'minimum_starting_ram_gib':self.minimum_ram,
            'optimization_mask':mask,'volume_analysis_only':True,
            'started_unix':time.time(),'candidate_sha256':sha(ROOT/'build-benchmark/Release/FasterShadows.dll'),
            'scheduling_mode':scheduling_mode,'culling_batch_toggle':culling_batch,'original_user_files_before':original_user_files(),
            **({'shadow_distance':self.shadow_distance} if self.shadow_distance else {})}

    def save(self):atomic_json(self.run/'case.json',self.case)

    def start(self):
        gate=self.slot.gate(claimed=True,minimum_ram=self.minimum_ram)
        manifest=read(ROOT/'build-benchmark/manifest.json')
        if manifest['protocol']!=4:raise RuntimeError('Private diagnostics need rebuilding')
        verify_sources(manifest['sources'])
        for name,digest in manifest['binaries'].items():
            if sha(ROOT/'build-benchmark/Release'/name)!=digest:raise RuntimeError('Mismatched private artifact: '+name)
        for name,digest in manifest['support'].items():
            if sha(SUPPORT/name)!=digest:raise RuntimeError('Changed diagnostic support input: '+name)
        load_support(manifest)  # Hash verification precedes importing copied Python support.
        self.case['runtime_artifact_sha256']=stage_private_artifacts(self.game,manifest,self.baseline)
        configuration=plugin_configuration(self.dry_run,self.diagnostics,self.overrides)
        settings_path=self.game/'Data/F4SE/Plugins/FasterShadows.ini'
        write(settings_path,configuration)
        self.case['plugin_settings']=settings_path.read_text()
        self.case['plugin_settings_sha256']=sha(settings_path)
        prefs_path=self.user/'Documents/My Games/Fallout4/Fallout4Prefs.ini'
        write(prefs_path,game_prefs_configuration(self.user,self.shadow_distance))
        self.case['game_prefs_sha256']=sha(prefs_path)
        self.case['candidate_sha256']=sha(ROOT/'build-benchmark/Release/FasterShadows.dll')
        self.case['folder_guard_sha256']=sha(self.game/'FPSFolderGuard.dll')
        self.case['probe_sha256']=sha(self.game/'Data/F4SE/Plugins/DTLBenchFO4.dll')
        self.case['loader_sha256']=sha(self.game/'DTLBenchLoader.exe')
        self.case['input_guard_sha256']=sha(self.game/'DTLBenchInputGuard.dll')
        environment={
            'DTL_BENCH_DIR':str(self.probe),'DTL_BENCH_THROUGHPUT':'1',
            'DTL_BENCH_GAME':str(self.game/'Fallout4.exe'),
            'DTL_BENCH_EXTENDER':str(self.game/('f4se_1_10_163.dll' if self.runtime=='og' else 'f4se_1_11_240.dll')),
            'DTL_BENCH_INPUT_GUARD':str(self.game/'DTLBenchInputGuard.dll'),
            'DTL_BENCH_INJECTION':'loadlibrary' if self.runtime=='og' else 'ordinal1',
            'F4SE_DLL':'f4se_1_10_163.dll' if self.runtime=='og' else 'f4se_1_11_240.dll',
            'F4SE_RUNTIME':'Fallout4.exe','F4SE_WAITFORDEBUGGER':'0',
            'FPS_BENCH_USER_ROOT':str(self.user),'FPS_BENCH_FOLDER_GUARD':str(self.game/'FPSFolderGuard.dll'),
            'SteamAppId':'377160','SteamNoOverlayUI':'1','SteamNoOverlayUIDrawing':'1',
            'LOCALAPPDATA':str(self.user/'AppData/Local'),'APPDATA':str(self.user/'AppData/Roaming'),
            'TEMP':str(self.user/'Temp'),'TMP':str(self.user/'Temp')}
        if os.environ.get('FPS_BENCH_STARTUP_DIAG')=='1':
            if not self.smoke:raise RuntimeError('Startup fault diagnosis is restricted to smoke runs')
            environment['FPS_BENCH_STARTUP_DIAG']='1'
        self.case['startup_diagnosis']=environment.get('FPS_BENCH_STARTUP_DIAG')=='1'
        if self.culling_batch:environment['FPS_BENCH_CULLING_BATCH']='1'
        atomic_json(self.run/'environment.json',environment)
        self.case['admission']=gate
        self.case['gpu_before_launch']=telemetry.gpu_state()  # heat soak left by a previous game
        # Calibrated on the quiet machine: a loading game preempts the spins and reads low.
        self.case['tsc_hz']=telemetry.tsc_hz()
        self.save()
        # The same exclusively owned claim is freshly checked before creation.
        self.case['final_admission']=self.slot.gate(claimed=True,minimum_ram=self.minimum_ram);self.save()
        command=[sys.executable,str(SUPPORT/'private_desktop.py'),'host','--session',str(self.run/'desktop'),
            '--exe',str(self.game/'DTLBenchLoader.exe'),'--cwd',str(self.game),
            '--target-exe',str(self.game/'Fallout4.exe'),'--env-file',str(self.run/'environment.json'),
            '--max-seconds',str(max(1,int(min(600,self.slot.work_deadline-time.monotonic()))))]
        launched=time.time()  # a FasterShadows.log older than this belongs to an earlier launch
        with (self.run/'host-stdout.log').open('w') as out,(self.run/'host-stderr.log').open('w') as err:
            self.slot.restoration_verified=False
            self.host=self.slot.spawn(command,stdout=out,stderr=err)
        atomic_json(ROOT/'.runtime-tests/coordination-status.json',{'state':'active','label':self.label,
            'owner_pid':os.getpid(),'host_pid':self.host.pid,'game_pid':None,'updated_unix':time.time(),
            'local_status_only':True,'exclusive_claim_nonce':self.slot.token})
        print('Started private host',self.host.pid,'for',self.runtime,flush=True)
        deadline=time.monotonic()+180;no_game_deadline=time.monotonic()+60;next_progress=0
        while not (self.probe/'ready.json').exists():
            if self.host.poll() is not None:raise RuntimeError('Private host exited during startup')
            self.slot.gate(claimed=True,minimum_ram=1.0)
            if time.monotonic()>no_game_deadline and not self.slot.seen_games:
                raise TimeoutError('No owned game appeared within 60 seconds')
            private_path=self.run/'desktop/status.json'
            if private_path.exists():
                private=read(private_path)
                owned=private.get('target_pids',[])
                if len(owned)>1:raise RuntimeError('More than one owned game started')
                if not owned and time.monotonic()>no_game_deadline:raise TimeoutError('No owned game appeared within 60 seconds')
            if time.monotonic()>deadline:raise TimeoutError('F4SE/game data did not become ready')
            if time.monotonic()>next_progress:
                print('Waiting for game data; RAM GiB',round(psutil.virtual_memory().available/2**30,2),flush=True)
                next_progress=time.monotonic()+15
            time.sleep(.5)
        ready=read(self.probe/'ready.json');self.pid=ready['pid']
        private=read(self.run/'desktop/status.json')
        if self.pid not in private['target_pids']:raise RuntimeError('Ready PID is not our owned game')
        if private['visible_owned_windows_on_input_desktop'] or not private['cursor_clip_unchanged']:
            raise RuntimeError('Input desktop isolation failed')
        log=self.user/'Documents/My Games/Fallout4/F4SE/FasterShadows.log'
        if not self.baseline and (not log.exists() or log.stat().st_mtime<launched-1 or
                                  'Installed Fallout shadow rules' not in log.read_text(errors='replace')):
            raise RuntimeError('Candidate did not install verified hooks (no FasterShadows.log from this launch)')
        self.case['pid']=self.pid;self.case['ready']=ready;self.save()
        self.sampler=telemetry.Sampler(self.slot.job_pids)
        self.case['cpu_topology']=self.sampler.topology
        # Identical for native and candidate; 'default' leaves Windows' background treatment in place.
        self.case['scheduling']=scheduling.apply(self.pid,self.scheduling_mode,self.sampler.topology);self.save()
        atomic_json(ROOT/'.runtime-tests/coordination-status.json',{'state':'active','label':self.label,
            'owner_pid':os.getpid(),'host_pid':self.host.pid,'game_pid':self.pid,'updated_unix':time.time(),
            'local_status_only':True,'exclusive_claim_nonce':self.slot.token})
        print('Baseline game ready' if self.baseline else 'Candidate hooks installed','; owned game PID',self.pid,flush=True)

    def command(self,*lines,timeout=45):
        self.sequence+=1;number=self.sequence
        temp=self.probe/'commands.tmp'
        temp.write_text(str(number)+'\n'+'\n'.join(lines)+'\n')
        replace(temp,self.probe/'commands.cmd')
        deadline=time.monotonic()+timeout
        ack=self.probe/(str(number)+'.ack.json')
        while True:
            if self.host and self.host.poll() is not None:raise RuntimeError('Owned host exited')
            if time.monotonic()>deadline:raise TimeoutError('Command task timed out: '+str(lines))
            self.slot.gate(claimed=True,minimum_ram=1.0)
            if ack.exists():
                try:
                    value=read(ack)
                    break
                except PermissionError:
                    # The probe publishes a closed file with MoveFileExW. On
                    # Windows it can briefly be visible but sharing-locked.
                    # Retry only the read, within the original task deadline.
                    pass
            time.sleep(.03)
        if value['error'] or value['executed']!=value['requested']:raise RuntimeError('Command failed: '+str(value))
        return value['state']

    def pause(self,seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if self.host.poll() is not None:raise RuntimeError('Owned host exited')
            self.slot.gate(claimed=True,minimum_ram=1.0)
            time.sleep(min(.5,max(0,deadline-time.monotonic())))

    def scene(self,name='DiamondCityExt',hour=12,yaw=0,timescale=0):
        timescale=game_timescale(timescale)  # reject bad input before issuing any game command
        distance=getattr(self,'shadow_distance',0)  # set in the game INI before launch (Runner)
        state=self.command('coc '+name)
        deadline=time.monotonic()+120
        while not state['world_ready'] or state['main_menu'] or state['loading']:
            if time.monotonic()>deadline:raise TimeoutError('Scene did not become ready')
            self.pause(1);state=self.command('@status')
        if name in SCENES and state['cell_id']!=SCENES[name]:
            raise RuntimeError('Wrong named benchmark cell: '+str(state['cell_id']))
        state=self.command(f'set timescale to {timescale:g}',f'set gamehour to {hour}','fw 2b52a','tgm','tai','tcl',
            'fov 75','player.setangle x 0',f'player.setangle z {yaw}','@present')
        self.pause(10)
        state=self.command('@status')
        if not state['world_ready'] or not state['camera_first_person']:
            raise RuntimeError('World/camera state is unsuitable for measurement')
        if not self.baseline:
            self.command('@fps-sun-state')
            hook_state=read(self.probe/'sun-state.json')
            configuration=ini(self.case['plugin_settings'])
            expected=expected_hook_mask(configuration)
            if hook_state['hook_mask']!=expected:raise RuntimeError('Unexpected installed feature hooks')
            self.case['installed_hook_state']=hook_state
        self.case['scene_origin']=state
        self.case['scene_parameters']={'name':name,'hour':hour,'yaw':yaw,'timescale':timescale,'weather':'2b52a','fov':75,
                                       **({'shadow_distance':distance} if distance else {})}
        self.save()
        if self.baseline:
            self.command('@capture native.ppm');self.pause(2)
        else:
            for active in [False,True]:
                self.command('@fps '+str(int(active)));self.pause(2)
                self.command('@capture '+('on.ppm' if active else 'off.ppm'));self.pause(2)
        print('Scene ready:',name,'position',state['position'],'yaw',yaw,flush=True)
        return state

    def wait_for_pose(self,position,yaw,timeout=10):
        deadline=time.monotonic()+timeout
        first_present=None
        while time.monotonic()<deadline:
            state=self.command('@status')
            present=state.get('present',{}).get('count')
            if pose_matches(state,position,yaw) and isinstance(present,int):
                if first_present is None:first_present=present
                if present-first_present>=2:return state
            else:first_present=None
            self.pause(.05)
        raise TimeoutError('Requested motion pose did not settle before capture')

    def capture_pose(self,name,position,yaw):
        before=self.wait_for_pose(position,yaw)
        requested=self.command('@capture '+name)
        capture=requested.get('capture',{})
        request_id=capture.get('request_id')
        if not isinstance(request_id,int) or request_id<=before.get('capture',{}).get('request_id',0):
            raise RuntimeError('Capture did not receive a fresh request ID')
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            after=self.command('@status');capture=after.get('capture',{})
            if not pose_matches(after,position,yaw):raise RuntimeError('Pose changed during capture')
            if capture.get('state')==4:raise RuntimeError('Capture failed: '+str(capture.get('error')))
            if capture.get('completed_id')==request_id and capture.get('state')==3:
                if Path(capture.get('path','')).resolve()!=(self.probe/name).resolve():
                    raise RuntimeError('Capture completed at an unexpected path')
                if capture.get('all_zero') or capture.get('width',0)<=0 or capture.get('height',0)<=0 or not (self.probe/name).is_file():
                    raise RuntimeError('Capture has no valid image')
                return {'requested_position':list(position),'requested_yaw':yaw,'before':before,'after':after,
                        'capture_request_id':request_id,'pose_verified':True}
            self.pause(.05)
        raise TimeoutError('Pose capture did not complete')

    def soak(self,seconds,still=False):
        """Untimed: the plugin stays on for `seconds` while the camera pans (FPSBenchmarkSetMotion), or
        holds still; a stability run, or a diagnostics run read from the [Census] line it logs at the end.
        No captures, no timing; the game must still answer at the end."""
        if self.baseline:raise RuntimeError('A soak needs the plugin')
        if not 10<=seconds<=600:raise ValueError('Soak seconds must be in [10,600]')
        self.command('@fps 1','@fps-stats reset',*([] if still else ['@fps-motion 1 45 30']))
        deadline=time.monotonic()+seconds;checks=0
        while time.monotonic()<deadline:
            self.slot.gate(claimed=True,minimum_ram=1.0)
            self.command('@status');checks+=1
            self.pause(min(5,max(0,deadline-time.monotonic())))
        # The [Census] line this sample logs carries the soak's totals (cadence waits, leaks, slot conflicts).
        self.command('@fps-motion 0 0 0','@fps-stats sample');self.pause(1)
        self.case['soak']={'seconds':seconds,'motion':'still' if still else 'pan 45 deg/s, 30 deg','status_checks':checks}
        self.save();print('Soak complete:',seconds,'s,',checks,'status checks',flush=True)

    def quit_check(self,seconds=60):
        """Untimed: `seconds` in the scene (the plugin on with the camera panning, or absent in a baseline
        run), then the game quits through the console (qqq), as a player quitting does, and must exit by
        itself. The private loader logs the game's exit code: 0 is a clean exit, 3221225477 (0xC0000005) an
        access violation. A game still running 120 s after qqq is recorded as a hang; stop() ends it."""
        if not 10<=seconds<=300:raise ValueError('Quit check seconds must be in [10,300]')
        if not self.baseline:self.command('@fps 1','@fps-motion 1 45 30')
        self.pause(seconds)
        if not self.baseline:self.command('@fps-motion 0 0 0')
        # qqq ends the game before the probe could acknowledge it, so it is sent without waiting.
        self.sequence+=1
        temp=self.probe/'commands.tmp'
        temp.write_text(f'{self.sequence}\nqqq\n')
        replace(temp,self.probe/'commands.cmd')
        sent=time.monotonic()
        while self.host.poll() is None and time.monotonic()-sent<=120:time.sleep(.5)
        result={'seconds':seconds,'plugin':'absent' if self.baseline else 'on','exit_seconds':round(time.monotonic()-sent,1)}
        if self.host.poll() is None:
            result.update(exit_code=None,clean=False,hang=True)
        else:
            loader=self.probe/'bench-loader.log'
            codes=re.findall(r'game exited code=(\d+)',loader.read_text(errors='replace')) if loader.exists() else []
            if not codes:raise RuntimeError('The private loader logged no game exit code')
            code=int(codes[-1])
            result.update(exit_code=code,exit_hex=f'0x{code:08X}',clean=code==0,hang=False)
        self.case['quit_check']=result
        self.save();print('Quit check:',result,flush=True)

    def camera_motion_check(self):
        """Untimed: the timing windows' camera motion (FPSBenchmarkSetMotion via @fps-motion) turns,
        strafes and walks the player, and stopping it puts the player back exactly. No captures."""
        if self.baseline:raise RuntimeError('Camera motion needs the plugin')
        results=self.case['camera_motion_check']={}
        for name,motion,key,least in (('pan',(1,45,30),'angles_radians',.1),('strafe',(2,300,300),'position',50),
                                      ('walk',(3,100,300),'position',25)):
            before=self.command('@status')
            self.command('@fps-motion {} {:g} {:g}'.format(*motion));self.pause(.5)
            moving=self.command('@status')
            self.command('@fps-motion 0 0 0');self.pause(1)
            after=self.command('@status')
            if key=='angles_radians':change=(moving[key][2]-before[key][2]+math.pi)%(2*math.pi)-math.pi
            else:change=math.hypot(moving[key][0]-before[key][0],moving[key][1]-before[key][1])  # strafe: x; walk: the heading
            results[name]={'motion':motion,'before':before[key],'moving':moving[key],'after':after[key],'moved':abs(change)}
            self.save()
            if not abs(change)>=least:raise RuntimeError(f'Camera {name} did not move: {results[name]}')
            if any(not abs(a-b)<=1e-4 for a,b in zip(after[key],before[key])):
                raise RuntimeError(f'Camera {name} was not put back: {results[name]}')
        print('Camera motion verified:',{name:round(r['moved'],3) for name,r in results.items()},flush=True)

    def motion_check(self,yaw=0):
        # Settled-pose comparisons, not first-frame/temporal tests. Console movement
        # is deferred; combining it with @capture captured the old home view.
        # Require target-pose readback and later presents before requesting capture.
        if self.baseline:raise RuntimeError('Motion check needs the plugin')
        x,y,z=self.case['scene_origin']['position']
        origin=(x,y,z)
        poses=[('turn +1.5',[f'player.setangle z {yaw+1.5}'],origin,yaw+1.5),
               ('turn -1.5',[f'player.setangle z {yaw-1.5}'],origin,yaw-1.5),
               ('move x +40',[f'player.setpos x {x+40:.6f}'],(x+40,y,z),yaw),
               ('move y +40',[f'player.setpos y {y+40:.6f}'],(x,y+40,z),yaw)]
        home=[f'player.setangle z {yaw}',f'player.setpos x {x:.6f}',f'player.setpos y {y:.6f}']
        captures=[]
        self.case['motion_check']={'mode':'settled_pose','transient_quality_valid':False,
                                   'poses':[label for label,_,_,_ in poses],'captures':captures}
        self.save()
        for active in [False,True]:
            self.command('@fps '+str(int(active)),*home);self.pause(3)
            for index,(label,commands,position,angle) in enumerate(poses):
                self.wait_for_pose(origin,yaw)
                name=f'motion-{"on" if active else "off"}-{index}.ppm'
                self.command(*commands)
                observation=self.capture_pose(name,position,angle)
                captures.append({'pose':label,'state':'on' if active else 'off','file':name,**observation})
                self.save()
                self.command(*home);self.pause(2)
        self.save()
        print('Motion captures:',len(captures),flush=True)

    def temporal_check(self,yaw=0,frames=32,pacing=33,pan_step=.25,strafe_step=4,settle=0,motions=SEQUENCE_MOTIONS[:3]):
        """An OFF then an ON sequence per motion. A walk steps along the heading by strafe_step."""
        if self.baseline:raise RuntimeError('Temporal comparison requires candidate control')
        motions=sequence_motions(','.join(motions))
        if not (0<pan_step<=5 and 0<strafe_step<=32):raise ValueError('Motion steps must be in (0,5] degrees and (0,32] units')
        if not 0<=settle<=60:raise ValueError('Settle seconds must be in [0,60]')
        origin=self.case['scene_origin']['position']
        home=[f'player.setangle z {yaw}',f'player.setpos x {origin[0]:.6f}',f'player.setpos y {origin[1]:.6f}']
        self.case['temporal_check']={'schema':1,'performance_valid':False,'visual_quality_accepted':False,
            'motion_kind':'console-command steps; recorded camera determines actual trajectory',
            'pan_step_degrees':pan_step,'strafe_step_units':strafe_step,'settle_seconds':settle,'motions':list(motions),
            'frames_per_sequence':frames,'pacing_ms':pacing,'actor_ai_enable_requested':True,'sequences':[]}
        self.save()
        self.command('tai')  # scene() disabled AI; allow moving actors during these visual checks.
        configured=ini(self.case['plugin_settings'])
        # The staggered comparison rules (CadencePolicy=1) have no waiting cap: with three cascades,
        # middle and far may both wait.
        waiting=(2 if configured.getint('SunShadows','CadencePolicy',fallback=0)==1 else
                 configured.getint('SunShadows','CadenceMaxWaiting',fallback=1))
        # Heavy scenes keep adapting exposure after load; the first (OFF) sequence would then
        # differ from the later ON one in brightness alone. Hold the home pose until it settles.
        if settle:self.command(*home);self.pause(settle)
        for active in (False,True):
            self.command('@fps '+str(int(active)),*home);self.pause(2)
            for motion in motions:
                self.command(*home);self.wait_for_pose(origin,yaw);self.pause(1)
                name=f'sequence-{"on" if active else "off"}-{motion}'
                state=self.command(f'@sequence {name} {frames} {pacing}')
                request_id=state.get('sequence',{}).get('request_id')
                if not isinstance(request_id,int) or request_id<=0:raise RuntimeError('Sequence was not acknowledged')
                item={'name':name,'enabled':active,'motion':motion,'request_id':request_id,'commands':[]}
                self.case['temporal_check']['sequences'].append(item);self.save()
                deadline=time.monotonic()+40;step=0
                while state.get('sequence',{}).get('active'):
                    if time.monotonic()>deadline:raise TimeoutError('Temporal capture did not complete')
                    # Each sequence writes ~0.3 GB of frames; on a shared, nearly full drive stop before it fills.
                    if shutil.disk_usage(self.probe).free<CAPTURE_DISK_FLOOR*2**30:
                        raise RuntimeError(f'Free disk under {CAPTURE_DISK_FLOOR} GiB during capture; stopped before filling the drive')
                    if motion!='still' and step<10:
                        step+=1
                        heading=math.radians(yaw)  # forward is (sin, cos) of the heading
                        commands=([f'player.setangle z {yaw+pan_step*step:.6f}'] if motion=='pan' else
                                  [f'player.setpos x {origin[0]+strafe_step*step:.6f}'] if motion=='strafe' else
                                  [f'player.setpos x {origin[0]+strafe_step*step*math.sin(heading):.6f}',
                                   f'player.setpos y {origin[1]+strafe_step*step*math.cos(heading):.6f}'])
                        state=self.command(*commands)
                        item['commands'].append({'command':'; '.join(commands),'ack':state});self.save()
                    else:state=self.command('@status')
                    self.pause(.03)
                receipt=state.get('sequence',{})
                item['receipt']=receipt;self.save()
                if receipt.get('request_id')!=request_id or not receipt.get('success') or receipt.get('error'):
                    raise RuntimeError('Temporal capture failed: '+str(receipt))
                item['validation']=validate_sequence(self.probe/name,frames,enabled=active,
                    require_motion=None if motion=='still' else motion,max_waiting=waiting)
                self.save()
                print('Sequence verified:',name,'frames',frames,'reuse',item['validation']['planned_reuse_frames'],flush=True)
        self.command(*home)
        configuration=ini(self.case['plugin_settings'])
        reuse_expected=configuration.getboolean('SunShadows','CascadeCadence',fallback=False) and any(
            configuration.getint('SunShadows',key,fallback=1)>1 for key in ('MidCascadeInterval','FarCascadeInterval'))
        reused=sum(item['validation']['planned_reuse_frames'] for item in self.case['temporal_check']['sequences'] if item['enabled'])
        self.case['temporal_check']['reuse_expected']=reuse_expected
        self.case['temporal_check']['planned_reuse_frames']=reused
        self.save()
        if reuse_expected and not reused:raise RuntimeError('Temporal test did not exercise cascade reuse')

    def sun_state(self):
        if self.baseline:return None
        self.command('@fps-sun-state')
        return read(self.probe/'sun-state.json')

    def window(self,enabled,seconds,tag,mask=None,motion=None):
        """motion: (kind, rate, amplitude) for FPSBenchmarkSetMotion (1 turn, 2 strafe, 3 walk). It starts a
        second before the measured interval, so every window times the same stretch of one path, and
        stops (restoring the pose) before the scene check after it."""
        if motion and self.baseline:raise ValueError('Camera motion needs the candidate plugin')
        self.slot.gate(claimed=True,minimum_ram=1.0)
        if not self.baseline:
            mask=self.mask if mask is None else mask
            self.command('@fps-options '+str(mask),'@fps '+str(int(enabled)))
        self.pause(3)
        # Outside the timing interval: retain the actual sun directions instead of
        # treating an advancing clock as proof that the engine updated its light.
        sun_before=self.sun_state()
        if not self.baseline:self.command('@fps-stats reset','@fps-optimizations reset')
        gpu_before=telemetry.gpu_state()
        started=rt.start(self.command,self.pause)
        before=self.command('@status');frequency=read(self.run/'desktop/status.json')['qpc_frequency']
        if motion:self.command('@fps-motion {} {:g} {:g}'.format(*motion));self.pause(1)
        self.sampler.prime();machine=[]
        begin=qpc();deadline=time.monotonic()+seconds;resources=[]
        process=psutil.Process(self.pid);process.cpu_percent()
        while time.monotonic()<deadline:
            self.slot.gate(claimed=True,minimum_ram=1.0)
            resources.append({'qpc':qpc(),'available_ram_gib':psutil.virtual_memory().available/2**30,
                'game_working_set_mib':process.memory_info().rss/2**20,'game_cpu_percent':process.cpu_percent(),
                'io_read_bytes':process.io_counters().read_bytes})
            self.pause(min(.5,max(0,deadline-time.monotonic())))
            machine.append(self.sampler.sample(self.pid))
        end=qpc();drain=rt.drain(self.command,self.pause)
        if motion:self.command('@fps-motion 0 0 0');self.pause(1)
        gpu_after=telemetry.gpu_state()
        if self.baseline:
            after=self.command('@status');counters={key:0 for key in ['sun_tested','sun_candidates','point_tested','point_candidates','other']};optimizations=None
        else:
            after=self.command('@fps-stats sample','@fps-optimizations sample','@status');counters=read(self.probe/'shadow-counters.json')
            optimizations=read(self.probe/'optimization-counters.json')
            if optimizations['mask']!=mask or optimizations['enabled']!=int(enabled) or optimizations['dry_run']!=int(self.dry_run) or optimizations['volume_analysis_only']!=1:
                raise RuntimeError('Optimization controls/readback differ from measured configuration')
        sun_after=self.sun_state()
        records=[json.loads(line) for line in (self.probe/'rendered-throughput.jsonl').read_text().splitlines()]
        measurement={'pid':self.pid,'start_qpc':begin,'end_qpc':end,'qpc_frequency':frequency,
            'rendered_throughput':{'started':started,'drain':drain}}
        validation=rt.validate(records,measurement,self.case,minimum_frames=60,minimum_seconds=seconds-.1)
        selected=[f for f in records if f.get('type')=='world_frame' and f.get('window')==started['window'] and
            begin<=f['begin_qpc'] and f['end_qpc']<=end]
        cpu=[(f['world_end_qpc']-f['begin_qpc'])*1000/frequency for f in selected]
        worldintervals=[(b['end_qpc']-a['end_qpc'])*1000/frequency for a,b in zip(selected,selected[1:])]
        summary={'tag':tag,'enabled':enabled,'motion':motion,'optimization_mask':mask,'optimization_counters':optimizations,'validation':validation,'measurement':measurement,'counters':counters,
            'sun_state_before':sun_before,'sun_state_after':sun_after,
            'state_before':before,'state_after':after,'resources':resources,'frames':len(selected),
            'machine':telemetry.summarize(machine,self.sampler.topology),'machine_samples':machine,
            'gpu_before':gpu_before,'gpu_after':gpu_after,
            'cpu_world_mean_ms':statistics.mean(cpu) if cpu else None,
            'cpu_world_median_ms':statistics.median(cpu) if cpu else None,
            'frame_interval_median_ms':statistics.median(worldintervals) if worldintervals else None,
            'frame_interval_p95_ms':sorted(worldintervals)[int((len(worldintervals)-1)*.95)] if worldintervals else None,
            'mean_ia_primitives':statistics.mean(f['activity']['ia_primitives'] for f in selected) if selected else None,
            'mean_vs_invocations':statistics.mean(f['activity']['vs_invocations'] for f in selected) if selected else None}
        self.results.append(summary);atomic_json(self.run/(tag+'.json'),summary)
        self.case['windows']=self.results;self.save()
        print(tag,'valid=',validation['valid'],'world renders/s=',validation.get('completed_world_renders_per_second'),
            'candidates=',counters,'issues=',validation['issues'],flush=True)
        if not validation['valid']:raise RuntimeError('Measured window failed validation')
        if before['cell_id']!=after['cell_id'] or before['position']!=after['position'] or before['camera_state']!=after['camera_state']:
            raise RuntimeError('Scene/camera changed during measurement')
        return summary

    def stop(self):
        if self.host is None:
            if self.baseline:
                shutil.copy2(ROOT/'build-benchmark/Release/FasterShadows.dll',self.game/'Data/F4SE/Plugins/FasterShadows.dll')
            self.slot.restoration_verified=original_user_files()==self.case['original_user_files_before']
            return
        desktop=self.run/'desktop'
        try:
            if not (desktop/'exit.json').exists():request(desktop,'stop',timeout=20)
        finally:
            try:self.host.wait(timeout=HOST_EXIT_RECEIPT_SECONDS)
            except subprocess.TimeoutExpired:raise RuntimeError('Private host cleanup did not finish')
        self.case['cleanup']=read(desktop/'exit.json')
        atomic_json(ROOT/'.runtime-tests/coordination-status.json',{'state':'idle','label':self.label,
            'owner_pid':os.getpid(),'game_pid':None,'updated_unix':time.time(),'cleanup':self.case['cleanup'],
            'local_status_only':True,'exclusive_claim_nonce':self.slot.token})
        self.case['completed_unix']=time.time()
        source_log=self.user/'Documents/My Games/Fallout4/F4SE'
        for path in source_log.glob('*.log'):shutil.copy2(path,self.run/path.name)
        self.case['archive_inputs_unchanged']=all(Path(row['source']).stat().st_size==row['bytes'] and
            Path(row['source']).stat().st_mtime_ns==row['mtime_ns'] for row in self.setup['immutable_archive_inputs'])
        self.case['original_user_files_after']=original_user_files()
        self.case['original_user_files_unchanged']=self.case['original_user_files_after']==self.case['original_user_files_before']
        self.save();print('Owned host exited; asset inputs unchanged:',self.case['archive_inputs_unchanged'],flush=True)
        if self.baseline:
            shutil.copy2(ROOT/'build-benchmark/Release/FasterShadows.dll',self.game/'Data/F4SE/Plugins/FasterShadows.dll')
        cleanup=self.case['cleanup'].get('cleanup',{})
        if self.case['cleanup'].get('error') or cleanup.get('remaining_pids') or cleanup.get('cleanup_errors'):
            raise RuntimeError('Owned desktop host reports a cleanup/runtime failure')
        if not self.case['archive_inputs_unchanged'] or not self.case['original_user_files_unchanged']:
            raise RuntimeError('Original files/asset inputs differ; restoration cannot be certified')
        self.slot.restoration_verified=True

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime',choices=['og','ae']);parser.add_argument('label')
    parser.add_argument('--handoff',type=Path,required=True)
    parser.add_argument('--request',type=Path,required=True)
    parser.add_argument('--lab',type=Path,required=True)
    parser.add_argument('--minutes',type=int,default=10)
    parser.add_argument('--mask',type=int,default=14,choices=range(32))
    parser.add_argument('--matrix',action='store_true',help='Paired masks 0 versus each of the five independent techniques')
    parser.add_argument('--smoke',action='store_true');parser.add_argument('--seconds',type=int,default=12)
    parser.add_argument('--yaw',type=int,default=0)
    parser.add_argument('--hour',type=int,default=12)
    parser.add_argument('--timescale',type=game_timescale,default=0,
        help='Game-clock rate: 0 freezes time for matched scenes; 20 checks normal-time cascade reuse')
    parser.add_argument('--shadow-distance',type=shadow_distance,default=0,
        help='fDirShadowDistance for the run, set in the game INI before launch (0 keeps the lab default); e.g. 3000 empties the far cascade')
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--minimum-ram-gib',type=float,default=5,
        help='Recorded starting RAM gate; at least 4 GiB for performance runs')
    parser.add_argument('--reduced-ram-gate',action='store_true',
        help='User-approved start gate down to 3 GiB; recorded in case.json, in-game 1 GiB floor unchanged')
    parser.add_argument('--scene',choices=list(SCENES),default='DiamondCityExt')
    parser.add_argument('--dry-run',action='store_true',
        help='ON windows test and count casters but remove none; recorded in case.json')
    parser.add_argument('--no-diagnostics',action='store_true',
        help='Diagnostics=0, the shipped counter state: timings without counter writes, census reads zero')
    parser.add_argument('--motion-check',action='store_true',
        help='No timing windows: validate and capture settled poses after small view changes, OFF and ON; not a flicker test')
    parser.add_argument('--temporal-check',action='store_true',help='Private consecutive world-frame capture; no performance windows')
    parser.add_argument('--soak',type=int,default=0,metavar='SECONDS',
        help='No timing windows: keep the plugin on while the camera pans for SECONDS (stability/diagnostics)')
    parser.add_argument('--soak-still',action='store_true',help='The soak holds the camera still instead of panning')
    parser.add_argument('--quit-check',type=int,default=0,metavar='SECONDS',
        help='No timing windows: after SECONDS in the scene the game quits through the console; records its exit code')
    parser.add_argument('--camera-motion-check',action='store_true',
        help="No timing windows: the timing windows' camera motion turns, strafes and walks the player and restores it")
    parser.add_argument('--sequence-frames',type=int,choices=range(8,49),default=32)
    parser.add_argument('--sequence-pacing-ms',type=int,choices=(0,33,50),default=33,
        help='Intentional minimum frame duration for quality stress; all capture overhead is recorded')
    parser.add_argument('--sequence-pan-step',type=float,default=.25,help='Degrees per pan command (at most 5)')
    parser.add_argument('--sequence-strafe-step',type=float,default=4,help='Game units per strafe or walk command (at most 32)')
    parser.add_argument('--sequence-motions',type=sequence_motions,default=SEQUENCE_MOTIONS[:3],
        help='Comma list of the temporal sequences to capture: still, pan, strafe, walk (default still,pan,strafe)')
    parser.add_argument('--sequence-settle-seconds',type=int,default=0,help='Seconds at the home pose before the first sequence (at most 60)')
    parser.add_argument('--ini',action='append',default=[],metavar='SECTION.KEY=VALUE',
        help='Override one shipped INI key in the lab copy; recorded in case.json')
    parser.add_argument('--scheduling',choices=list(scheduling.MODES),default='default',
        help='foreground: opt the hidden game out of Windows power throttling, as a real foreground game is; '
             'performance-cores: also keep it on performance cores. Applied identically to native and candidate')
    parser.add_argument('--culling-batch-toggle',action='store_true',
        help="ON windows also enable FO4's batched culling (bCullingBatch); private candidate DLL only")
    args=parser.parse_args()
    if args.dry_run and args.baseline:parser.error('A plugin-absent control has no rules to dry-run')
    if args.no_diagnostics and args.baseline:parser.error('A plugin-absent control has no counters to disable')
    if args.culling_batch_toggle and args.baseline:parser.error('A plugin-absent control cannot toggle culling batches')
    if args.motion_check and (args.baseline or args.smoke or args.matrix):parser.error('The motion check compares the plugin OFF and ON only')
    if args.temporal_check and (args.baseline or args.smoke or args.matrix or args.motion_check):parser.error('Temporal capture runs independently of other modes')
    if args.camera_motion_check and (args.baseline or args.smoke or args.matrix or args.motion_check or args.temporal_check):
        parser.error('The camera motion check runs independently of other modes')
    if args.quit_check and (args.smoke or args.matrix or args.motion_check or args.temporal_check or args.camera_motion_check or args.soak):
        parser.error('The quit check runs independently of other modes')
    if not (0<args.sequence_pan_step<=5 and 0<args.sequence_strafe_step<=32):parser.error('Motion steps must be in (0,5] degrees and (0,32] units')
    lowest=3 if args.reduced_ram_gate else 4
    if not math.isfinite(args.minimum_ram_gib) or args.minimum_ram_gib<lowest:parser.error('Performance RAM gate must be at least %d GiB'%lowest)
    if args.seconds<3 or args.seconds>30:parser.error('Window seconds must be in [3,30]')
    if args.matrix and (args.smoke or args.baseline):parser.error('Matrix needs a private candidate measurement')
    # No waiting or auto retries: one handoff permits exactly this one launch.
    slot=Slot(args.handoff,args.label,request_path=args.request,minutes=args.minutes,
              minimum_ram=2.5 if args.smoke else args.minimum_ram_gib,phase='runtime',game_starts=1,targets=['private_runtime'])
    with slot:
        if args.runtime=='ae' and not any((p.info['name'] or '').lower()=='steam.exe' for p in psutil.process_iter(['name'])):
            raise RuntimeError('Steam must already be running for the AE private lab')
        runner=Runner(args.runtime,args.label,args.smoke,args.baseline,args.minimum_ram_gib,slot=slot,lab=args.lab,mask=args.mask,dry_run=args.dry_run,diagnostics=not args.no_diagnostics,overrides=args.ini,scheduling_mode=args.scheduling,culling_batch=args.culling_batch_toggle,shadow_distance=args.shadow_distance)
        runner.case['reduced_start_ram_gate']=args.reduced_ram_gate
        execute(runner,args)

def execute(runner,args):
    try:
        runner.start();runner.scene(name=args.scene,hour=args.hour,yaw=args.yaw,timescale=args.timescale)
        if getattr(args,'temporal_check',False):
            runner.temporal_check(args.yaw,args.sequence_frames,args.sequence_pacing_ms,
                                  getattr(args,'sequence_pan_step',.25),getattr(args,'sequence_strafe_step',4),
                                  getattr(args,'sequence_settle_seconds',0),
                                  getattr(args,'sequence_motions',SEQUENCE_MOTIONS[:3]))
        elif getattr(args,'camera_motion_check',False):
            runner.camera_motion_check()
        elif getattr(args,'quit_check',0):
            runner.quit_check(args.quit_check)
        elif getattr(args,'soak',0):
            runner.soak(args.soak,getattr(args,'soak_still',False))
        elif args.motion_check:
            runner.motion_check(args.yaw)
        elif args.smoke:
            if args.baseline:
                runner.pause(5);state=runner.command('@status')
            else:
                runner.command('@fps-options '+str(args.mask),'@fps-stats reset','@fps-optimizations reset');runner.pause(5)
                state=runner.command('@fps-stats sample','@fps-optimizations sample','@status')
                runner.case['smoke_counters']=read(runner.probe/'shadow-counters.json')
                runner.case['smoke_optimizations']=read(runner.probe/'optimization-counters.json')
                runner.command('@fps-sun-state')
                runner.case['smoke_sun_state']=read(runner.probe/'sun-state.json')
            runner.case['smoke_state']=state
            runner.save();print('Smoke scene ready; counters:',runner.case.get('smoke_counters'),flush=True)
        else:
            for enabled,mask,tag in measurement_plan(args.matrix,args.baseline,args.mask):
                runner.window(enabled,args.seconds,tag,mask=mask)
    except BaseException as error:
        runner.case['error']=repr(error);runner.save();print('Run failed:',repr(error),flush=True);raise
    finally:
        runner.stop()

if __name__=='__main__':main()
