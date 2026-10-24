"""Machine state around timing windows: per-core clocks and load, foreign CPU, GPU thermals.

The probe records the render thread's core and on-CPU time per frame; this module
records what the game cannot see. One sample costs about 8 ms of runner CPU.
"""
from __future__ import annotations
import ctypes
import os
import statistics
import struct
import subprocess
import time

import psutil
import pywintypes
import win32pdh

STATUS_INFO_LENGTH_MISMATCH = 0xC0000004
GPU_FIELDS = ('temperature.gpu', 'clocks.gr', 'clocks.mem', 'power.draw', 'utilization.gpu', 'pstate',
              'clocks_event_reasons.active')


def cpu_topology():
    """Logical processors with their core and efficiency class (higher = faster core)."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    size = ctypes.c_ulong()
    kernel.GetSystemCpuSetInformation(None, 0, ctypes.byref(size), None, 0)
    buffer = ctypes.create_string_buffer(size.value)
    if not kernel.GetSystemCpuSetInformation(buffer, size, ctypes.byref(size), None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    rows, offset = [], 0
    while offset < size.value:
        length, = struct.unpack_from('<I', buffer.raw, offset)
        cpu_set, group, logical, core = struct.unpack_from('<IHBB', buffer.raw, offset + 8)
        efficiency, = struct.unpack_from('<B', buffer.raw, offset + 18)
        rows.append({'cpu_set_id': cpu_set, 'group': group, 'logical': logical, 'core': core,
                     'efficiency_class': efficiency})
        offset += length
    return rows


def performance_logicals(topology):
    best = max(row['efficiency_class'] for row in topology)
    return {row['logical'] for row in topology if row['efficiency_class'] == best}


class ProcessCpu:
    """All processes' CPU time from one NtQuerySystemInformation call (psutil opens each: ~650 ms)."""

    def __init__(self):
        self.ntdll = ctypes.WinDLL('ntdll')
        self.buffer = ctypes.create_string_buffer(4 << 20)

    def snapshot(self):
        size = ctypes.c_ulong()
        while True:
            status = self.ntdll.NtQuerySystemInformation(5, self.buffer, len(self.buffer), ctypes.byref(size)) & 0xFFFFFFFF
            if status != STATUS_INFO_LENGTH_MISMATCH:
                break
            self.buffer = ctypes.create_string_buffer(max(size.value, len(self.buffer)) * 2)
        if status:
            raise OSError(f'NtQuerySystemInformation failed: {status:#x}')
        raw, base, offset, rows = self.buffer.raw, ctypes.addressof(self.buffer), 0, {}
        while True:
            following, = struct.unpack_from('<I', raw, offset)
            user, kernel = struct.unpack_from('<qq', raw, offset + 0x28)
            length, = struct.unpack_from('<H', raw, offset + 0x38)
            pointer, = struct.unpack_from('<Q', raw, offset + 0x40)
            pid, = struct.unpack_from('<Q', raw, offset + 0x50)
            name = raw[pointer - base:pointer - base + length].decode('utf-16-le', 'replace') if pointer else 'Idle'
            rows[pid] = (name, user + kernel)  # 100 ns units
            if not following:
                return rows
            offset += following


class Sampler:
    """Sample every half second inside a window; prime() once just before it starts."""

    def __init__(self, own_pids):
        self.own_pids = own_pids  # callable: our runner, host, loader and game
        self.topology = cpu_topology()
        self.processes = ProcessCpu()
        self.query = win32pdh.OpenQuery()
        self.performance = {}
        for path in win32pdh.ExpandCounterPath(r'\Processor Information(*)\% Processor Performance'):
            instance = path.split('(')[1].split(')')[0]
            group, _, logical = instance.partition(',')
            if logical.isdigit():
                self.performance[int(logical)] = win32pdh.AddCounter(self.query, path)
        nominal = win32pdh.AddCounter(self.query, r'\Processor Information(_Total)\Processor Frequency')
        win32pdh.CollectQueryData(self.query)
        self.nominal_mhz = win32pdh.GetFormattedCounterValue(nominal, win32pdh.PDH_FMT_DOUBLE)[1]
        self.previous = None

    def prime(self):
        win32pdh.CollectQueryData(self.query)
        psutil.cpu_percent(percpu=True)
        self.previous = (time.perf_counter(), self.processes.snapshot())

    def sample(self, game_pid):
        win32pdh.CollectQueryData(self.query)
        busy = psutil.cpu_percent(percpu=True)
        now, processes = time.perf_counter(), self.processes.snapshot()
        before, old = self.previous
        self.previous = (now, processes)
        own = set(self.own_pids()) | {os.getpid(), game_pid}
        elapsed = (now - before) * 1e7
        game, foreign = 0.0, []
        for pid, (name, cpu) in processes.items():
            if pid == 0 or pid not in old or old[pid][0] != name:
                continue  # Idle, or started/reused during the interval
            cores = (cpu - old[pid][1]) / elapsed
            if pid == game_pid:
                game = cores
            elif pid not in own and cores > 0:
                foreign.append((cores, name, pid))
        foreign.sort(reverse=True)
        performance = {}
        for logical, counter in self.performance.items():
            try:
                performance[logical] = win32pdh.GetFormattedCounterValue(counter, win32pdh.PDH_FMT_DOUBLE)[1]
            except pywintypes.error:
                # PDH reports a negative denominator now and then (2026-10-17, window 10 of a
                # timing launch). That core has no clock this sample; the window stays valid.
                continue
        return {'perf_counter': now, 'busy_percent': busy,
                'effective_mhz': [self.nominal_mhz * performance[i] / 100 if i in performance else None
                                  for i in range(len(busy))],
                'game_cores': game, 'foreign_cores': sum(c for c, _, _ in foreign),
                'foreign_top': [{'name': n, 'pid': p, 'cores': c} for c, n, p in foreign[:5]]}


def summarize(samples, topology):
    """Window aggregates. Clocks are busy-weighted so idle cores do not dilute them."""
    if not samples:
        return None
    fast = performance_logicals(topology)

    def weighted_mhz(logicals):
        pairs = [(s['busy_percent'][i], s['effective_mhz'][i]) for s in samples for i in logicals
                 if i < len(s['busy_percent']) and s['effective_mhz'][i] is not None]
        weight = sum(b for b, _ in pairs)
        return sum(b * m for b, m in pairs) / weight if weight else None

    slow = {row['logical'] for row in topology} - fast
    by_name = {}
    for sample in samples:
        for row in sample['foreign_top']:
            by_name[row['name']] = by_name.get(row['name'], 0) + row['cores'] / len(samples)
    foreign = [s['foreign_cores'] for s in samples]
    return {'samples': len(samples), 'foreign_cores_mean': statistics.mean(foreign), 'foreign_cores_max': max(foreign),
            'foreign_top': sorted(({'name': n, 'cores_mean': c} for n, c in by_name.items()),
                                  key=lambda row: -row['cores_mean'])[:5],
            'game_cores_mean': statistics.mean(s['game_cores'] for s in samples),
            'performance_core_mhz': weighted_mhz(fast), 'efficiency_core_mhz': weighted_mhz(slow),
            'performance_core_busy_percent': statistics.mean(s['busy_percent'][i] for s in samples for i in fast),
            'efficiency_core_busy_percent': statistics.mean(s['busy_percent'][i] for s in samples for i in slow)
            if slow else None}


def gpu_state():
    """One nvidia-smi reading, taken outside timing intervals; None when unavailable."""
    try:
        result = subprocess.run(['nvidia-smi', '--query-gpu=' + ','.join(GPU_FIELDS), '--format=csv,noheader,nounits'],
                                capture_output=True, text=True, timeout=10,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode:
        return None
    values = [value.strip() for value in result.stdout.splitlines()[0].split(',')]
    state = dict(zip(GPU_FIELDS, values))
    for key in ('temperature.gpu', 'clocks.gr', 'clocks.mem', 'power.draw', 'utilization.gpu'):
        try:
            state[key] = float(state[key])
        except (KeyError, ValueError):
            state[key] = None
    try:
        reasons = int(state.get('clocks_event_reasons.active', ''), 16)
    except ValueError:
        reasons = None
    # NVML clock event bits: 0x4 software power cap, 0x20/0x40 software/hardware thermal slowdown.
    state['power_capped'] = None if reasons is None else bool(reasons & 0x4)
    state['thermal_slowdown'] = None if reasons is None else bool(reasons & 0x60)
    return state


def tsc_hz(seconds=0.002, repeats=200):
    """QueryThreadCycleTime units per second, for the probe's per-frame cycle counts.

    Preemption and interrupts lower a spin's rate, so spins stay far shorter than a
    scheduler quantum; the 95th percentile also drops the rare read-jitter overshoot.
    Call it before the game starts."""
    kernel = ctypes.WinDLL('kernel32')
    kernel.GetCurrentThread.restype = ctypes.c_void_p  # pseudo-handle must stay 64-bit
    kernel.QueryThreadCycleTime.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulonglong)]
    thread, cycles = kernel.GetCurrentThread(), ctypes.c_ulonglong()
    rates = []
    for _ in range(repeats):
        kernel.QueryThreadCycleTime(thread, ctypes.byref(cycles))
        start_cycles, start = cycles.value, time.perf_counter()
        while time.perf_counter() - start < seconds:
            pass
        kernel.QueryThreadCycleTime(thread, ctypes.byref(cycles))
        rates.append((cycles.value - start_cycles) / (time.perf_counter() - start))
    return statistics.quantiles(rates, n=20)[-1]
