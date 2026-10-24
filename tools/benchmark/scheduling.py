"""Opt-in scheduling controls for the measured game, applied identically to native and candidate.

The private desktop never makes the game the foreground process, so Windows may treat it as
background work (EcoQoS: lower clocks, efficiency cores). A real player's game is foreground.
"""
from __future__ import annotations
import ctypes
from ctypes import wintypes

PROCESS_SET_INFORMATION = 0x0200
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_SET_LIMITED_INFORMATION = 0x2000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_POWER_THROTTLING = 4  # PROCESS_INFORMATION_CLASS ProcessPowerThrottling
EXECUTION_SPEED = 0x1
MODES = {
    'default': (),
    'foreground': ('power_throttling_off',),
    'performance-cores': ('power_throttling_off', 'performance_cores'),
}


class PowerThrottlingState(ctypes.Structure):
    _fields_ = [('Version', wintypes.ULONG), ('ControlMask', wintypes.ULONG), ('StateMask', wintypes.ULONG)]


def _kernel():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    for name in ('SetProcessInformation', 'GetProcessInformation'):
        getattr(kernel, name).argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetProcessDefaultCpuSets.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG]
    kernel.GetProcessDefaultCpuSets.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG,
                                                ctypes.POINTER(wintypes.ULONG)]
    return kernel


def apply(pid, mode, topology):
    """Apply one named mode; returns a record of what was set and read back. Raises on failure."""
    if mode not in MODES:
        raise ValueError('Unknown scheduling mode: ' + mode)
    record = {'mode': mode, 'controls': list(MODES[mode])}
    if not MODES[mode]:
        return record
    kernel = _kernel()
    access = PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_SET_LIMITED_INFORMATION | \
        PROCESS_QUERY_LIMITED_INFORMATION
    handle = kernel.OpenProcess(access, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if 'power_throttling_off' in MODES[mode]:
            state = PowerThrottlingState(1, EXECUTION_SPEED, 0)  # controlled, never throttled
            if not kernel.SetProcessInformation(handle, PROCESS_POWER_THROTTLING, ctypes.byref(state),
                                                ctypes.sizeof(state)):
                raise ctypes.WinError(ctypes.get_last_error())
            check = PowerThrottlingState(1, 0, 0)
            if not kernel.GetProcessInformation(handle, PROCESS_POWER_THROTTLING, ctypes.byref(check),
                                                ctypes.sizeof(check)):
                raise ctypes.WinError(ctypes.get_last_error())
            if (check.ControlMask & EXECUTION_SPEED, check.StateMask & EXECUTION_SPEED) != (EXECUTION_SPEED, 0):
                raise RuntimeError('Power throttling opt-out did not read back')
            record['power_throttling'] = {'control_mask': check.ControlMask, 'state_mask': check.StateMask}
        if 'performance_cores' in MODES[mode]:
            best = max(row['efficiency_class'] for row in topology)
            ids = [row['cpu_set_id'] for row in topology if row['efficiency_class'] == best]
            array = (wintypes.ULONG * len(ids))(*ids)
            if not kernel.SetProcessDefaultCpuSets(handle, array, len(ids)):
                raise ctypes.WinError(ctypes.get_last_error())
            back, count = (wintypes.ULONG * 64)(), wintypes.ULONG()
            if not kernel.GetProcessDefaultCpuSets(handle, back, 64, ctypes.byref(count)):
                raise ctypes.WinError(ctypes.get_last_error())
            if sorted(back[:count.value]) != sorted(ids):
                raise RuntimeError('Default CPU sets did not read back')
            record['default_cpu_sets'] = ids
    finally:
        kernel.CloseHandle(handle)
    return record
