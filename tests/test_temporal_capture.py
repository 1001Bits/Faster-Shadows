import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/benchmark'))
from temporal import validate_sequence


class TemporalCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.folder=Path(self.temp.name)
        camera={'eye':[0,0,0],'rotation':[1,0,0,0,1,0,0,0,1],
                'world_to_cam':[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],'frustum':[-1,1,1,-1,.1,1000]}
        self.rows=[dict(type='metadata',schema=1,expected=8,qpc_frequency=1000,performance_valid=False)]
        for i in range(8):
            row=dict(type='frame',index=i,world_id=i+1,present_id=i+10,
                     world_begin_qpc=100*i+1,world_end_qpc=100*i+2,capture_begin_qpc=100*i+3,
                     capture_end_qpc=100*i+4,forward_qpc=100*i+5,returned_qpc=100*i+6,
                     file=f'frame-{i:03}.ppm',width=1,height=1,all_zero=False,hresult=0x087A0001,
                     camera_begin=copy.deepcopy(camera),camera_end=copy.deepcopy(camera),
                     cadence_known=True,enabled=1,cadence=1,planned_skip_mask=2 if i%2 else 4)
            self.rows.append(row);(self.folder/row['file']).write_bytes(b'P6\n1 1\n255\nabc')
        self.rows.append(dict(type='terminal',success=True,written=8,expected=8))

    def validate(self,**kwargs):
        (self.folder/'frames.jsonl').write_text('\n'.join(json.dumps(row) for row in self.rows)+'\n')
        return validate_sequence(self.folder,8,enabled=True,**kwargs)

    def test_complete_sequence_is_capture_valid_but_not_performance_or_quality_proof(self):
        result=self.validate()
        self.assertTrue(result['valid']);self.assertEqual(result['planned_reuse_frames'],8)
        self.assertFalse(result['performance_valid']);self.assertFalse(result['visual_quality_accepted'])
        self.assertEqual(len(result['frame_sha256']),8)

    def test_frame_gaps_duplicates_and_unpaired_timing_rejected(self):
        original=copy.deepcopy(self.rows)
        for key,value in [('world_id',99),('present_id',10),('world_begin_qpc',2),('capture_end_qpc',1)]:
            with self.subTest(key=key):
                self.rows=copy.deepcopy(original);self.rows[3][key]=value
                with self.assertRaises(ValueError):self.validate()

    def test_missing_partial_blank_and_escaped_frames_rejected(self):
        path=self.folder/'frame-003.ppm'
        for data in (b'',b'P6\n1 1\n255\na',b'P6\n1 1\n255\n\0\0\0'):
            path.write_bytes(data)
            with self.assertRaises(ValueError):self.validate()
        path.write_bytes(b'P6\n1 1\n255\nabc');self.rows[4]['file']='../frame-003.ppm'
        with self.assertRaises(ValueError):self.validate()

    def test_failed_terminal_and_invalid_camera_rejected(self):
        self.rows[-1]['success']=False
        with self.assertRaises(ValueError):self.validate()
        self.rows[-1]['success']=True;self.rows[2]['camera_end']['eye'][0]=float('nan')
        with self.assertRaises(ValueError):self.validate()

    def test_actual_camera_motion_required(self):
        for motion in ('pan','strafe'):
            with self.assertRaisesRegex(ValueError,'not observed'):self.validate(require_motion=motion)
        with self.assertRaisesRegex(ValueError,'walk not observed'):self.validate(require_motion='walk')
        self.rows[8]['camera_end']['eye'][0]=4
        self.assertEqual(self.validate(require_motion='strafe')['camera_travel_units'],4)
        self.assertEqual(self.validate(require_motion='walk')['camera_travel_units'],4)

    def test_unverified_enabled_state_and_near_cascade_reuse_rejected(self):
        for key,value in [('cadence_known',False),('enabled',0),('planned_skip_mask',1),('planned_skip_mask',6)]:
            old=self.rows[1][key];self.rows[1][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.validate()
            self.rows[1][key]=old

    def test_two_waiting_cascades_need_cadence_max_waiting_two(self):
        self.rows[1]['planned_skip_mask']=6
        with self.assertRaises(ValueError):self.validate()
        self.assertTrue(self.validate(max_waiting=2)['valid'])
        for mask in (1,7,16):
            self.rows[1]['planned_skip_mask']=mask
            with self.subTest(mask=mask),self.assertRaises(ValueError):self.validate(max_waiting=3)

    def test_missing_frame_row_rejected(self):
        self.rows.pop(3)
        with self.assertRaisesRegex(ValueError,'Incomplete'):self.validate()


if __name__=='__main__':unittest.main()
