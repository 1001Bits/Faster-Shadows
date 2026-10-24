"""Prepare a local vanilla-asset game with private Documents/AppData folders."""
import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SOURCES={'og':Path(r'C:\Games\Fallout.4 1.10.163'),
         'ae':Path(r'C:\Games\Steam\steamapps\common\Fallout 4')}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def ini(text=''):
    config=configparser.ConfigParser(interpolation=None,strict=False)
    config.optionxform=str
    config.read_string(text)
    return config

def values(config,section,**fields):
    if not config.has_section(section):config.add_section(section)
    for key,value in fields.items():config.set(section,key,str(value))

def write(path,config):
    with path.open('w',encoding='utf-8') as out:config.write(out,space_around_delimiters=False)

def prepare(runtime,lab_name,masters_from=None):
    source=SOURCES[runtime]
    if not re.fullmatch(r'[A-Za-z0-9_-]+',lab_name):raise ValueError('Simple fresh lab name required')
    lab=ROOT/'.runtime-tests/labs'/lab_name
    if not lab.resolve().is_relative_to(ROOT.resolve()) or lab.is_symlink() or lab.exists():
        raise RuntimeError('A new workspace-owned lab is required; existing or redirected labs are never resumed')
    if masters_from is not None:
        # Hard links to verified master copies of an earlier workspace lab cost no disk.
        masters_from=Path(masters_from).resolve()
        if not masters_from.is_relative_to((ROOT/'.runtime-tests').resolve()) or not (masters_from/'game/Data').is_dir():
            raise RuntimeError('Masters must come from a workspace lab')
    if shutil.disk_usage(ROOT).free<(2 if masters_from else 5)*1024**3:
        raise RuntimeError('Less than %d GiB free disk' % (2 if masters_from else 5))
    game=lab/'game';plugins=game/'Data/F4SE/Plugins'
    plugins.mkdir(parents=True,exist_ok=False)
    allowed={'Fallout4.exe','bink2w64.dll','d3dcompiler_46e.dll','steam_api64.dll',
        'steam_emu.ini','Galaxy64.dll','fmt.dll','spdlog.dll','cudart64_75.dll',
        'flexExtRelease_x64.dll','flexRelease_x64.dll','GFSDK_GodraysLib.x64.dll',
        'GFSDK_SSAO_D3D11.win64.dll','msvcp110.dll','msvcr110.dll','nvToolsExt64_1.dll'}
    binaries=[]
    for file in source.iterdir():
        if file.is_file() and (file.name in allowed or file.name.startswith('f4se_') and file.suffix=='.dll'):
            target=game/file.name;shutil.copy2(file,target)
            binaries.append({'name':file.name,'sha256':sha(target)})
    archives=[]
    inputs=list((source/'Data').iterdir())+list((source/'Data/Video').glob('*.bk2'))
    for file in sorted(inputs):
        if file.is_file() and (file.parent.name=='Video' or
                file.name.startswith(('Fallout4','DLC')) and file.suffix.lower() in {'.esm','.ba2','.csg','.cdx'}):
            # File symlinks expose read-only asset inputs without changing the
            # source file's hardlink metadata. Never edit linked assets.
            target=game/'Data'/file.relative_to(source/'Data')
            target.parent.mkdir(parents=True,exist_ok=True)
            if file.suffix.lower()=='.esm':
                # Native master discovery excluded file symlinks in the live
                # OG control. Copy ESMs (or hard-link verified workspace copies,
                # which are regular files); archive/video reads accept file links.
                temporary=target.with_name(target.name+'.prepare-tmp')
                if temporary.exists() or temporary.is_symlink():raise RuntimeError('Unexpected master temporary')
                reuse=masters_from/'game/Data'/file.name if masters_from else None
                if reuse is not None and reuse.is_file() and not reuse.is_symlink():
                    os.link(reuse,temporary)
                else:
                    shutil.copy2(file,temporary)
                if sha(temporary)!=sha(file):raise RuntimeError('Master copy verification failed')
                os.replace(temporary,target)
            elif target.is_symlink():
                if target.resolve()!=file.resolve():raise RuntimeError('Unexpected asset link')
            else:
                os.symlink(file,target)
            stat=file.stat()
            archives.append({'source':str(file),'target':str(target),'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns})
    for file in (source/'Data/F4SE/Plugins').iterdir():
        if file.is_file() and file.name.startswith(('version-','versionlib-')) and file.suffix in {'.bin','.csv'}:
            shutil.copy2(file,plugins/file.name)
    user=lab/'user'
    for folder in ['Documents/My Games/Fallout4/F4SE','Documents/My Games/Fallout4/Saves',
                   'AppData/Local/Fallout4','AppData/Roaming','Saved Games','Temp']:
        (user/folder).mkdir(parents=True,exist_ok=False)
    defaults=(source/'Fallout4_Default.ini').read_text(errors='replace').replace('sLanguage=RUSSIAN','sLanguage=ENGLISH').replace('Voices_ru0','Voices_en0').replace('FontConfig_ru','FontConfig')
    (game/'Fallout4_Default.ini').write_text(defaults,encoding='utf-8')
    configuration=ini(defaults)
    values(configuration,'General',sLanguage='en',bAlwaysActive=1,sIntroSequence='',bSkipSplash=1,sLocalSavePath='Saves\\')
    values(configuration,'Archive',bInvalidateOlderFiles=1,sResourceDataDirsFinal='')
    values(configuration,'Audio',bEnableAudio=0)
    values(configuration,'Camera',bDisableAutoVanityMode=1,bForceAutoVanityMode=0)
    values(configuration,'Display',iPresentInterval=0)
    documents=user/'Documents/My Games/Fallout4'
    write(documents/'Fallout4.ini',configuration)
    write(documents/'Fallout4Custom.ini',configuration)
    prefs=ini((source/'High.ini').read_text(errors='replace'))
    values(prefs,'Display',**{'bFull Screen':0,'bBorderless':1,'iSize W':1920,'iSize H':1080,
                            'iPresentInterval':0,'iTexMipMapSkip':0,'iMaxAnisotropy':16})
    values(prefs,'MAIN',bGamepadEnable=0,bSaveOnPause=0,bSaveOnTravel=0,bSaveOnWait=0,bSaveOnRest=0)
    values(prefs,'Bethesda.net',bEnablePlatform=0)
    values(prefs,'AudioMenu',fAudioMasterVolume=0)
    write(documents/'Fallout4Prefs.ini',prefs)
    masters=['Fallout4.esm']+[name for name in ['DLCRobot.esm','DLCworkshop01.esm','DLCCoast.esm',
        'DLCworkshop02.esm','DLCworkshop03.esm','DLCNukaWorld.esm'] if (game/'Data'/name).exists()]
    (user/'AppData/Local/Fallout4/plugins.txt').write_text(''.join('*'+name+'\n' for name in masters))
    (user/'AppData/Local/Fallout4/loadorder.txt').write_text('\n'.join(masters)+'\n')
    (game/'Fallout4.ccc').write_text('')
    (game/'steam_appid.txt').write_text('377160\n')
    for name in ['DTLBenchLoader.exe','FPSFolderGuard.dll']:
        shutil.copy2(ROOT/'build-benchmark/Release'/name,game/name)
    shutil.copy2(ROOT/'.runtime-tests/support/private-loader/build/DTLBenchInputGuard.dll',game/'DTLBenchInputGuard.dll')
    shutil.copy2(ROOT/'build-benchmark/Release/DTLBenchFO4.dll',plugins/'DTLBenchFO4.dll')
    shutil.copy2(ROOT/'build-benchmark/Release/FasterShadows.dll',plugins/'FasterShadows.dll')
    settings=ini((ROOT/'package/FasterShadows.ini').read_text())
    values(settings,'General',DryRun=0,ToggleKey=0,ReportSeconds=600)
    write(plugins/'FasterShadows.ini',settings)
    setup={'schema':4,'plugin_name':'FasterShadows.dll','runtime':runtime,'source':str(source),'game':str(game),'user':str(user),
        'graphics':'1920x1080 High preset, native TAA, vsync off, no frame generation, vanilla assets',
        'executable_sha256':sha(game/'Fallout4.exe'),'plugin_sha256':sha(plugins/'FasterShadows.dll'),
        'binaries':binaries,'immutable_archive_inputs':archives,'masters':masters,
        'folder_guard':'Process-local Documents/AppData redirection before extender/game entry'}
    (lab/'setup.json').write_text(json.dumps(setup,indent=2)+'\n')
    print(json.dumps({'runtime':runtime,'archives':len(archives),'game':str(game)},indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('runtime',choices=SOURCES)
    parser.add_argument('--lab-name',required=True,help='Fresh isolated lab; historical labs are never migrated in place')
    parser.add_argument('--masters-from',type=Path,help='Hard-link SHA-verified ESM copies from an earlier workspace lab')
    args=parser.parse_args();prepare(args.runtime,args.lab_name,args.masters_from)
