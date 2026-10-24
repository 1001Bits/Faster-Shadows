"""Package the checked experimental DLL, settings, source, and license notices."""
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
from provenance import verify_sources

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.0'


def package():
    dll = ROOT/'build-release/Release/FasterShadows.dll'
    verification = json.loads((ROOT/'evidence/verification.json').read_text())
    validation = json.loads((ROOT/'evidence/build-validation.json').read_text())
    if validation.get('schema')!=2 or validation.get('version')!=VERSION or validation.get('passed') is not True:
        raise RuntimeError(f'A fresh successful {VERSION} build/test record is required')
    verify_sources(validation['source_sha256'])
    if validation['dll_sha256']!=verification['dll']['sha256']:
        raise RuntimeError('Build/test and ABI evidence describe different DLLs')
    if hashlib.sha256(dll.read_bytes()).hexdigest()!=verification['dll']['sha256']:
        raise RuntimeError('DLL differs from the statically verified build; run verify_port.py again')
    dist = ROOT/'dist'
    dist.mkdir(exist_ok=True)
    binary = dist/f'FasterShadows-{VERSION}.zip'
    with ZipFile(binary,'w',ZIP_DEFLATED) as z:
        z.write(dll,'F4SE/Plugins/'+dll.name)
        z.write(ROOT/'package/FasterShadows.ini','F4SE/Plugins/FasterShadows.ini')
        for path in sorted((ROOT/'package/MCM').rglob('*')):
            if path.is_file():
                z.write(path,path.relative_to(ROOT/'package').as_posix())
        for name in ['README.md','LICENSE','EXCEPTIONS.md','THIRD_PARTY.md','docs/PORTING_ANALYSIS.md',
                     'docs/BENCHMARK_METHOD.md','docs/PERFORMANCE_RESULTS.md']:
            z.write(ROOT/name,'docs/FasterShadows/'+name)
        for path in sorted((ROOT/'licenses').glob('*')):
            z.write(path,'docs/FasterShadows/licenses/'+path.name)
    source = dist/f'FasterShadows-{VERSION}-source.zip'
    current = Path(validation['evidence']).resolve()
    def shipped(path):
        # Session handovers and research notes stay local, and so do the evidence folders of earlier
        # builds; the top-level records and this build's own evidence ship.
        if path.parent==ROOT/'docs' and path.name.startswith(('HANDOVER','AUDIT_FIXES','LATEST_RESEARCH')):
            return False
        inside = path.relative_to(ROOT).parts
        return not (inside[0]=='evidence' and len(inside)>2 and not path.resolve().is_relative_to(current))
    with ZipFile(source,'w',ZIP_DEFLATED) as z:
        for name in ['CMakeLists.txt','.gitignore','README.md','LICENSE','EXCEPTIONS.md','THIRD_PARTY.md']:
            z.write(ROOT/name,name)
        for folder in ['src','tests','tools','docs','package','evidence','licenses']:
            for path in sorted((ROOT/folder).rglob('*')):
                # API responses contain third-party descriptions/comments. Keep
                # research caches local; ship our bounded, attributed analysis.
                if path.is_file() and '__pycache__' not in path.parts and 'latest-20261011' not in path.parts and shipped(path):
                    z.write(path,path.relative_to(ROOT))
        # Corresponding source includes the exact statically linked library.
        for path in sorted((ROOT/'external/CommonLibF4').rglob('*')):
            if path.is_file() and '.git' not in path.parts:
                z.write(path,path.relative_to(ROOT))
        for path in sorted((ROOT/'external/dependencies').rglob('*')):
            if path.is_file() and '.git' not in path.parts:
                z.write(path,path.relative_to(ROOT))
    files = [{'file':p.name,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in [binary,source]]
    benchmarks = dist/f'FasterShadows-{VERSION}-benchmarks.zip'
    with ZipFile(benchmarks,'w',ZIP_DEFLATED) as z:
        z.write(ROOT/'docs/PERFORMANCE_RESULTS.md','PERFORMANCE_RESULTS.md')
        z.write(ROOT/'docs/BENCHMARK_METHOD.md','BENCHMARK_METHOD.md')
        for path in sorted((ROOT/'evidence').glob('*')):
            if path.is_file() and (path.name.startswith('benchmark-') or path.name=='performance-results.json'):
                z.write(path,'evidence/'+path.name)
        z.writestr('SCOPE.txt',f'{VERSION}: the in-game timing and visual results are in PERFORMANCE_RESULTS.md (lab runs of 2026-10-22 to 24); '
                   'this archive adds the helper CPU microbenchmarks, test records and the validation evidence of this build.\n')
        folder=Path(validation['evidence'])
        if not folder.resolve().is_relative_to((ROOT/'evidence').resolve()):raise RuntimeError('Evidence folder must be workspace owned')
        for path in sorted(folder.rglob('*')):
            if path.is_file() and path.suffix.lower() in {'.json','.log'}:
                z.write(path,'current-build/'+path.relative_to(folder).as_posix())
    files.append({'file':benchmarks.name,'bytes':benchmarks.stat().st_size,
                  'sha256':hashlib.sha256(benchmarks.read_bytes()).hexdigest()})
    (dist/'manifest.json').write_text(json.dumps(files,indent=2)+'\n')
    print(json.dumps(files,indent=2))


if __name__=='__main__':
    package()
