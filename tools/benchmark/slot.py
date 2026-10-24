"""Single-use, bounded shared-machine claims; cleanup never touches foreign jobs."""
from __future__ import annotations
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import subprocess
import time
import uuid
from datetime import datetime, timedelta, timezone
import psutil
import win32con
import win32job

ROOT = Path(__file__).resolve().parents[2]
LEASE = Path(r'C:\Development\Realistic Reflections FO4 - Glass\.tmp\launch-coordination\priority-window.json')
WRITER = LEASE.with_name(LEASE.name + '.lock')
GAME_LOCK = Path(r'C:\Games\GAME_LOCK.json')
OWNER = '01a1183c-fea2-7be0-ad46-6b1ffe1ae1f8'
GAMES = {'fallout4.exe', 'fallout4vr.exe', 'skyrimse.exe', 'skyrimvr.exe', 'starfield.exe', 'creationkit.exe'}
BUILDERS = {'cl.exe', 'link.exe', 'ninja.exe', 'msbuild.exe', 'clang-cl.exe', 'clang.exe', 'lld-link.exe',
            'cmake.exe', 'ctest.exe', 'fxc.exe', 'dxc.exe', 'csc.exe', 'vbc.exe', 'ghidrarun.exe',
            'java.exe', 'javaw.exe'}
READY = {'source_ready_awaiting_fresh_handoff', 'ready'}


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atomic(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def present(path):
    try:
        Path(path).lstat()  # A broken symlink is still a blocking lock/hold.
        return True
    except FileNotFoundError:
        return False


def owns(path, token):
    try:
        value = read(path)
        return value.get('claim_token') == token and value.get('nonce') == token
    except (OSError, ValueError):
        return False


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise RuntimeError('Explicit integer required: ' + name)
    return value


class Slot:
    # exclusive=False is for untimed work only (visual captures, builds), as the user
    # allowed on 2026-10-21: no shared lease, writer lock or GAME_LOCK is taken, and
    # other projects' games and compilers may run alongside. The job object, recovery
    # journal, RAM floor, user priority hold and local launch hold still apply.
    # Timing runs keep the default exclusive claims.
    def __init__(self, handoff, label, *, request_path, minutes=10, minimum_ram=2,
                 phase='cpu', game_starts=0, targets=(), exclusive=True):
        if phase not in {'cpu', 'runtime'} or game_starts not in {0, 1} or (phase == 'cpu' and game_starts):
            raise ValueError('CPU slots allow zero games; runtime slots allow at most one')
        if not math.isfinite(minutes) or minutes <= 0 or not math.isfinite(minimum_ram) or minimum_ram <= 0:
            raise ValueError('Positive finite bounds required')
        if not label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in label):
            raise ValueError('Simple fresh label required')
        self.handoff = Path(handoff).resolve()
        self.request_path = Path(request_path).resolve()
        if not self.request_path.is_relative_to((ROOT / '.runtime-tests').resolve()):
            raise ValueError('Owner request must be in this workspace')
        self.label, self.minutes, self.minimum_ram = label, minutes, minimum_ram
        self.phase, self.maximum_game_starts, self.targets = phase, game_starts, set(targets)
        self.exclusive = exclusive
        self.token = secrets.token_hex(32)
        self.job = None
        self.children = []
        self.unassigned = []
        self.record = None
        self.recovery_path = ROOT / '.runtime-tests/coordination' / ('claim-' + label + '.json')
        self.recovery = None
        self.total_deadline = self.work_deadline = None
        self.restoration_verified = True  # No child or external configuration has been changed yet.
        self.seen_games = set()
        self.receipt = {'label': label, 'started': datetime.now(timezone.utc).isoformat(),
                        'claim_token': self.token, 'phase': phase, 'maximum_minutes': minutes, 'exclusive': exclusive,
                        'commands': [], 'game_starts': 0, 'player_changes': False, 'terminal': False}

    def owner_ready(self, claimed=False):
        state = read(self.request_path)
        if state.get('owner_thread') != OWNER or state.get('project') != 'Faster Shadows' or state.get('phase') != self.phase:
            raise RuntimeError('Owner request identity/phase mismatch')
        if present(ROOT / '.runtime-tests/launch-hold.json'):
            raise RuntimeError('Local user launch hold')
        if claimed:
            if state.get('status') != 'active' or state.get('claim_token') != self.token:
                raise RuntimeError('Owner request paused, withdrawn, or changed')
        elif state.get('status') not in READY or state.get('ready') is not True:
            raise RuntimeError('Owner request is not ready; paused requests cannot be resumed by the runner')
        return state

    def validate_grant(self):
        state = self.owner_ready()
        grant = read(self.handoff)
        expiry = datetime.fromisoformat(grant['expires'].replace('Z', '+00:00'))
        now = datetime.now(timezone.utc)
        if expiry.tzinfo is None or expiry <= now:
            raise RuntimeError('Fresh unexpired handoff required')
        if (grant.get('status') != 'handed_off' or grant.get('restoration_verified') is not True or
                grant.get('single_use') is not True or grant.get('next_thread') != OWNER or
                grant.get('next_project') != 'Faster Shadows' or grant.get('phase') != self.phase):
            raise RuntimeError('Actual restored, single-use handoff for this owner and phase required')
        self.request_hash = digest(self.request_path)
        if (Path(grant['owner_request']).resolve() != self.request_path or
                grant.get('owner_request_sha256') != self.request_hash):
            raise RuntimeError('Handoff does not bind the current owner request')
        for key in ('maximum_game_starts', 'maximum_game_launches'):
            if integer(grant.get(key), key) != self.maximum_game_starts:
                raise RuntimeError('Explicit game launch bound mismatch')
        if integer(state.get('maximum_game_starts'), 'request games') != self.maximum_game_starts:
            raise RuntimeError('Owner request game bound mismatch')
        floor=state.get('minimum_ram_gib',self.minimum_ram)
        if type(floor) not in (int,float) or not math.isfinite(floor) or floor<=0 or self.minimum_ram<floor:
            raise RuntimeError('Runner RAM gate is below the owner request minimum')
        if (integer(grant.get('maximum_workers'), 'grant workers', 1) != 1 or
                integer(state.get('workers'), 'request workers', 1) != 1):
            raise RuntimeError('Only one worker is supported')
        if integer(grant.get('automatic_retries'), 'grant retries') or integer(state.get('automatic_retries'), 'request retries'):
            raise RuntimeError('Automatic retries are forbidden')
        if not self.targets or self.targets != set(state.get('targets', [])) or not self.targets.issubset(set(grant.get('targets', []))):
            raise RuntimeError('Explicit target scope mismatch')
        total = integer(grant.get('maximum_total_seconds'), 'total seconds', 1)
        work = integer(grant.get('maximum_work_seconds'), 'work seconds', 1)
        cleanup = integer(grant.get('minimum_cleanup_seconds'), 'cleanup seconds', 30)
        requested = self.minutes * 60
        if work + cleanup > total or requested > total or requested > integer(state.get('maximum_minutes'), 'request minutes', 1) * 60:
            raise RuntimeError('Work/cleanup bounds exceed the agreed grant')
        identity = str(uuid.UUID(grant['handoff_id']))
        remaining = min(requested, total, (expiry - now).total_seconds())
        if remaining <= cleanup:
            raise RuntimeError('Handoff has insufficient cleanup reserve')
        self.total_deadline = time.monotonic() + remaining
        self.work_deadline = min(self.total_deadline - cleanup, time.monotonic() + work)
        self.receipt['handoff'] = {'path': str(self.handoff), 'handoff_id': identity,
                                   'sha256': digest(self.handoff), 'owner_request_sha256': self.request_hash,
                                   'cleanup_seconds': cleanup, 'work_seconds': self.work_deadline - time.monotonic()}
        return identity

    def job_pids(self):
        if self.job is None:
            return set()
        # pywin32 returns a tuple, not a dictionary, for this information class.
        return set(win32job.QueryInformationJobObject(self.job, win32job.JobObjectBasicProcessIdList))

    def gate(self, claimed=False, minimum_ram=None):
        self.owner_ready(claimed)
        lease = read(LEASE)
        if lease.get('user_priority'):
            raise RuntimeError('User priority hold')
        if not self.exclusive:
            pass  # Shared claims belong to exclusive (timing) work; untimed work neither takes nor waits for them.
        elif claimed:
            if lease.get('status') != 'active' or not all(owns(p, self.token) for p in (LEASE, GAME_LOCK, WRITER)):
                raise RuntimeError('Own matching exclusive claims lost')
        elif present(GAME_LOCK) or present(WRITER) or lease.get('status') != 'released':
            raise RuntimeError('Existing lock or unreleased lease; expiry is not restoration')
        own = self.job_pids() if claimed else set()
        foreign = []
        for process in psutil.process_iter(['pid', 'name', 'create_time']):
            name = (process.info['name'] or '').lower()
            if process.pid in own:
                if name in GAMES:
                    self.seen_games.add((process.pid, process.info['create_time']))
                continue
            if self.exclusive and (name in GAMES or name in BUILDERS):
                foreign.append({'pid': process.pid, 'name': name})
        self.receipt['game_starts'] = len(self.seen_games)
        if len(self.seen_games) > self.maximum_game_starts:
            raise RuntimeError('Owned game-start bound exceeded')
        if foreign:
            raise RuntimeError('Foreign game/build blocks slot: ' + str(foreign))
        available = psutil.virtual_memory().available / 2**30
        floor = self.minimum_ram if minimum_ram is None else minimum_ram
        if available < floor:
            raise RuntimeError(f'Available physical RAM {available:.2f} GiB < {floor:.2f} GiB')
        if self.work_deadline is not None and time.monotonic() >= self.work_deadline:
            raise RuntimeError('Bounded work deadline reached; cleanup reserve begins now')
        return {'available_ram_gib': available, 'foreign_games_builds': foreign}

    def __enter__(self):
        identity = self.validate_grant()
        self.receipt['admission'] = self.gate()
        if self.exclusive and psutil.cpu_percent(interval=1) > 30:
            raise RuntimeError('CPU busy before isolated measurements')
        now = datetime.now(timezone.utc)
        self.record = {'id': self.label, 'project': 'Faster Shadows', 'owner_thread': OWNER, 'owner_pid': os.getpid(),
                       'owner_create_time': psutil.Process(os.getpid()).create_time(),
                       'claim_token': self.token, 'nonce': self.token, 'ownership_token': self.token,
                       'created': now.isoformat(), 'expires': (now + timedelta(seconds=self.total_deadline - time.monotonic())).isoformat(),
                       'status': 'active', 'phase': self.phase, 'automatic_relaunch': False,
                       'maximum_game_launches': self.maximum_game_starts, 'maximum_workers': 1,
                       'exclusive': self.exclusive}
        try:
            # Persist before publishing claims or entering a potentially long quiet wait.
            # Missing case.json is safe to recover only while no child creation was attempted.
            self.recovery = {'schema': 1, 'claim': self.record.copy(), 'owner_request': str(self.request_path),
                             'child_start_attempts': 0, 'children': []}
            self.persist_recovery('claimed_no_child')
            self.job = win32job.CreateJobObject(None, '')
            limits = win32job.QueryInformationJobObject(self.job, win32job.JobObjectExtendedLimitInformation)
            limits['BasicLimitInformation']['LimitFlags'] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            win32job.SetInformationJobObject(self.job, win32job.JobObjectExtendedLimitInformation, limits)
            if self.exclusive:
                with WRITER.open('x', encoding='utf-8') as stream:
                    json.dump(self.record, stream)
                lease = read(LEASE)
                if lease.get('status') != 'released' or lease.get('user_priority') or present(GAME_LOCK):
                    raise RuntimeError('Claim race or user priority hold')
            self.owner_ready()
            if digest(self.request_path) != self.request_hash:
                raise RuntimeError('Owner request changed after handoff validation')
            consumed = ROOT / '.runtime-tests/coordination/consumed' / (identity + '.json')
            consumed.parent.mkdir(parents=True, exist_ok=True)
            with consumed.open('x', encoding='utf-8') as stream:
                json.dump({'handoff_id': identity, 'claim_token': self.token, 'owner_request_sha256': self.request_hash}, stream)
            if self.exclusive:
                with GAME_LOCK.open('x', encoding='utf-8') as stream:
                    json.dump(self.record, stream)
                atomic(LEASE, self.record)
            state = read(self.request_path)
            state.update(status='active', ready=False, claim_token=self.token, current_attempt=self.label)
            atomic(self.request_path, state)
            self.receipt['claim'] = self.record
            self.gate(claimed=True)
            return self
        except BaseException:
            self.release()
            raise

    def child_environment(self, env=None):
        environment = {key.upper(): value for key, value in (os.environ if env is None else env).items()}
        environment['MSBUILDDISABLENODEREUSE'] = '1'
        if self.exclusive:
            environment.pop('DTL_BENCH_COEXIST', None)
            environment['DTL_BENCH_OWNED_LEASE'] = json.dumps({'path': str(LEASE), 'id': self.label,
                'owner_thread': OWNER, 'ownership_token': self.token})
        else:
            # The private host then skips shared reservations (support/launch_coordination.py).
            environment.pop('DTL_BENCH_OWNED_LEASE', None)
            environment['DTL_BENCH_COEXIST'] = '1'
        return environment

    def persist_recovery(self, lifecycle):
        self.recovery.update(lifecycle=lifecycle, updated=datetime.now(timezone.utc).isoformat())
        atomic(self.recovery_path, self.recovery)

    def spawn(self, args, *, env=None, affinity=None, **kwargs):
        self.gate(claimed=True)
        # This write must finish before Popen: interruption during process creation
        # must never leave a journal that certifies that no child could have existed.
        self.recovery['child_start_attempts'] += 1
        self.persist_recovery('child_starting')
        process = subprocess.Popen(args, cwd=ROOT, env=self.child_environment(env),
            creationflags=subprocess.CREATE_NO_WINDOW | win32con.CREATE_SUSPENDED, **kwargs)
        self.children.append(process)
        self.unassigned.append(process)
        try:
            child = {'pid': process.pid, 'create_time': None}
            self.recovery['children'].append(child)
            try:
                child['create_time'] = psutil.Process(process.pid).create_time()
            except psutil.NoSuchProcess:
                pass  # Keep the attempted PID; recovery remains conservative.
            self.persist_recovery('child_created')
            win32job.AssignProcessToJobObject(self.job, int(process._handle))
            self.unassigned.remove(process)
            if affinity is not None:
                psutil.Process(process.pid).cpu_affinity([affinity])
            self.gate(claimed=True)  # Recheck holds/user games before resuming this suspended child.
            resume = ctypes.windll.ntdll.NtResumeProcess
            resume.argtypes, resume.restype = [ctypes.c_void_p], ctypes.c_long
            if resume(int(process._handle)) != 0:
                raise RuntimeError('Cannot resume owned job process')
            return process
        except BaseException:
            # Assignment can fail before the child belongs to the job. Kill via
            # the original Popen handle, never by a potentially reused PID.
            try:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=max(.01, min(10, self.total_deadline - time.monotonic())))
            except (OSError, subprocess.TimeoutExpired):
                pass  # release() retains claims if termination cannot be verified.
            raise

    def command(self, args, output, env=None, affinity=None):
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('wb') as stream:
            process = self.spawn(args, env=env, affinity=affinity, stdout=stream, stderr=subprocess.STDOUT)
            while process.poll() is None:
                time.sleep(.25)
                self.gate(claimed=True)
            row = {'argv': args, 'returncode': process.returncode, 'output': str(output)}
            self.receipt['commands'].append(row)
            if process.returncode:
                raise RuntimeError('Owned command failed: ' + str(row))

    def release(self):
        errors = []
        for process in self.unassigned:
            try:
                if process.poll() is None:
                    process.kill()
            except OSError as error:
                errors.append(repr(error))
        if self.job is not None:
            try:
                win32job.TerminateJobObject(self.job, 0)
            except Exception as error:
                errors.append(repr(error))
        deadline = self.total_deadline or time.monotonic()
        remaining = set()
        while True:
            try:
                remaining = self.job_pids()
            except Exception as error:
                errors.append(repr(error))
                remaining = {-1}  # Unknown job membership cannot certify exit.
            remaining.update(p.pid for p in self.children if p.poll() is None)
            if not remaining or time.monotonic() >= deadline:
                break
            time.sleep(.05)
        clean = not remaining and self.restoration_verified
        if not remaining and self.job is not None:
            self.job.Close()
            self.job = None
        if owns(LEASE, self.token):
            record = read(LEASE)
            record.update(status='released' if clean else 'cleanup_failed',
                          phase='terminal' if clean else 'cleanup_failed', remaining_owned_pids=sorted(remaining),
                          restoration_verified=clean, terminal_proof=str(ROOT / '.runtime-tests/coordination' / ('terminal-' + self.label + '.json')))
            if clean:
                record['released'] = datetime.now(timezone.utc).isoformat()
            atomic(LEASE, record)
        if clean:
            for path in (GAME_LOCK, WRITER):
                if owns(path, self.token):
                    path.unlink()
        state = read(self.request_path)
        if state.get('status') == 'active' and state.get('claim_token') == self.token:
            state.update(status='terminal' if clean else 'cleanup_failed', ready=False, slot_claimed=not clean,
                         terminal_evidence=str(ROOT / '.runtime-tests/coordination' / ('terminal-' + self.label + '.json')))
            atomic(self.request_path, state)
        self.receipt.update(terminal=clean, finished=datetime.now(timezone.utc).isoformat(),
                            remaining_owned_pids=sorted(remaining), restoration_verified=self.restoration_verified,
                            cleanup_errors=errors, own_game_lock_absent=not owns(GAME_LOCK, self.token),
                            own_writer_lock_absent=not owns(WRITER, self.token),
                            lease_released=not owns(LEASE, self.token) or read(LEASE).get('status') == 'released')
        atomic(ROOT / '.runtime-tests/coordination' / ('terminal-' + self.label + '.json'), self.receipt)
        if self.recovery is not None:
            self.persist_recovery('terminal' if clean else 'cleanup_failed')
        return clean

    def __exit__(self, kind, error, trace):
        self.receipt['error'] = repr(error) if error else None
        if not self.release():
            raise RuntimeError('Owned cleanup not verified; claims retained') from error
