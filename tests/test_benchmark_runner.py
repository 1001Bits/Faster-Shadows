"""Fixture migration and attribution tests; importing the runner starts nothing."""
import importlib.util
import argparse
import json
import math
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

BENCH = Path(__file__).resolve().parents[1] / 'tools/benchmark'
sys.path.insert(0, str(BENCH))
SPEC = importlib.util.spec_from_file_location('shadow_benchmark', BENCH / 'run.py')
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
import prepare_game


class RunnerTests(unittest.TestCase):
    def test_read_retries_a_status_file_the_host_is_replacing(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'status.json';path.write_text('{"target_pids": [7]}',encoding='utf-8')
            original=Path.read_text
            calls=[]
            def flaky(self,*args,**kwargs):
                calls.append(1)
                if len(calls)<3:raise PermissionError(13,'Permission denied')
                return original(self,*args,**kwargs)
            with patch.object(Path,'read_text',flaky),patch.object(runner.time,'sleep') as sleep:
                self.assertEqual(runner.read(path),{'target_pids':[7]})
            self.assertEqual(len(calls),3);self.assertEqual(sleep.call_count,2)
            with patch.object(Path,'read_text',side_effect=PermissionError(13,'Permission denied')),patch.object(runner.time,'sleep'):
                with self.assertRaises(PermissionError):runner.read(path,attempts=3)

    def test_replace_retries_a_command_file_the_probe_is_reading(self):
        with tempfile.TemporaryDirectory() as folder:
            source,target=Path(folder)/'commands.tmp',Path(folder)/'commands.cmd'
            source.write_text('1\n@status\n')
            original=runner.os.replace;calls=[]
            def flaky(a,b):
                calls.append(1)
                if len(calls)<3:raise PermissionError(5,'Access is denied')
                return original(a,b)
            with patch.object(runner.os,'replace',flaky),patch.object(runner.time,'sleep') as sleep:
                runner.replace(source,target)
            self.assertEqual(target.read_text(),'1\n@status\n');self.assertEqual(sleep.call_count,2)
            source.write_text('2\n')
            with patch.object(runner.os,'replace',side_effect=PermissionError(5,'Access is denied')),patch.object(runner.time,'sleep'):
                with self.assertRaises(PermissionError):runner.replace(source,target,attempts=3)

    @staticmethod
    def private_artifacts(root):
        game=root/'game';(game/'Data/F4SE/Plugins').mkdir(parents=True)
        release=root/'build-benchmark/Release';release.mkdir(parents=True)
        names=('DTLBenchLoader.exe','FPSFolderGuard.dll','DTLBenchFO4.dll','FasterShadows.dll')
        manifest={'binaries':{},'support':{}}
        for name in names:
            path=release/name;path.write_bytes(('new '+name).encode())
            manifest['binaries'][name]=runner.sha(path)
        support=root/'support';guard=support/'private-loader/build/DTLBenchInputGuard.dll'
        guard.parent.mkdir(parents=True);guard.write_bytes(b'new input guard')
        manifest['support']['private-loader/build/DTLBenchInputGuard.dll']=runner.sha(guard)
        return game,support,manifest

    def test_reused_lab_refreshes_actual_loader_guard_and_candidate(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);game,support,manifest=self.private_artifacts(root)
            (game/'DTLBenchLoader.exe').write_bytes(b'old loader')
            (game/'DTLBenchInputGuard.dll').write_bytes(b'old guard')
            with patch.object(runner,'ROOT',root),patch.object(runner,'SUPPORT',support):
                recorded=runner.stage_private_artifacts(game,manifest)
            self.assertEqual(recorded['DTLBenchLoader.exe'],manifest['binaries']['DTLBenchLoader.exe'])
            self.assertEqual(recorded['DTLBenchInputGuard.dll'],manifest['support']['private-loader/build/DTLBenchInputGuard.dll'])
            self.assertEqual(len(recorded),5)
            for name,digest in recorded.items():self.assertEqual(runner.sha(game/name),digest)

    def test_staging_rejects_changed_source_before_replacing_old_loader(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);game,support,manifest=self.private_artifacts(root)
            (game/'DTLBenchLoader.exe').write_bytes(b'old loader')
            (root/'build-benchmark/Release/DTLBenchLoader.exe').write_bytes(b'changed after verification')
            with patch.object(runner,'ROOT',root),patch.object(runner,'SUPPORT',support):
                with self.assertRaisesRegex(RuntimeError,'Mismatched staged private artifact'):
                    runner.stage_private_artifacts(game,manifest)
            self.assertEqual((game/'DTLBenchLoader.exe').read_bytes(),b'old loader')
            self.assertEqual(list(game.glob('*.tmp')),[])

    def test_baseline_refreshes_support_but_removes_only_lab_candidate(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);game,support,manifest=self.private_artifacts(root)
            candidate=game/'Data/F4SE/Plugins/FasterShadows.dll';candidate.write_bytes(b'old candidate')
            with patch.object(runner,'ROOT',root),patch.object(runner,'SUPPORT',support):
                recorded=runner.stage_private_artifacts(game,manifest,baseline=True)
            self.assertFalse(candidate.exists())
            self.assertTrue((root/'build-benchmark/Release/FasterShadows.dll').is_file())
            self.assertEqual(len(recorded),4)

    def test_ini_override_records_effective_dry_run_and_diagnostics(self):
        shipped=(runner.ROOT/'package/FasterShadows.ini').read_text()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'package').mkdir();(root/'package/FasterShadows.ini').write_text(shipped)
            lab=root/'.runtime-tests/labs/test';(lab/'game/Data/F4SE/Plugins').mkdir(parents=True)
            setup={'schema':4,'runtime':'og','plugin_name':'FasterShadows.dll','game':str(lab/'game'),'user':str(lab/'user')}
            (lab/'setup.json').write_text(json.dumps(setup))
            with patch.object(runner,'ROOT',root),patch.object(runner,'sha',return_value='candidate'), \
                    patch.object(runner,'original_user_files',return_value={}):
                instance=runner.Runner('og','override',slot=Mock(),lab=lab,
                    overrides=['General.DryRun=1','General.Diagnostics=0'])
            self.assertTrue(instance.dry_run);self.assertFalse(instance.diagnostics)
            self.assertTrue(instance.case['dry_run']);self.assertFalse(instance.case['diagnostics'])

    def test_ack_sharing_violation_retries_read_without_resending_command(self):
        with tempfile.TemporaryDirectory() as folder:
            instance=object.__new__(runner.Runner)
            instance.probe=Path(folder);instance.sequence=0;instance.host=None;instance.slot=Mock()
            (instance.probe/'1.ack.json').write_text('{}')
            result={'error':'','executed':1,'requested':1,'state':{'present':7}}
            with patch.object(runner,'read',side_effect=[PermissionError(13,'sharing'),result]) as read_ack, \
                    patch.object(runner.time,'sleep'),patch.object(runner.os,'replace',wraps=runner.os.replace) as publish:
                self.assertEqual(instance.command('@status'),{'present':7})
            self.assertEqual(read_ack.call_count,2)
            self.assertEqual(instance.slot.gate.call_count,2)
            self.assertEqual(instance.sequence,1)
            publish.assert_called_once()
            self.assertEqual((instance.probe/'commands.cmd').read_text(),'1\n@status\n')

    def test_ack_read_retry_retains_original_deadline_and_host_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            instance=object.__new__(runner.Runner)
            instance.probe=Path(folder);instance.sequence=0;instance.host=Mock();instance.slot=Mock()
            (instance.probe/'1.ack.json').write_text('{}')
            instance.host.poll.return_value=None
            with patch.object(runner,'read',side_effect=PermissionError(13,'sharing')), \
                    patch.object(runner.time,'sleep'),patch.object(runner.time,'monotonic',side_effect=[0,0,46]):
                with self.assertRaisesRegex(TimeoutError,'Command task timed out'):
                    instance.command('@status')
            self.assertEqual(instance.host.poll.call_count,2)
            instance.slot.gate.assert_called_once()
            instance.sequence=0;instance.host.poll.side_effect=[None,1]
            with patch.object(runner,'read',side_effect=PermissionError(13,'sharing')),patch.object(runner.time,'sleep'):
                with self.assertRaisesRegex(RuntimeError,'Owned host exited'):
                    instance.command('@status')

    def test_temporal_mode_never_opens_performance_windows(self):
        instance=Mock()
        args=SimpleNamespace(scene='DiamondCityExt',hour=12,yaw=180,timescale=0,
                             temporal_check=True,sequence_frames=32,sequence_pacing_ms=33)
        runner.execute(instance,args)
        instance.temporal_check.assert_called_once_with(180,32,33,.25,4,0,('still','pan','strafe'))
        instance.window.assert_not_called()
        instance.stop.assert_called_once()

    @staticmethod
    def pose_state(position=(1,2,3),yaw=180,present=10,**extra):
        return dict(world_ready=True,loading=False,camera_first_person=True,position=list(position),
                    angles_radians=[0,0,math.radians(yaw)],present={'count':present},**extra)

    def test_pose_readback_rejects_old_position_angle_and_invalid_state(self):
        state=self.pose_state()
        self.assertTrue(runner.pose_matches(state,(1,2,3),180))
        self.assertTrue(runner.pose_matches(state,(1,2,3),-180))
        self.assertFalse(runner.pose_matches(state,(41,2,3),180))
        self.assertFalse(runner.pose_matches(state,(1,2,3),181.5))
        self.assertFalse(runner.pose_matches({**state,'loading':True},(1,2,3),180))
        self.assertFalse(runner.pose_matches({**state,'position':[float('nan'),2,3]},(1,2,3),180))
        self.assertFalse(runner.pose_matches({},(1,2,3),180))

    def test_capture_waits_for_deferred_pose_and_completed_image(self):
        instance=object.__new__(runner.Runner)
        instance.pause=Mock()
        old=self.pose_state(yaw=180,present=10)
        settled=self.pose_state(yaw=181.5,present=20,capture={'request_id':7})
        ready={**settled,'present':{'count':22}}
        requested={**ready,'capture':{'request_id':8,'completed_id':7,'state':1}}
        with tempfile.TemporaryDirectory() as folder:
            instance.probe=Path(folder)
            path=instance.probe/'pose.ppm';path.write_bytes(b'P6\n1 1\n255\nabc')
            complete={**ready,'capture':{'request_id':8,'completed_id':8,'state':3,
                       'path':str(path),'width':1,'height':1,'all_zero':False}}
            instance.command=Mock(side_effect=[old,settled,ready,requested,requested,complete])
            result=instance.capture_pose('pose.ppm',(1,2,3),181.5)
        self.assertTrue(result['pose_verified'])
        self.assertEqual(result['capture_request_id'],8)
        self.assertEqual([call.args for call in instance.command.call_args_list],
                         [('@status',),('@status',),('@status',),('@capture pose.ppm',),('@status',),('@status',)])

    def test_pose_timeout_never_requests_an_old_view_capture(self):
        instance=object.__new__(runner.Runner)
        instance.pause=Mock()
        instance.command=Mock(return_value=self.pose_state(yaw=180))
        with patch.object(runner.time,'monotonic',side_effect=[0,0,5,11]):
            with self.assertRaisesRegex(TimeoutError,'pose did not settle'):
                instance.capture_pose('pose.ppm',(1,2,3),181.5)
        self.assertTrue(all(call.args==('@status',) for call in instance.command.call_args_list))

    def test_pose_must_remain_matched_until_capture_completion(self):
        instance=object.__new__(runner.Runner)
        instance.wait_for_pose=Mock(return_value=self.pose_state(capture={'request_id':1}))
        instance.command=Mock(side_effect=[self.pose_state(capture={'request_id':2}),self.pose_state(yaw=181.5)])
        with self.assertRaisesRegex(RuntimeError,'Pose changed'):
            instance.capture_pose('pose.ppm',(1,2,3),180)

    def test_capture_rejects_stale_request_and_invalid_output(self):
        with tempfile.TemporaryDirectory() as folder:
            instance=object.__new__(runner.Runner);instance.probe=Path(folder)
            path=instance.probe/'pose.ppm';path.write_bytes(b'P6\n1 1\n255\nabc')
            instance.wait_for_pose=Mock(return_value=self.pose_state(capture={'request_id':1}))
            instance.command=Mock(return_value=self.pose_state(capture={'request_id':1}))
            with self.assertRaisesRegex(RuntimeError,'fresh request ID'):
                instance.capture_pose('pose.ppm',(1,2,3),180)
            valid={'request_id':2,'completed_id':2,'state':3,'path':str(path),'width':1,'height':1,'all_zero':False}
            for change,error in [({'path':str(instance.probe/'other.ppm')},'unexpected path'),
                                 ({'all_zero':True},'no valid image'),({'state':4,'error':'GPU failure'},'Capture failed')]:
                with self.subTest(change=change):
                    instance.command=Mock(side_effect=[self.pose_state(capture={'request_id':2}),
                                                      self.pose_state(capture={**valid,**change})])
                    with self.assertRaisesRegex(RuntimeError,error):
                        instance.capture_pose('pose.ppm',(1,2,3),180)

    def test_scene_freezes_by_default_and_can_advance_game_time(self):
        state = dict(world_ready=True, main_menu=False, loading=False, camera_first_person=True,
                     cell_id=runner.SCENES['DiamondCityExt'], position=[1, 2, 3])
        for rate in (None, 20):
            instance = object.__new__(runner.Runner)
            instance.baseline = True
            instance.case = {}
            instance.command = Mock(return_value=state)
            instance.pause = Mock()
            instance.save = Mock()
            instance.scene(**({} if rate is None else {'timescale': rate}))
            expected = 0 if rate is None else rate
            self.assertEqual(instance.command.call_args_list[1].args[0], f'set timescale to {expected}')
            self.assertEqual(instance.case['scene_parameters']['timescale'], expected)

    def test_shadow_distance_goes_into_the_game_ini_of_its_run_only(self):
        with tempfile.TemporaryDirectory() as folder:
            user = Path(folder)
            documents = user / 'Documents/My Games/Fallout4'
            documents.mkdir(parents=True)
            prefs = documents / 'Fallout4Prefs.ini'
            prefs.write_text('[Display]\nfDirShadowDistance=14000.0000\niDirShadowSplits=3\n')
            shortened = runner.game_prefs_configuration(user, 3000)
            self.assertEqual(shortened.get('Display', 'fDirShadowDistance'), '3000.0000')
            self.assertEqual(shortened.get('Display', 'iDirShadowSplits'), '3')
            runner.write(prefs, shortened)  # a shortened run leaves its prefs behind ...
            plain = runner.game_prefs_configuration(user)  # ... and the next run starts from the original
            self.assertEqual(plain.get('Display', 'fDirShadowDistance'), '14000.0000')
            self.assertEqual((documents / 'Fallout4Prefs.original.ini').read_text(),
                             '[Display]\nfDirShadowDistance=14000.0000\niDirShadowSplits=3\n')
        self.assertEqual(runner.shadow_distance(3000), 3000)
        for value in (500, 40000, 'far'):
            with self.assertRaises(argparse.ArgumentTypeError):
                runner.shadow_distance(value)
        state = dict(world_ready=True, main_menu=False, loading=False, camera_first_person=True,
                     cell_id=runner.SCENES['DiamondCityExt'], position=[1, 2, 3])
        instance = object.__new__(runner.Runner)
        instance.baseline = True
        instance.case = {}
        instance.shadow_distance = 3000.0
        instance.command = Mock(return_value=state)
        instance.pause = Mock()
        instance.save = Mock()
        instance.scene()
        self.assertFalse(any('setini' in str(call) for call in instance.command.call_args_list))
        self.assertEqual(instance.case['scene_parameters']['shadow_distance'], 3000)

    def test_invalid_clock_rate_is_rejected_before_game_commands(self):
        instance = object.__new__(runner.Runner)
        instance.command = Mock()
        for value in (-1, float('inf'), float('nan'), 'not-a-number'):
            with self.assertRaises(argparse.ArgumentTypeError):
                instance.scene(timescale=value)
        instance.command.assert_not_called()

    def test_execute_passes_clock_rate_to_the_scene(self):
        instance = Mock(baseline=True, case={})
        args = SimpleNamespace(scene='DiamondCityExt', hour=12, yaw=180, timescale=20,
                               motion_check=False, smoke=True, baseline=True)
        runner.execute(instance, args)
        instance.scene.assert_called_once_with(name='DiamondCityExt', hour=12, yaw=180, timescale=20)
        instance.stop.assert_called_once_with()

    def test_sun_observation_requires_no_plugin_in_a_baseline(self):
        instance = object.__new__(runner.Runner)
        instance.baseline = True
        instance.command = Mock()
        self.assertIsNone(instance.sun_state())
        instance.command.assert_not_called()

    def test_sun_observations_stay_outside_the_timing_window(self):
        events = []
        state = dict(cell_id=1, position=[1, 2, 3], camera_state=0)
        counters = dict(sun_tested=0, sun_candidates=0, point_tested=0, point_candidates=0, other=0)
        options = dict(mask=14, enabled=1, dry_run=0, volume_analysis_only=1)
        rt = Mock()
        rt.start.side_effect = lambda *a: events.append('start') or {'window': 1}
        rt.drain.side_effect = lambda *a: events.append('drain') or {}
        rt.validate.return_value = {'valid': True, 'issues': []}
        with tempfile.TemporaryDirectory() as folder:
            instance = object.__new__(runner.Runner)
            instance.run = Path(folder)
            instance.probe = instance.run / 'probe'
            instance.probe.mkdir()
            (instance.probe / 'rendered-throughput.jsonl').write_text('')
            instance.slot = Mock()
            instance.baseline = instance.dry_run = False
            instance.mask = 14
            instance.pid = 1
            instance.case = {}
            instance.results = []
            instance.pause = Mock()
            instance.command = Mock(return_value=state)
            instance.save = Mock()
            instance.sun_state = Mock(side_effect=lambda: events.append('sun') or {'present': 1})
            instance.sampler = Mock(topology=[])
            instance.sampler.prime.side_effect = lambda: events.append('prime')
            data = {'status.json': {'qpc_frequency': 1000}, 'shadow-counters.json': counters,
                    'optimization-counters.json': options}
            with patch.object(runner, 'rt', rt), patch.object(runner, 'qpc', side_effect=[100, 200]), \
                    patch.object(runner, 'read', side_effect=lambda path: data[path.name]), \
                    patch.object(runner.time, 'monotonic', side_effect=[0, 20]), \
                    patch.object(runner.psutil, 'Process'), \
                    patch.object(runner.telemetry, 'gpu_state', side_effect=lambda: events.append('gpu') or {}), \
                    patch.object(runner.telemetry, 'summarize', return_value={'samples': 0}):
                result = instance.window(True, 12, 'on')
        # nvidia-smi runs only outside the throughput window; the sampler is primed inside it.
        self.assertEqual(events, ['sun', 'gpu', 'start', 'prime', 'drain', 'gpu', 'sun'])
        self.assertEqual(result['machine'], {'samples': 0})
        self.assertEqual(result['sun_state_before'], {'present': 1})
        self.assertEqual(result['sun_state_after'], {'present': 1})

    def test_motion_spans_the_measured_interval_and_restores_the_pose_after_it(self):
        events = []
        state = dict(cell_id=1, position=[1, 2, 3], camera_state=0)
        counters = dict(sun_tested=0, sun_candidates=0, point_tested=0, point_candidates=0, other=0)
        options = dict(mask=14, enabled=1, dry_run=0, volume_analysis_only=1)
        rt = Mock()
        rt.start.side_effect = lambda *a: events.append('start') or {'window': 1}
        rt.drain.side_effect = lambda *a: events.append('drain') or {}
        rt.validate.return_value = {'valid': True, 'issues': []}
        def command(*lines):
            events.extend(line for line in lines if line.startswith('@fps-motion'))
            return state
        with tempfile.TemporaryDirectory() as folder:
            instance = object.__new__(runner.Runner)
            instance.run = Path(folder)
            instance.probe = instance.run / 'probe'
            instance.probe.mkdir()
            (instance.probe / 'rendered-throughput.jsonl').write_text('')
            instance.slot = Mock()
            instance.baseline = instance.dry_run = False
            instance.mask = 14
            instance.pid = 1
            instance.case = {}
            instance.results = []
            instance.pause = Mock()
            instance.command = Mock(side_effect=command)
            instance.save = Mock()
            instance.sun_state = Mock(return_value={'present': 1})
            instance.sampler = Mock(topology=[])
            instance.sampler.prime.side_effect = lambda: events.append('prime')
            data = {'status.json': {'qpc_frequency': 1000}, 'shadow-counters.json': counters,
                    'optimization-counters.json': options}
            with patch.object(runner, 'rt', rt), patch.object(runner, 'qpc', side_effect=[100, 200]),                     patch.object(runner, 'read', side_effect=lambda path: data[path.name]),                     patch.object(runner.time, 'monotonic', side_effect=[0, 20]),                     patch.object(runner.psutil, 'Process'),                     patch.object(runner.telemetry, 'gpu_state', return_value={}),                     patch.object(runner.telemetry, 'summarize', return_value={'samples': 0}):
                result = instance.window(True, 12, 'on', motion=(1, 45, 30))
            self.assertEqual(events, ['start', '@fps-motion 1 45 30', 'prime', 'drain', '@fps-motion 0 0 0'])
            self.assertEqual(result['motion'], (1, 45, 30))
            instance.baseline = True
            with self.assertRaisesRegex(ValueError, 'candidate'):
                instance.window(True, 12, 'on', motion=(2, 300, 300))

    def test_camera_motion_check_needs_movement_and_an_exact_return(self):
        home = {'angles_radians': [0, 0, 3.14159], 'position': [100.0, 200.0, 300.0]}
        def statuses(turned, strafed, back):
            turn = dict(home, angles_radians=[0, 0, 3.14159 + turned])
            strafe = dict(home, position=[100.0 + strafed, 200.0, 300.0])
            ended = dict(home, position=[100.0 + back, 200.0, 300.0])
            walk = dict(home, position=[100.0, 200.0 - 40, 300.0])  # heading 180 degrees: forward is -y
            # pan: before, start, moving, stop, after; strafe and walk: the same five
            return iter([home, {}, turn, {}, home, home, {}, strafe, {}, ended, home, {}, walk, {}, home])
        def check(turned, strafed, back):
            instance = object.__new__(runner.Runner)
            instance.baseline = False
            instance.case = {}
            instance.save = Mock()
            instance.pause = Mock()
            replies = statuses(turned, strafed, back)
            instance.command = Mock(side_effect=lambda *lines: next(replies))
            instance.camera_motion_check()
            return instance
        instance = check(0.3, 150, 0)
        self.assertEqual(set(instance.case['camera_motion_check']), {'pan', 'strafe', 'walk'})
        self.assertIn(unittest.mock.call('@fps-motion 1 45 30'), instance.command.call_args_list)
        self.assertIn(unittest.mock.call('@fps-motion 2 300 300'), instance.command.call_args_list)
        self.assertIn(unittest.mock.call('@fps-motion 3 100 300'), instance.command.call_args_list)
        self.assertEqual(instance.case['camera_motion_check']['walk']['moved'], 40)
        with self.assertRaisesRegex(RuntimeError, 'pan did not move'):
            check(0.01, 150, 0)
        with self.assertRaisesRegex(RuntimeError, 'strafe was not put back'):
            check(0.3, 150, 2)
        self.assertAlmostEqual(check(-6.0, 150, 0).case['camera_motion_check']['pan']['moved'], 2 * 3.14159265 - 6.0, 5)  # wraps at 2 pi

    def test_quit_check_records_the_games_own_exit_code(self):
        def check(baseline, log, exits=True):
            with tempfile.TemporaryDirectory() as folder:
                instance = object.__new__(runner.Runner)
                instance.baseline = baseline
                instance.case = {}
                instance.save = Mock()
                instance.pause = Mock()
                instance.command = Mock(return_value={})
                instance.sequence = 7
                instance.probe = Path(folder)
                instance.host = Mock()
                instance.host.poll.return_value = 0 if exits else None
                if log is not None:
                    (instance.probe / 'bench-loader.log').write_text(log)
                clock = iter(range(0, 1000, 10))
                with patch.object(runner.time, 'monotonic', lambda: next(clock)), patch.object(runner.time, 'sleep'):
                    instance.quit_check(60)
                self.assertEqual((instance.probe / 'commands.cmd').read_text(), '8\nqqq\n')  # never waits for an ack
                return instance
        crashed = check(False, 'game resumed\ngame exited code=3221225477\n')
        self.assertEqual(crashed.case['quit_check']['exit_hex'], '0xC0000005')
        self.assertFalse(crashed.case['quit_check']['clean'])
        self.assertIn(unittest.mock.call('@fps 1', '@fps-motion 1 45 30'), crashed.command.call_args_list)
        clean = check(True, 'game exited code=0\n')
        self.assertTrue(clean.case['quit_check']['clean'])
        self.assertEqual(clean.case['quit_check']['plugin'], 'absent')
        clean.command.assert_not_called()  # no plugin: no motion commands
        hung = check(False, None, exits=False)
        self.assertTrue(hung.case['quit_check']['hang'])
        self.assertIsNone(hung.case['quit_check']['exit_code'])
        with self.assertRaisesRegex(RuntimeError, 'no game exit code'):
            check(False, 'game resumed\n')

    def test_light_report_distinguishes_changed_unchanged_and_absent_sun(self):
        import light_report
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            directions = [(None, None), ([0, 0, 1], [0, 0, 1]), ([0, 0, 1], [0.001, 0, 1])]
            windows = []
            for index, (before, after) in enumerate(directions):
                windows.append(dict(tag=str(index), enabled=bool(index), validation={}, counters={},
                                    sun_state_before={'present': int(before is not None), 'direction': before},
                                    sun_state_after={'present': int(after is not None), 'direction': after}))
            (run / 'case.json').write_text(json.dumps({'windows': windows}))
            _, rows, _, _ = light_report.windows(run)
        self.assertNotIn('sun_changed', rows[0])
        self.assertFalse(rows[1]['sun_changed'])
        self.assertTrue(rows[2]['sun_changed'])

    def test_five_independent_abba_comparisons(self):
        plan = runner.measurement_plan(matrix=True)
        self.assertEqual(len(plan), 20)
        self.assertEqual(len({tag for _, _, tag in plan}), 20)
        for i, bit in enumerate([1, 2, 4, 8, 16]):
            pair = plan[i*4:(i+1)*4]
            self.assertEqual([value for _, value, _ in pair], [0, bit, bit, 0])
            self.assertTrue(all(active for active, _, _ in pair))

    def test_plugin_absent_control_never_enables_policy(self):
        self.assertTrue(all(not active for active, _, _ in runner.measurement_plan(baseline=True)))
        with self.assertRaises(ValueError):
            runner.measurement_plan(matrix=True, baseline=True)

    def test_hook_mask_matches_shipped_settings(self):
        shipped = prepare_game.ini((runner.ROOT / 'package/FasterShadows.ini').read_text())
        self.assertEqual(runner.expected_hook_mask(shipped), 0b1010001)  # 1.0 ships the cascade cadence; the caster and rain rules are off
        rules = prepare_game.ini('[SunShadows]\nEnabled=0\n[ActorShadows]\nEnabled=0\n[PointShadows]\nSpotLights=0\n'
                                 '[RainOcclusion]\nEnabled=0\n')
        self.assertEqual(runner.expected_hook_mask(rules), 0b000110)

    def test_rain_only_keeps_sun_update_control_poll_without_sun_filter(self):
        rules=runner.plugin_configuration(overrides=['SunShadows.Enabled=0','ActorShadows.Enabled=0',
            'PointShadows.Enabled=0','RainOcclusion.Enabled=1','SunShadows.CascadeCadence=0','SunShadows.CullOnWorkerThread=0'])
        self.assertEqual(runner.expected_hook_mask(rules),0x21)
        rules.set('RainOcclusion','Enabled','0')
        self.assertEqual(runner.expected_hook_mask(rules),0)
        for section,key in (('SunShadows','CullOnWorkerThread'),('Optimizations','SunGlareNoFlush')):
            rules.set(section,key,'1')
            self.assertEqual(runner.expected_hook_mask(rules),1,key+' keeps only the sun update poll')
            rules.set(section,key,'0')

    def test_dry_run_only_changes_the_dry_run_setting(self):
        normal, dry = runner.plugin_configuration(), runner.plugin_configuration(dry_run=True)
        self.assertEqual((normal['General']['DryRun'], dry['General']['DryRun']), ('0', '1'))
        self.assertEqual(dry['General']['Diagnostics'], '1')
        self.assertEqual({s: dict(normal[s]) for s in normal.sections() if s != 'General'},
                         {s: dict(dry[s]) for s in dry.sections() if s != 'General'})
        self.assertEqual(runner.expected_hook_mask(dry), runner.expected_hook_mask(normal))
        staggered = runner.plugin_configuration(overrides=['SunShadows.CascadeCadence=1', 'SunShadows.Enabled=0',
                                                           'ActorShadows.Enabled=0'])
        self.assertEqual(runner.expected_hook_mask(staggered) & 0b1010001, 0b1010001)
        with self.assertRaises(ValueError):
            runner.plugin_configuration(overrides=['SunShadows.CascadeCadenc=1'])
        quiet = runner.plugin_configuration(diagnostics=False)
        self.assertEqual((quiet['General']['Diagnostics'], quiet['General']['DryRun']), ('0', '0'))

    def test_light_report_pairs_window_lines_in_order(self):
        import json
        import light_report
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            window = lambda tag, on: {'tag': tag, 'enabled': on, 'frame_interval_median_ms': 10.0,
                                      'validation': {'completed_world_renders_per_second': 100.0},
                                      'counters': {'sun_tested': 1, 'sun_candidates': 0, 'point_tested': 2,
                                                   'point_candidates': 1, 'other': 0}}
            (run / 'case.json').write_text(json.dumps({'windows': [window('01-off', False), window('02-on', True)]}))
            (run / 'FasterShadows.log').write_text(
                '[LightCost] interval frames=9 lightLists=9.0000 ms/frame objectLighting=9.0000 ms/frame objectCalls/frame=9.0\n'
                '[Census] window actor candidates=4 rain tested=7 candidates=3 removed=0 sunBlending=0\n'
                '[LightCost] window frames=1200 lightLists=0.1000 ms/frame objectLighting=0.5000 ms/frame objectCalls/frame=800.0\n'
                '[Census] window actor candidates=5 rain tested=8 candidates=2 removed=0 sunBlending=120\n'
                '[LightCost] window frames=1210 lightLists=0.2000 ms/frame objectLighting=0.6000 ms/frame objectCalls/frame=810.5\n')
            _, rows, light, census = light_report.windows(run)
        self.assertEqual((light, census), (2, 2))
        self.assertEqual([(r['tag'], r['lists'], r['objects'], r['actor']) for r in rows],
                         [('01-off', 0.1, 0.5, 4), ('02-on', 0.2, 0.6, 5)])
        self.assertEqual(rows[1]['calls'], 810.5)
        self.assertEqual([r['sun_blending'] for r in rows], [0, 120])

    def test_invalid_masks_rejected(self):
        for mask in [-1, 32, 255]:
            with self.assertRaises(ValueError):
                runner.measurement_plan(mask=mask)

    def test_legacy_lab_cannot_be_used(self):
        with self.assertRaisesRegex(RuntimeError, 'fresh versioned lab'):
            runner.Runner('og', 'unused', slot=Mock(), lab=runner.ROOT / '.runtime-tests/og')

    def test_duplicate_legacy_dll_rejected_before_candidate_read_or_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lab = root / '.runtime-tests/labs/new'
            plugins = lab / 'game/Data/F4SE/Plugins'
            plugins.mkdir(parents=True)
            (plugins / 'FalloutPerformanceSolution.dll').write_bytes(b'old')
            setup = {'schema': 4, 'runtime': 'og', 'plugin_name': 'FasterShadows.dll',
                     'game': str(lab / 'game'), 'user': str(lab / 'user')}
            with patch.object(runner, 'ROOT', root), patch.object(runner, 'read', return_value=setup), \
                    patch.object(runner, 'sha') as sha:
                with self.assertRaisesRegex(RuntimeError, 'duplicate hooks'):
                    runner.Runner('og', 'unused', slot=Mock(), lab=lab)
                sha.assert_not_called()
            self.assertEqual((plugins / 'FalloutPerformanceSolution.dll').read_bytes(), b'old')

    def test_external_game_path_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lab = root / '.runtime-tests/labs/new'
            setup = {'schema': 4, 'runtime': 'og', 'plugin_name': 'FasterShadows.dll',
                     'game': str(root / 'outside'), 'user': str(lab / 'user')}
            with patch.object(runner, 'ROOT', root), patch.object(runner, 'read', return_value=setup):
                with self.assertRaisesRegex(RuntimeError, 'private workspace'):
                    runner.Runner('og', 'unused', slot=Mock(), lab=lab)

    def test_baseline_init_allows_absent_candidate_in_fresh_lab(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lab = root / '.runtime-tests/labs/new'
            (lab / 'game/Data/F4SE/Plugins').mkdir(parents=True)
            setup = {'schema': 4, 'runtime': 'og', 'plugin_name': 'FasterShadows.dll',
                     'game': str(lab / 'game'), 'user': str(lab / 'user')}
            with patch.object(runner, 'ROOT', root), patch.object(runner, 'read', return_value=setup), \
                    patch.object(runner, 'sha', return_value='compiled-hash'), \
                    patch.object(runner, 'original_user_files', return_value={}):
                result = runner.Runner('og', 'fresh', baseline=True, slot=Mock(), lab=lab)
                self.assertEqual(result.case['candidate_sha256'], 'compiled-hash')
                self.assertTrue(result.case['plugin_absent_baseline'])

    def test_preparer_refuses_existing_partial_lab(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);lab=root/'.runtime-tests/labs/existing'
            lab.mkdir(parents=True)
            sentinel=lab/'sentinel';sentinel.write_text('untouched')
            with patch.object(prepare_game,'ROOT',root):
                with self.assertRaisesRegex(RuntimeError,'never resumed'):
                    prepare_game.prepare('og','existing')
            self.assertEqual(sentinel.read_text(),'untouched')

    def test_preparer_refuses_traversal_name(self):
        with self.assertRaises(ValueError):
            prepare_game.prepare('og','../foreign')

    def test_changed_support_is_refused_before_import(self):
        with tempfile.TemporaryDirectory() as temp:
            support=Path(temp);(support/'private_desktop.py').write_text('raise RuntimeError("must not execute")')
            with patch.object(runner,'SUPPORT',support):
                with self.assertRaisesRegex(RuntimeError,'support inputs differ'):
                    runner.load_support({'support':{}})


if __name__ == '__main__':
    unittest.main()
