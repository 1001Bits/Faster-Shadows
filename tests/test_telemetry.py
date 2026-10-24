"""Machine telemetry, scheduling controls and the probe placement patch; nothing here starts a game."""
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

BENCH = Path(__file__).resolve().parents[1] / 'tools/benchmark'
sys.path.insert(0, str(BENCH))
import prepare_support
import scheduling
import telemetry

TOPOLOGY = [{'cpu_set_id': 256 + i, 'group': 0, 'logical': i, 'core': i, 'efficiency_class': 1 if i < 2 else 0}
            for i in range(4)]


def sampler(snapshots, own=()):
    instance = object.__new__(telemetry.Sampler)
    instance.own_pids = lambda: set(own)
    instance.topology = TOPOLOGY
    instance.processes = Mock(snapshot=Mock(side_effect=snapshots))
    instance.query, instance.performance, instance.nominal_mhz = None, {}, 2000.0
    return instance


class SamplerTests(unittest.TestCase):
    def test_foreign_cpu_excludes_idle_owned_game_and_reused_pids(self):
        old = {0: ('Idle', 0), 10: ('Fallout4.exe', 0), 20: ('python.exe', 0), 30: ('cl.exe', 0),
               40: ('old.exe', 0), 50: ('MsMpEng.exe', 0)}
        new = {0: ('Idle', 9_000_000), 10: ('Fallout4.exe', 15_000_000), 20: ('python.exe', 5_000_000),
               30: ('cl.exe', 2_500_000), 40: ('reused.exe', 5_000_000), 50: ('MsMpEng.exe', 5_000_000),
               60: ('new.exe', 5_000_000)}
        instance = sampler([old, new], own={20})
        with patch.object(telemetry.win32pdh, 'CollectQueryData'), \
                patch.object(telemetry.psutil, 'cpu_percent', return_value=[50.0] * 4), \
                patch.object(telemetry.time, 'perf_counter', side_effect=[0.0, 0.5]):
            instance.prime()
            row = instance.sample(10)
        self.assertAlmostEqual(row['game_cores'], 3.0)
        self.assertAlmostEqual(row['foreign_cores'], 1.5)  # cl.exe 0.5 + MsMpEng 1.0
        self.assertEqual([r['name'] for r in row['foreign_top']], ['MsMpEng.exe', 'cl.exe'])

    def test_transient_pdh_error_drops_that_core_clock_only(self):
        instance = sampler([{}, {}])
        instance.performance = {0: 'core0', 1: 'core1'}

        def value(counter, _format):
            if counter == 'core1':
                raise telemetry.pywintypes.error(-2147481642, 'GetFormattedCounterValue', 'negative denominator')
            return 0, 110.0
        with patch.object(telemetry.win32pdh, 'CollectQueryData'), \
                patch.object(telemetry.win32pdh, 'GetFormattedCounterValue', side_effect=value), \
                patch.object(telemetry.psutil, 'cpu_percent', return_value=[50.0] * 4), \
                patch.object(telemetry.time, 'perf_counter', side_effect=[0.0, 0.5]):
            instance.prime()
            row = instance.sample(10)
        self.assertEqual(row['effective_mhz'], [2200.0, None, None, None])
        summary = telemetry.summarize([{**row, 'foreign_top': []}], TOPOLOGY)
        self.assertAlmostEqual(summary['performance_core_mhz'], 2200.0)  # the missing clock carries no weight
        self.assertIsNone(summary['efficiency_core_mhz'])

    def test_summary_weights_clocks_by_busy_time_and_splits_core_classes(self):
        samples = [{'busy_percent': [100, 0, 50, 50], 'effective_mhz': [4000, 1000, 3000, 3000],
                    'game_cores': 3.0, 'foreign_cores': 0.25,
                    'foreign_top': [{'name': 'MsMpEng.exe', 'pid': 5, 'cores': 0.25}]},
                   {'busy_percent': [50, 50, 0, 0], 'effective_mhz': [2000, 2000, 1000, 1000],
                    'game_cores': 2.0, 'foreign_cores': 0.75,
                    'foreign_top': [{'name': 'MsMpEng.exe', 'pid': 5, 'cores': 0.75}]}]
        summary = telemetry.summarize(samples, TOPOLOGY)
        self.assertAlmostEqual(summary['performance_core_mhz'], (100 * 4000 + 50 * 2000 + 50 * 2000) / 200)
        self.assertAlmostEqual(summary['efficiency_core_mhz'], 3000)  # idle samples carry no weight
        self.assertAlmostEqual(summary['foreign_cores_mean'], 0.5)
        self.assertEqual(summary['foreign_cores_max'], 0.75)
        self.assertEqual(summary['foreign_top'], [{'name': 'MsMpEng.exe', 'cores_mean': 0.5}])
        self.assertIsNone(telemetry.summarize([], TOPOLOGY))

    def test_gpu_state_parses_clock_event_reasons_and_tolerates_missing_values(self):
        output = '87, 1605, 7001, 34.68, 41, P3, 0x0000000000000020\n'
        with patch.object(telemetry.subprocess, 'run', return_value=Mock(returncode=0, stdout=output)):
            state = telemetry.gpu_state()
        self.assertEqual(state['temperature.gpu'], 87.0)
        self.assertTrue(state['thermal_slowdown'])
        self.assertFalse(state['power_capped'])
        with patch.object(telemetry.subprocess, 'run', return_value=Mock(returncode=0, stdout='[N/A], 1, 1, 1, 1, P8, [N/A]\n')):
            state = telemetry.gpu_state()
        self.assertIsNone(state['temperature.gpu'])
        self.assertIsNone(state['thermal_slowdown'])
        with patch.object(telemetry.subprocess, 'run', side_effect=FileNotFoundError):
            self.assertIsNone(telemetry.gpu_state())
        with patch.object(telemetry.subprocess, 'run', side_effect=subprocess.TimeoutExpired('nvidia-smi', 10)):
            self.assertIsNone(telemetry.gpu_state())

    def test_tsc_rate_ignores_preempted_spins(self):
        # Fake clock: 0.5 ms per perf_counter call; one early spin loses 3 ms to another thread.
        state = {'now': 0.0, 'lost': 0.0, 'calls': 0}
        def perf_counter():
            state['calls'] += 1
            state['now'] += 0.0005
            if state['calls'] == 30:
                state['now'] += 0.003
                state['lost'] += 0.003
            return state['now']
        def query(thread, value):
            value._obj.value = int((state['now'] - state['lost']) * 2.6e9)
        kernel = Mock()
        kernel.QueryThreadCycleTime.side_effect = query
        with patch.object(telemetry.ctypes, 'WinDLL', return_value=kernel),                 patch.object(telemetry.time, 'perf_counter', side_effect=perf_counter):
            rate = telemetry.tsc_hz()
        self.assertEqual(kernel.QueryThreadCycleTime.call_count, 400)
        self.assertAlmostEqual(rate / 2.6e9, 1.0, places=6)

    def test_performance_logicals_use_the_highest_efficiency_class(self):
        self.assertEqual(telemetry.performance_logicals(TOPOLOGY), {0, 1})


class SchedulingTests(unittest.TestCase):
    def test_unknown_mode_rejected_and_default_touches_no_process(self):
        with self.assertRaises(ValueError):
            scheduling.apply(1, 'turbo', TOPOLOGY)
        with patch.object(scheduling, '_kernel') as kernel:
            self.assertEqual(scheduling.apply(1, 'default', TOPOLOGY), {'mode': 'default', 'controls': []})
        kernel.assert_not_called()

    def test_runner_rejects_culling_batch_toggle_without_the_plugin(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('shadow_benchmark_batch', BENCH / 'run.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        with self.assertRaisesRegex(ValueError, 'cannot toggle culling batches'):
            runner.Runner('og', 'unused', baseline=True, slot=Mock(), lab=runner.ROOT / '.runtime-tests/labs/x',
                          culling_batch=True)

    def test_runner_rejects_unknown_mode_before_creating_a_run(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('shadow_benchmark_scheduling', BENCH / 'run.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        with self.assertRaisesRegex(ValueError, 'Unknown scheduling mode'):
            runner.Runner('og', 'unused', slot=Mock(), lab=runner.ROOT / '.runtime-tests/labs/x', scheduling_mode='turbo')


class PlacementPatchTests(unittest.TestCase):
    def test_placement_anchors_apply_once_to_upstream_probe(self):
        source = prepare_support.SOURCE / 'probe-common/RenderedThroughput.h'
        if not source.exists():
            self.skipTest('upstream probe source unavailable')
        text, written = source.read_text(), {}
        with patch.object(Path, 'read_text', lambda self, *a, **k: text), \
                patch.object(Path, 'write_text', lambda self, value, *a, **k: written.setdefault('text', value)):
            prepare_support.placement_support()
        patched = written['text']
        for needle in ('inline void Placement', 'Placement(slot.frame.beginCycles', 'Placement(slot.frame.worldEndCycles',
                       r'\"begin_cpu\":', r'\"world_end_cycles\":'):
            self.assertEqual(patched.count(needle), 1, needle)


if __name__ == '__main__':
    unittest.main()
