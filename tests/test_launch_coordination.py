"""The private host's launch check: shared reservations block exclusive runs, not untimed ones."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'launch_coordination', Path(__file__).resolve().parents[1] / '.runtime-tests/support/launch_coordination.py')
coordination = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coordination)


class LaunchCoordinationTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.lab = Path(folder.name)
        self.lease = self.lab / 'lease.json'
        later = (datetime.now(timezone.utc) + timedelta(hours=1)).isoformat()
        self.lease.write_text(json.dumps({'id': 'release-test', 'status': 'reserved', 'expires': later}), encoding='utf-8')
        (self.lab / 'launch-coordination.json').write_text(
            json.dumps({'schema': 1, 'reservation_files': [str(self.lease)]}), encoding='utf-8')
        environment = patch.dict(os.environ, {}, clear=False)
        environment.start()
        self.addCleanup(environment.stop)
        for key in (coordination.COEXIST_ENV, coordination.OWNED_LEASE_ENV):
            os.environ.pop(key, None)

    def test_foreign_reservation_blocks_an_exclusive_run(self):
        result = coordination.check(self.lab)
        self.assertFalse(result['available'])
        self.assertIn('reserved', result['issues'][0])

    def test_untimed_run_launches_alongside_a_foreign_reservation(self):
        os.environ[coordination.COEXIST_ENV] = '1'
        result = coordination.check(self.lab)
        self.assertTrue(result['available'])
        self.assertTrue(result['coexist'])

    def test_untimed_run_still_honors_the_local_hold_and_cannot_own_the_lease(self):
        os.environ[coordination.COEXIST_ENV] = '1'
        (self.lab / 'launch-hold.json').write_text('{}', encoding='utf-8')
        self.assertFalse(coordination.check(self.lab)['available'])
        (self.lab / 'launch-hold.json').unlink()
        os.environ[coordination.OWNED_LEASE_ENV] = '{}'
        self.assertFalse(coordination.check(self.lab)['available'])


if __name__ == '__main__':
    unittest.main()
