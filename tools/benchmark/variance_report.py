"""Attribute window-to-window throughput variation to machine state.

Usage: python -B tools/benchmark/variance_report.py <run folder> [...] [--json out.json]

Per window: throughput; render-thread CPU world time and how much of it the thread was
actually running (probe cycle counts); the share of frames on efficiency cores; busy-weighted
core clocks; foreign CPU; GPU temperature. Then rank correlations across all windows given.
Runs recorded before this telemetry existed show only the probe timing columns.
"""
import argparse
import json
import statistics
from pathlib import Path


def rank(values):
    order = sorted(range(len(values)), key=values.__getitem__)
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2
        i = j + 1
    return ranks


def spearman(x, y):
    pairs = [(a, b) for a, b in zip(x, y) if a is not None and b is not None]
    if len(pairs) < 4:
        return None
    rx, ry = rank([a for a, _ in pairs]), rank([b for _, b in pairs])
    mx, my = statistics.mean(rx), statistics.mean(ry)
    sxy = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    sx, sy = sum((a - mx) ** 2 for a in rx) ** .5, sum((b - my) ** 2 for b in ry) ** .5
    return sxy / (sx * sy) if sx and sy else None


def windows(run):
    case = json.loads((run / 'case.json').read_text())
    frames = [json.loads(line) for line in (run / 'probe/rendered-throughput.jsonl').read_text().splitlines()]
    tsc = case.get('tsc_hz')
    fast = None
    if case.get('cpu_topology'):
        best = max(row['efficiency_class'] for row in case['cpu_topology'])
        fast = {row['logical'] for row in case['cpu_topology'] if row['efficiency_class'] == best}
    for w in case.get('windows', []):
        m = w['measurement']
        number, frequency = m['rendered_throughput']['started']['window'], m['qpc_frequency']
        selected = [f for f in frames if f.get('type') == 'world_frame' and f.get('window') == number and
                    m['start_qpc'] <= f['begin_qpc'] and f['end_qpc'] <= m['end_qpc']]
        world = [(f['world_end_qpc'] - f['begin_qpc']) / frequency for f in selected]
        row = {'run': run.name, 'tag': w['tag'], 'native': case['plugin_absent_baseline'],
               'enabled': w['enabled'], 'scheduling': case.get('scheduling_mode', 'default (unrecorded)'),
               'rate': w['validation']['completed_world_renders_per_second'],
               'cpu_world_ms': 1000 * statistics.median(world) if world else None,
               'interval_p95_ms': w.get('frame_interval_p95_ms')}
        placed = [f for f in selected if 'begin_cpu' in f]
        if placed and tsc:
            running = [(f['world_end_cycles'] - f['begin_cycles']) / tsc / max(t, 1e-9)
                       for f, t in zip(selected, world) if 'begin_cycles' in f]
            row['running_fraction'] = statistics.median(running)
            if fast is not None:
                row['efficiency_core_frames'] = sum(f['begin_cpu'] not in fast or f['world_end_cpu'] not in fast
                                                    for f in placed) / len(placed)
            row['migrated_frames'] = sum(f['begin_cpu'] != f['world_end_cpu'] for f in placed) / len(placed)
        machine = w.get('machine') or {}
        row.update({key: machine.get(key) for key in ('performance_core_mhz', 'efficiency_core_mhz',
                                                       'foreign_cores_mean', 'foreign_cores_max', 'game_cores_mean')})
        row['foreign_top'] = [f"{f['name']}:{f['cores_mean']:.2f}" for f in machine.get('foreign_top', [])[:3]]
        gpu = [g for g in (w.get('gpu_before'), w.get('gpu_after')) if g]
        row['gpu_temp_c'] = statistics.mean(g['temperature.gpu'] for g in gpu) if gpu and all(
            g['temperature.gpu'] is not None for g in gpu) else None
        row['gpu_thermal_slowdown'] = any(g.get('thermal_slowdown') for g in gpu) if gpu else None
        yield row


def text(value, digits=1):
    return '-' if value is None else (f'{value:.{digits}f}' if isinstance(value, float) else str(value))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runs', nargs='+', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    rows = [row for run in args.runs for row in windows(run)]
    columns = [('rate', 'rate', 1), ('cpu_world_ms', 'cpuW', 2), ('running_fraction', 'run%', 2),
               ('efficiency_core_frames', 'Ecore', 2), ('migrated_frames', 'migr', 2),
               ('performance_core_mhz', 'P-MHz', 0), ('efficiency_core_mhz', 'E-MHz', 0),
               ('foreign_cores_mean', 'forgn', 2), ('foreign_cores_max', 'fmax', 2), ('gpu_temp_c', 'gpuC', 0)]
    current = None
    for row in rows:
        if row['run'] != current:
            current = row['run']
            print(f"\n{current}  scheduling={row['scheduling']}")
            print('  ' + f"{'tag':10}" + ''.join(f'{title:>7}' for _, title, _ in columns) + '  foreign top')
        print('  ' + f"{row['tag']:10}" + ''.join(f'{text(row.get(key), d):>7}' for key, _, d in columns) +
              '  ' + ' '.join(row['foreign_top']))
    print('\nSpearman rank correlation with throughput across all windows above:')
    for key in ('cpu_world_ms', 'running_fraction', 'efficiency_core_frames', 'performance_core_mhz',
                'foreign_cores_mean', 'gpu_temp_c'):
        value = spearman([r['rate'] for r in rows], [r.get(key) for r in rows])
        print(f'  {key:24} {text(value, 2)}')
    for run in dict.fromkeys(r['run'] for r in rows):
        rates = [r['rate'] for r in rows if r['run'] == run]
        print(f'  {run}: median {statistics.median(rates):.2f}, CV {statistics.pstdev(rates) / statistics.mean(rates):.1%}')
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
