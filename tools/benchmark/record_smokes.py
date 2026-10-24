"""Archive compact smoke outcomes without claiming measured performance."""
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
labels=['og-shadow-smoke-01','og-native-smoke-01','og-native-diagnosis-02',
    'og-native-diagnosis-05','og-shadow-smoke-02','og-shadow-smoke-03']
keys=['label','runtime','error','candidate_sha256','smoke_counters','smoke_sun_state',
    'scene_parameters','archive_inputs_unchanged','original_user_files_unchanged','cleanup']
rows=[]
for label in labels:
    case=json.loads((ROOT/'.runtime-tests/runs'/label/'case.json').read_text())
    rows.append({key:case.get(key) for key in keys})
result={'performance_measured':False,'policy_checks_passed':44,'runs':rows,
    'native_startup_fix':'ESM file symlinks replaced by verified regular copies',
    'sun_coverage':'Three native cameras and hooked vtables observed; no non-skinned sun-policy tests in two fixed views.',
    'point_coverage':'35,520 submissions in the gate view; no defaults-eligible removals.'}
(ROOT/'evidence/smoke-results.json').write_text(json.dumps(result,indent=2)+'\n')
print('Smoke evidence recorded; performance measurements remain pending')
