"""Summarize private runs: throughput, shadow census and light-assignment cost.

Each measured window resets the plugin counters at its start and samples them at
its end, so the n-th "[LightCost] window" / "[Census] window" line in the copied
FasterShadows.log belongs to the n-th window in case.json.

Usage: python -B tools/benchmark/light_report.py .runtime-tests/runs/<label> [...]
"""
import json
import re
import statistics
import sys
from pathlib import Path

LIGHT = re.compile(r'\[LightCost\] window frames=(\d+) lightLists=([\d.]+) ms/frame '
                   r'objectLighting=([\d.]+) ms/frame objectCalls/frame=([\d.]+)')
CENSUS = re.compile(r'\[Census\] window actor candidates=(\d+) rain tested=(\d+) candidates=(\d+) removed=(\d+)')
# Staggered cascades (since 2026-10-12): cascades left out, registrations dropped, leftover
# batches in a left-out accumulator, and lamp slots equal to a sun cascade slot.
CADENCE = re.compile(r'\[Census\] window .*?cadence mid=(\d+) far=(\d+) dropped=(\d+) leaks=(\d+) slotConflicts=(\d+)')
BLENDS = re.compile(r'\[Census\] window .*?sunBlending=(\d+)')
# Throttle is worth designing only when light assignment is a visible share of the frame.
WORTH_MS, WORTH_SHARE = 0.5, 0.03


def windows(run):
    case = json.loads((run / 'case.json').read_text())
    log = run / 'FasterShadows.log'
    text = log.read_text(errors='replace') if log.exists() else ''
    light, census, cadence = LIGHT.findall(text), CENSUS.findall(text), CADENCE.findall(text)
    blends = BLENDS.findall(text)
    rows = []
    for index, window in enumerate(case.get('windows', [])):
        row = {'tag': window['tag'], 'on': window['enabled'],
               'renders': window['validation'].get('completed_world_renders_per_second'),
               'interval': window.get('frame_interval_median_ms'), **window['counters']}
        before, after = window.get('sun_state_before'), window.get('sun_state_after')
        if before and after and before.get('present') and after.get('present'):
            row['sun_changed'] = before['direction'] != after['direction']
        if index < len(light):
            frames, lists, objects, calls = light[index]
            row.update(frames=int(frames), lists=float(lists), objects=float(objects), calls=float(calls))
        if index < len(census):
            row.update(zip(('actor', 'rain_tested', 'rain_candidates', 'removed'), map(int, census[index])))
        if index < len(cadence):
            row.update(zip(('mid_waits', 'far_waits', 'dropped', 'leaks', 'conflicts'), map(int, cadence[index])))
        if index < len(blends):
            row['sun_blending'] = int(blends[index])
        rows.append(row)
    return case, rows, len(light), len(census)


def mean(rows, key, on=None):
    values = [r[key] for r in rows if r.get(key) is not None and (on is None or r['on'] == on)]
    return statistics.mean(values) if values else None


def text(value, digits=2):
    return '-' if value is None else f'{value:.{digits}f}' if isinstance(value, float) else str(value)


def report(run):
    case, rows, light, census = windows(run)
    scene = case.get('scene_parameters', {})
    print(f"## {run.name}: {scene.get('name')} at {scene.get('hour')}:00")
    print(f"caster-filter dry run (ON filters remove no casters): {case.get('dry_run', False)}; "
          f"timescale: {scene.get('timescale', 'unrecorded')}; "
          f"diagnostic counters: {case.get('diagnostics', True)}; "
          f"INI overrides: {' '.join(case.get('ini_overrides', [])) or 'none'}; "
          f"reduced start RAM gate: {case.get('reduced_start_ram_gate', False)}; "
          f"log lines: {light} LightCost, {census} Census for {len(rows)} windows")
    columns = ['tag', 'on', 'renders', 'interval', 'sun_tested', 'sun_candidates', 'point_tested',
               'point_candidates', 'actor', 'rain_candidates', 'mid_waits', 'far_waits', 'dropped', 'leaks',
               'conflicts', 'sun_blending', 'sun_changed', 'lists', 'objects', 'calls']
    print('| ' + ' | '.join(columns) + ' |')
    print('|' + '---|' * len(columns))
    for row in rows:
        print('| ' + ' | '.join(text(row.get(c)) for c in columns) + ' |')
    off, on = mean(rows, 'renders', False), mean(rows, 'renders', True)
    if off and on:
        print(f"\nworld renders/s: OFF {off:.2f}, ON {on:.2f} ({(on / off - 1) * 100:+.2f}%)")
        # Medians resist a single drifting window (the first window after a load often runs fast).
        offs = [r['renders'] for r in rows if r['renders'] and not r['on']]
        ons = [r['renders'] for r in rows if r['renders'] and r['on']]
        m_off, m_on = statistics.median(offs), statistics.median(ons)
        print(f"medians: OFF {m_off:.2f}, ON {m_on:.2f} ({(m_on / m_off - 1) * 100:+.2f}%)")
    cost = [r['lists'] + r['objects'] for r in rows if 'lists' in r]
    interval = mean(rows, 'interval')
    if cost and interval:
        average = statistics.mean(cost)
        share = average / interval
        verdict = 'worth designing' if average >= WORTH_MS and share >= WORTH_SHARE else 'not worth it'
        print(f"light assignment: {average:.3f} ms/frame summed over threads, "
              f"{share * 100:.1f}% of the {interval:.2f} ms median frame interval -> throttle {verdict} "
              f"(threshold {WORTH_MS} ms and {WORTH_SHARE * 100:.0f}%)")
    print()


def main(paths):
    if not paths:
        print(__doc__)
        return 2
    for path in paths:
        report(Path(path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
