"""Compare a run's OFF and ON captures (same static view, taken 2 s apart).

A pixel counts as changed when any channel differs by more than THRESHOLD levels.
Native frame noise (TAA, wind, NPC motion) sets the floor, so judge a run against a
control run of the same scene, not against zero.

Usage: python -B tools/benchmark/image_diff.py .runtime-tests/runs/<label> [...]
"""
import sys
from pathlib import Path

import numpy

THRESHOLD = 8


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=4)
    if parts[0] != b'P6' or int(parts[3]) != 255:
        raise ValueError(f'{path}: not an 8-bit binary PPM')
    width, height = int(parts[1]), int(parts[2])
    pixels = numpy.frombuffer(parts[4][:width * height * 3], dtype=numpy.uint8)
    return pixels.reshape(height, width, 3).astype(numpy.int16)


def compare(run, off_name='off.ppm', on_name='on.ppm'):
    off, on = read_ppm(run / 'probe' / off_name), read_ppm(run / 'probe' / on_name)
    difference = numpy.abs(on - off).max(axis=2)
    changed = difference > THRESHOLD
    # Tiles show whether changes cluster (a real shadow change) or scatter (noise).
    rows, columns = 9, 16
    height, width = changed.shape
    tiles = changed[:height // rows * rows, :width // columns * columns]
    tiles = tiles.reshape(rows, height // rows, columns, width // columns).mean(axis=(1, 3))
    return {'mean_abs': float(numpy.abs(on - off).mean()), 'changed_pct': float(changed.mean() * 100),
            'max': int(difference.max()), 'worst_tile_pct': float(tiles.max() * 100)}


def main(paths):
    if not paths:
        print(__doc__)
        return 2
    def show(label, result):
        print(f"{label}: mean |ON-OFF| {result['mean_abs']:.3f} levels; "
              f"{result['changed_pct']:.3f}% pixels > {THRESHOLD}; worst 120x120 tile {result['worst_tile_pct']:.2f}%; "
              f"max {result['max']}")
    for path in paths:
        run = Path(path)
        if (run / 'probe/off.ppm').exists():
            show(run.name + ' static', compare(run))
        # Motion check: the frame after each small view change, rules OFF against ON.
        index = 0
        while (run / f'probe/motion-off-{index}.ppm').exists() and (run / f'probe/motion-on-{index}.ppm').exists():
            show(f'{run.name} motion {index}', compare(run, f'motion-off-{index}.ppm', f'motion-on-{index}.ppm'))
            index += 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
