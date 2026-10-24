"""Summarize complete, validated private-render comparisons without FPS claims."""
from __future__ import annotations
import argparse
import configparser
import itertools
import json
import math
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[2]
METRIC='completed_offscreen_world_renders_per_second'

def percentile(values,p):
    values=sorted(values);position=(len(values)-1)*p
    lower=int(position);upper=min(lower+1,len(values)-1)
    return values[lower]+(values[upper]-values[lower])*(position-lower)

def effective_dry_run(case):
    """INI overrides/readbacks outrank the historical CLI-only case field."""
    configured=None
    if case.get('plugin_settings'):
        configuration=configparser.ConfigParser(interpolation=None,strict=False)
        configuration.read_string(case['plugin_settings'])
        configured=configuration.getboolean('General','DryRun',fallback=None)
    readings=[w['optimization_counters']['dry_run'] for w in case.get('windows',[])
              if isinstance(w.get('optimization_counters'),dict) and 'dry_run' in w['optimization_counters']]
    if readings:
        if any(type(v) not in (int,bool) or v not in (0,1) for v in readings) or len(set(readings))!=1:
            raise ValueError('Inconsistent dry-run readbacks')
        actual=bool(readings[0])
        if configured is not None and configured!=actual:raise ValueError('Dry-run INI/readback mismatch')
        return actual
    if configured is not None:return configured
    value=case.get('dry_run')
    return value if type(value) is bool else None

def analyze(path):
    case=json.loads(path.read_text(encoding='utf-8-sig'))
    windows=case.get('windows',[]);issues=[]
    try:dry_run=effective_dry_run(case)
    except (ValueError,configparser.Error) as error:
        dry_run=None;issues.append('unverified_dry_run: '+str(error))
    if case.get('error'):issues.append('run_error: '+case['error'])
    if case.get('smoke_only'):issues.append('smoke_only')
    expected=4 if case.get('plugin_absent_baseline') else 8
    if len(windows)!=expected:issues.append('incomplete_window_order')
    if not all(w.get('validation',{}).get('valid') is True for w in windows):issues.append('invalid_window')
    if not case.get('archive_inputs_unchanged'):issues.append('source_assets_not_verified_unchanged')
    if not case.get('original_user_files_unchanged'):issues.append('original_user_files_changed_or_not_verified')
    cleanup=case.get('cleanup',{}).get('cleanup',{})
    if not cleanup or cleanup.get('remaining_pids') or cleanup.get('cleanup_errors'):
        issues.append('cleanup_not_verified')
    for w in windows:
        before,after=w['state_before'],w['state_after']
        if any(before[k]!=after[k] for k in ['cell_id','position','camera_state']):issues.append('scene_changed')
    summary={'label':case['label'],'runtime':case['runtime'],'case':str(path),
        'candidate_sha256':case['candidate_sha256'],'baseline':case.get('plugin_absent_baseline',False),
        'dry_run':dry_run,
        'metric':METRIC,'valid':not issues,'issues':sorted(set(issues)),
        'scene':case.get('scene_origin'),'scene_parameters':case.get('scene_parameters'),'windows':[]}
    if issues:return summary
    expected_order=[False]*4 if summary['baseline'] else [False,True,True,False,True,False,False,True]
    if [w['enabled'] for w in windows]!=expected_order:
        summary.update(valid=False,issues=['wrong_window_order']);return summary
    for w in windows:
        summary['windows'].append({
            'tag':w['tag'],'enabled':w['enabled'],'renders_per_second':w['validation']['completed_world_renders_per_second'],
            'cpu_world_mean_ms':w['cpu_world_mean_ms'],'frame_interval_p95_ms':w['frame_interval_p95_ms'],
            'frames':w['frames'],'mean_ia_primitives':w['mean_ia_primitives'],
            'mean_vs_invocations':w['mean_vs_invocations'],'counters':w['counters'],
            'minimum_available_ram_gib':min(r['available_ram_gib'] for r in w['resources']),
            'maximum_game_working_set_mib':max(r['game_working_set_mib'] for r in w['resources'])})
    groups={str(flag):[w for w in summary['windows'] if w['enabled']==flag] for flag in [False,True]}
    summary['groups']={key:{field:statistics.mean(w[field] for w in group)
        for field in ['renders_per_second','cpu_world_mean_ms','frame_interval_p95_ms','mean_ia_primitives','mean_vs_invocations']}
        for key,group in groups.items() if group}
    if not summary['baseline']:
        pairs=[]
        for a,b in zip(summary['windows'][::2],summary['windows'][1::2]):
            off,on=(b,a) if a['enabled'] else (a,b)
            pairs.append(math.log(on['renders_per_second']/off['renders_per_second']))
        boot=[statistics.mean(draw) for draw in itertools.product(pairs,repeat=len(pairs))]
        summary['paired_render_gain_percent']=(math.exp(statistics.mean(pairs))-1)*100
        summary['paired_bootstrap_interval_percent']=[(math.exp(percentile(boot,p))-1)*100 for p in [.025,.975]]
        summary['pair_gains_percent']=[(math.exp(p)-1)*100 for p in pairs]
        summary['inference_scope']='Four adjacent window pairs in one fixed-view run; not independent scene/runtime replications.'
        summary['sun_tested']=sum(w['counters']['sun_tested'] for w in summary['windows'] if w['enabled'])
        summary['sun_candidates']=sum(w['counters']['sun_candidates'] for w in summary['windows'] if w['enabled'])
        summary['point_tested']=sum(w['counters']['point_tested'] for w in summary['windows'] if w['enabled'])
        summary['point_candidates']=sum(w['counters']['point_candidates'] for w in summary['windows'] if w['enabled'])
        # These eligibility counters omit actors, rain and cascade reuse. They prove
        # removal only with dry-run disabled; zero cannot prove no shadows were skipped.
        policy_removed=(summary['sun_candidates']+summary['point_candidates']) if dry_run is False else 0
        point_removed=sum((w.get('optimization_counters') or {}).get('point_removed',0)
                          for w in windows if w['enabled'])
        summary['point_removals_observed']=point_removed if dry_run is not True else 0
        summary['runtime_coverage']={'non_skinned_sun_policy_tests_observed':summary['sun_tested']>0,
            'non_skinned_point_policy_tests_observed':summary['point_tested']>0,
            'any_caster_policy_submissions_removed':False if dry_run is True else (True if policy_removed+point_removed>0 else None)}
    return summary

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('labels',nargs='+');parser.add_argument('--output',default='evidence/performance-results.json')
    args=parser.parse_args()
    results=[analyze(ROOT/'.runtime-tests/runs'/label/'case.json') for label in args.labels]
    native_comparisons=[]
    for result in results:
        if not result['valid'] or result['baseline']:continue
        for baseline in results:
            if not baseline['valid'] or not baseline['baseline'] or baseline['runtime']!=result['runtime']:continue
            if baseline['scene_parameters']!=result['scene_parameters']:continue
            fields=['cell_id','position','camera_state']
            if any(baseline['scene'][key]!=result['scene'][key] for key in fields):continue
            native=baseline['groups']['False']['renders_per_second']
            native_comparisons.append({'candidate':result['label'],'baseline':baseline['label'],
                'enabled_vs_native_percent':100*(result['groups']['True']['renders_per_second']/native-1),
                'disabled_vs_native_percent':100*(result['groups']['False']['renders_per_second']/native-1),
                'inference_scope':'Separate game runs; startup/cache/system drift can confound this comparison.'})
    report={'metric':METRIC,'displayed_fps_valid':False,'results':results,'native_comparisons':native_comparisons}
    output=ROOT/args.output;output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
