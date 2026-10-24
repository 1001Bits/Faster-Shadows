"""Offline evidence attribution: controls, completed launches and distinct interventions."""
import copy
import importlib.util
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


report=load('benchmark_report_tests',ROOT/'tools/benchmark/report.py')
anatomy=load('frame_report_tests',ROOT/'.runtime-tests/fs030-frame-report.py')


def case_fixture():
    state={'cell_id':1,'position':[0,0,0],'camera_state':0}
    windows=[]
    for index,on in enumerate((False,True,True,False,True,False,False,True),1):
        windows.append({'tag':str(index),'enabled':on,'state_before':state,'state_after':state,
            'validation':{'valid':True,'completed_world_renders_per_second':100.},
            'cpu_world_mean_ms':7.,'frame_interval_p95_ms':11.,'frames':1200,
            'mean_ia_primitives':100.,'mean_vs_invocations':100.,
            'resources':[{'available_ram_gib':4.,'game_working_set_mib':2000.}],
            'counters':{'sun_tested':10,'sun_candidates':10,'point_tested':5,'point_candidates':5},
            'optimization_counters':{'dry_run':0,'point_removed':0}})
    return {'label':'test','runtime':'og','candidate_sha256':'candidate','dry_run':False,'windows':windows,
            'archive_inputs_unchanged':True,'original_user_files_unchanged':True,
            'cleanup':{'cleanup':{'remaining_pids':[],'cleanup_errors':[]}}}


class BenchmarkReportTests(unittest.TestCase):
    def analyze(self,case):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'case.json';path.write_text(json.dumps(case))
            return report.analyze(path)

    def test_dry_run_candidates_never_become_removed_shadows(self):
        case=case_fixture();case['dry_run']=True
        for window in case['windows']:window['optimization_counters']['dry_run']=1
        result=self.analyze(case)
        self.assertTrue(result['valid']);self.assertEqual(result['sun_candidates'],40)
        self.assertEqual(result['point_candidates'],20)
        self.assertNotIn('sun_skipped',result);self.assertNotIn('point_skipped',result)
        self.assertFalse(result['runtime_coverage']['any_caster_policy_submissions_removed'])

    def test_ini_override_outranks_historical_cli_field(self):
        case=case_fixture();case['plugin_settings']='[General]\nDryRun=1\n'
        for window in case['windows']:window['optimization_counters']['dry_run']=1
        result=self.analyze(case)
        self.assertTrue(result['dry_run']);self.assertTrue(result['valid'])
        self.assertFalse(result['runtime_coverage']['any_caster_policy_submissions_removed'])
        for window in case['windows']:del window['optimization_counters']
        self.assertTrue(self.analyze(case)['dry_run'])

    def test_inconsistent_readback_is_excluded(self):
        case=case_fixture();case['plugin_settings']='[General]\nDryRun=1\n'
        result=self.analyze(case)
        self.assertFalse(result['valid'])
        self.assertIn('unverified_dry_run',result['issues'][0])

    def test_dry_run_with_cadence_only_claims_caster_filters_are_inert(self):
        case=case_fixture()
        case['plugin_settings']='[General]\nDryRun=1\n[SunShadows]\nCascadeCadence=1\n'
        for window in case['windows']:window['optimization_counters']['dry_run']=1
        result=self.analyze(case)
        self.assertTrue(result['valid']);self.assertTrue(result['dry_run'])
        coverage=result['runtime_coverage']
        self.assertFalse(coverage['any_caster_policy_submissions_removed'])
        self.assertNotIn('any_shadow_submissions_skipped',coverage)

    def test_real_policy_removal_and_unknown_counter_coverage(self):
        case=case_fixture()
        self.assertTrue(self.analyze(case)['runtime_coverage']['any_caster_policy_submissions_removed'])
        for window in case['windows']:
            window['counters']['sun_candidates']=window['counters']['point_candidates']=0
        self.assertIsNone(self.analyze(case)['runtime_coverage']['any_caster_policy_submissions_removed'])
        case['windows'][1]['optimization_counters']['point_removed']=3
        result=self.analyze(case)
        self.assertEqual(result['point_removals_observed'],3)
        self.assertTrue(result['runtime_coverage']['any_caster_policy_submissions_removed'])


class AnatomyReportTests(unittest.TestCase):
    def write_case(self,root,name,mode='cadence',off=10.,on=8.):
        run=root/name;run.mkdir()
        case=case_fixture()
        case['frame_anatomy']={'mode':mode,'windows':12,'seconds':12,
                              'order':['on' if value else 'off' for value in anatomy.ORDER]}
        original=case['windows'][0];case['windows']=[];lines=[]
        for index,state in enumerate(anatomy.ORDER,1):
            window=copy.deepcopy(original);window.update(tag=f'{index:02d}-'+('on' if state else 'off'),enabled=state)
            case['windows'].append(window)
            fields={'loop':on if state else off,**{key:.1 for key in anatomy.ONIDLE}}
            lines.append('[FrameCost] window frames=1200 '+' '.join(f'{key}={value}' for key,value in fields.items()))
        (run/'case.json').write_text(json.dumps(case))
        (run/'FasterShadows.log').write_text('\n'.join(lines))
        return run,case

    def test_three_interventions_keep_separate_medians_and_pairs(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            self.write_case(root,'z-cadence','cadence',10,8)
            self.write_case(root,'a-batch','culling-batch',100,110)
            self.write_case(root,'m-no-skip','cadence-no-skip',20,19)
            with redirect_stdout(io.StringIO()):
                result=anatomy.main([str(root/'*')])
            self.assertEqual([r['run'] for r in result['runs']],['a-batch','m-no-skip','z-cadence'])
            medians={r['mode']:(r['medians']['off']['loop'],r['medians']['on']['loop']) for r in result['runs']}
            self.assertEqual(medians,{'cadence':(10,8),'culling-batch':(100,110),'cadence-no-skip':(20,19)})
            self.assertNotIn('medians',result)
            for run in result['runs']:self.assertEqual(len(run['adjacent_pairs']),6)
            self.assertAlmostEqual(result['runs'][0]['adjacent_pairs'][0]['loop_reduction_percent'],-10)
            self.assertEqual(len(anatomy.expand_runs([str(root/'*'),str(root/'a-batch')])),3)
            with self.assertRaisesRegex(ValueError,'No run folders match'):anatomy.expand_runs([str(root/'missing-*')])

    def test_failed_incomplete_invalid_or_unclean_runs_are_excluded(self):
        changes={
            'failed':lambda c:c.update(error='host exited'),
            'incomplete':lambda c:c['windows'].pop(),
            'order':lambda c:c['windows'][0].update(enabled=True),
            'invalid':lambda c:c['windows'][0]['validation'].update(valid=False),
            'unclean':lambda c:c['cleanup']['cleanup'].update(remaining_pids=[123]),
            'changed-files':lambda c:c.update(original_user_files_unchanged=False),
        }
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            for name,change in changes.items():
                run,case=self.write_case(root,name);change(case)
                (run/'case.json').write_text(json.dumps(case))
            good,_=self.write_case(root,'valid')
            with redirect_stdout(io.StringIO()):result=anatomy.main([str(root/'*')])
            self.assertEqual([r['run'] for r in result['runs'] if r['valid']],[good.name])
            self.assertTrue(all('medians' not in r for r in result['runs'] if not r['valid']))

    def test_missing_timer_line_cannot_shift_window_attribution(self):
        with tempfile.TemporaryDirectory() as folder:
            run,_=self.write_case(Path(folder),'case')
            log=run/'FasterShadows.log';log.write_text('\n'.join(log.read_text().splitlines()[1:]))
            with self.assertRaisesRegex(ValueError,'FrameCost lines do not match'):anatomy.windows(run)


if __name__=='__main__':unittest.main()
