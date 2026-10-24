"""Safety regressions use private temporary files and mocked jobs, never shared locks."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import uuid
from datetime import datetime, timedelta, timezone
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location('slot', Path(__file__).resolve().parents[1] / 'tools/benchmark/slot.py')
slot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(slot)


class SlotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lease = self.root / 'lease.json'
        self.writer = self.root / 'writer.lock'
        self.game = self.root / 'game.lock'
        self.request = self.root / '.runtime-tests/request.json'
        self.handoff = self.root / 'handoff.json'
        for name, value in [('ROOT', self.root), ('LEASE', self.lease), ('WRITER', self.writer), ('GAME_LOCK', self.game)]:
            p = patch.object(slot, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.state = dict(project='Faster Shadows', owner_thread=slot.OWNER, phase='cpu',
                          status='ready', ready=True, maximum_minutes=10, maximum_game_starts=0,
                          workers=1, automatic_retries=0, targets=['test'])
        slot.atomic(self.request, self.state)
        slot.atomic(self.lease, {'status': 'released'})
        self.grant = dict(handoff_id=str(uuid.uuid4()), status='handed_off', restoration_verified=True,
                          single_use=True, next_project='Faster Shadows', next_thread=slot.OWNER, phase='cpu',
                          owner_request=str(self.request), owner_request_sha256=slot.digest(self.request),
                          expires=(datetime.now(timezone.utc) + timedelta(minutes=30)).isoformat(),
                          maximum_game_starts=0, maximum_game_launches=0, maximum_workers=1,
                          automatic_retries=0, maximum_total_seconds=600, maximum_work_seconds=540,
                          minimum_cleanup_seconds=60, targets=['test'])
        slot.atomic(self.handoff, self.grant)
        self.processes = []
        mocks = [patch.object(slot.psutil, 'process_iter', side_effect=lambda *a: iter(self.processes)),
                 patch.object(slot.psutil, 'virtual_memory', return_value=Mock(available=8 * 2**30)),
                 patch.object(slot.psutil, 'cpu_percent', return_value=0),
                 patch.object(slot.win32job, 'CreateJobObject', return_value=Mock()),
                 patch.object(slot.win32job, 'SetInformationJobObject'),
                 patch.object(slot.win32job, 'TerminateJobObject'),
                 patch.object(slot.win32job, 'QueryInformationJobObject', side_effect=lambda job, kind:
                    {'BasicLimitInformation': {'LimitFlags': 0}} if kind == slot.win32job.JobObjectExtendedLimitInformation else ())]
        for p in mocks:
            p.start()
            self.addCleanup(p.stop)

    def make(self):
        return slot.Slot(self.handoff, 'isolated-test', request_path=self.request, targets=['test'])

    def change_grant(self, **values):
        self.grant.update(values)
        slot.atomic(self.handoff, self.grant)

    def test_paused_request_is_preserved(self):
        self.state.update(status='paused_for_user_reboot', ready=False)
        slot.atomic(self.request, self.state)
        before = self.request.read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'not ready'):
            self.make().__enter__()
        self.assertEqual(before, self.request.read_bytes())
        self.assertFalse(self.game.exists())
        self.assertFalse(self.writer.exists())

    def test_missing_explicit_zero_game_bound_rejected(self):
        del self.grant['maximum_game_launches']
        slot.atomic(self.handoff, self.grant)
        with self.assertRaisesRegex(RuntimeError, 'Explicit integer'):
            self.make().validate_grant()

    def test_request_hash_and_thread_bound(self):
        self.change_grant(owner_request_sha256='0' * 64)
        with self.assertRaisesRegex(RuntimeError, 'bind'):
            self.make().validate_grant()
        self.change_grant(owner_request_sha256=slot.digest(self.request), next_thread='foreign')
        with self.assertRaisesRegex(RuntimeError, 'this owner'):
            self.make().validate_grant()

    def test_cleanup_reserve_honors_grant(self):
        item = self.make()
        item.validate_grant()
        self.assertAlmostEqual(item.total_deadline - item.work_deadline, 60, delta=.01)
        self.change_grant(maximum_work_seconds=555)
        with self.assertRaisesRegex(RuntimeError, 'bounds'):
            self.make().validate_grant()

    def test_worker_target_retry_and_expiry_constraints(self):
        for values in [dict(maximum_workers=2), dict(targets=['other']), dict(automatic_retries=1),
                       dict(expires=(datetime.now(timezone.utc) - timedelta(seconds=1)).isoformat())]:
            old = self.grant.copy()
            self.change_grant(**values)
            with self.assertRaises(RuntimeError):
                self.make().validate_grant()
            self.grant = old

    def test_expired_unreleased_lease_still_blocks(self):
        slot.atomic(self.lease, {'status': 'active', 'expires': '2000-01-01T00:00:00Z'})
        with self.assertRaisesRegex(RuntimeError, 'unreleased'):
            self.make().gate()

    def test_success_claim_matches_both_locks_and_releases(self):
        with self.make() as item:
            self.assertEqual(len(item.token), 64)
            self.assertTrue(all(slot.owns(p, item.token) for p in [self.lease, self.game, self.writer]))
            journal = slot.read(item.recovery_path)
            self.assertEqual(journal['lifecycle'], 'claimed_no_child')
            self.assertEqual(journal['child_start_attempts'], 0)
            self.assertEqual(journal['children'], [])
            self.assertEqual(journal['claim'], slot.read(self.game))
            self.assertGreater(journal['claim']['owner_create_time'], 0)
            self.assertEqual(journal['owner_request'], str(self.request.resolve()))
            item.gate(claimed=True)
        self.assertTrue(item.receipt['terminal'])
        self.assertEqual(slot.read(self.lease)['status'], 'released')
        self.assertFalse(self.game.exists())
        self.assertFalse(self.writer.exists())
        self.assertEqual(slot.read(item.recovery_path)['lifecycle'], 'terminal')

    def test_child_attempt_is_durable_before_process_creation(self):
        with self.make() as item:
            def fail_creation(*args, **kwargs):
                journal = slot.read(item.recovery_path)
                self.assertEqual(journal['lifecycle'], 'child_starting')
                self.assertEqual(journal['child_start_attempts'], 1)
                raise OSError('creation failed')
            with patch.object(slot.subprocess, 'Popen', side_effect=fail_creation):
                with self.assertRaisesRegex(OSError, 'creation failed'):
                    item.spawn(['not-executed'])

    def test_failed_journal_write_prevents_process_creation(self):
        with self.make() as item:
            with patch.object(item, 'persist_recovery', side_effect=OSError('journal failed')), \
                    patch.object(slot.subprocess, 'Popen') as create:
                with self.assertRaisesRegex(OSError, 'journal failed'):
                    item.spawn(['not-executed'])
                create.assert_not_called()

    def test_consumed_handoff_cannot_repeat(self):
        with self.make():
            pass
        slot.atomic(self.request, self.state)
        with self.assertRaises(FileExistsError):
            self.make().__enter__()
        self.assertFalse(self.writer.exists())
        self.assertFalse(self.game.exists())

    def test_assignment_failure_kills_unassigned_child_handle(self):
        child = Mock(pid=123, _handle=456, returncode=1)
        child.poll.return_value = None
        child.kill.side_effect = lambda: setattr(child.poll, 'return_value', 1)
        with self.make() as item:
            with patch.object(slot.subprocess, 'Popen', return_value=child), \
                    patch.object(slot.win32job, 'AssignProcessToJobObject', side_effect=OSError('assign failed')):
                with self.assertRaisesRegex(OSError, 'assign failed'):
                    item.spawn(['not-executed'])
            child.kill.assert_called_once()
            child.wait.assert_called_once()
        self.assertTrue(item.receipt['terminal'])

    def test_unverified_live_root_retains_claims(self):
        item = self.make().__enter__()
        child = Mock(pid=123)
        child.poll.return_value = None
        item.children.append(child)
        item.total_deadline = time.monotonic() - 1
        self.assertFalse(item.release())
        self.assertTrue(all(slot.owns(p, item.token) for p in [self.lease, self.game, self.writer]))
        self.assertEqual(slot.read(self.lease)['status'], 'cleanup_failed')
        self.assertFalse(item.receipt['terminal'])

    def test_descendant_without_root_retains_claims(self):
        item = self.make().__enter__()
        item.total_deadline = time.monotonic() - 1
        with patch.object(item, 'job_pids', return_value={321}):
            self.assertFalse(item.release())
        self.assertEqual(item.receipt['remaining_owned_pids'], [321])
        self.assertTrue(self.game.exists())

    def test_foreign_claim_is_never_released(self):
        item = self.make().__enter__()
        foreign = {'claim_token': 'foreign', 'nonce': 'foreign', 'status': 'active'}
        for path in [self.lease, self.writer, self.game]:
            slot.atomic(path, foreign)
        item.release()
        for path in [self.lease, self.writer, self.game]:
            self.assertEqual(slot.read(path), foreign)

    def test_new_foreign_game_yields_and_preserves_user_process(self):
        with self.make() as item:
            user = Mock(pid=999, info={'name': 'Fallout4.exe', 'create_time': 1})
            self.processes.append(user)
            with self.assertRaisesRegex(RuntimeError, 'Foreign game'):
                item.gate(claimed=True)
            user.kill.assert_not_called()
        self.assertTrue(item.receipt['terminal'])

    def test_untimed_slot_shares_the_machine_without_touching_shared_claims(self):
        foreign = {'claim_token': 'foreign', 'nonce': 'foreign', 'status': 'reserved', 'project': 'Modifier'}
        for path in [self.lease, self.game]:
            slot.atomic(path, foreign)
        self.processes += [Mock(pid=901, info={'name': 'Fallout4VR.exe', 'create_time': 1}),
                           Mock(pid=902, info={'name': 'cl.exe', 'create_time': 1})]
        item = slot.Slot(self.handoff, 'isolated-test', request_path=self.request, targets=['test'], exclusive=False)
        with patch.object(slot.psutil, 'cpu_percent', return_value=95):
            with item:
                self.assertEqual(item.gate(claimed=True)['foreign_games_builds'], [])
                environment = item.child_environment({'DTL_BENCH_OWNED_LEASE': 'inherited'})
                self.assertEqual(environment['DTL_BENCH_COEXIST'], '1')
                self.assertNotIn('DTL_BENCH_OWNED_LEASE', environment)
                self.assertFalse(self.writer.exists())
        for path in [self.lease, self.game]:
            self.assertEqual(slot.read(path), foreign)
        self.assertTrue(item.receipt['terminal'])
        self.assertFalse(item.receipt['exclusive'])

    def test_untimed_slot_still_honors_user_priority_and_ram(self):
        slot.atomic(self.lease, {'status': 'released', 'user_priority': True})
        item = slot.Slot(self.handoff, 'isolated-test', request_path=self.request, targets=['test'], exclusive=False)
        with self.assertRaisesRegex(RuntimeError, 'User priority'):
            item.gate()
        slot.atomic(self.lease, {'status': 'released'})
        with patch.object(slot.psutil, 'virtual_memory', return_value=Mock(available=1 * 2**30)):
            with self.assertRaisesRegex(RuntimeError, 'RAM'):
                item.gate()

    def test_exclusive_slot_still_yields_to_foreign_builds(self):
        self.processes.append(Mock(pid=902, info={'name': 'cl.exe', 'create_time': 1}))
        with self.assertRaisesRegex(RuntimeError, 'Foreign game/build'):
            self.make().gate()
        environment = self.make().child_environment({'DTL_BENCH_COEXIST': '1'})
        self.assertNotIn('DTL_BENCH_COEXIST', environment)

    def test_pause_during_work_is_not_overwritten_by_cleanup(self):
        with self.make() as item:
            paused = slot.read(self.request)
            paused['status'] = 'paused_for_user_reboot'
            slot.atomic(self.request, paused)
            with self.assertRaisesRegex(RuntimeError, 'paused'):
                item.gate(claimed=True)
        self.assertEqual(slot.read(self.request)['status'], 'paused_for_user_reboot')


if __name__ == '__main__':
    unittest.main()
