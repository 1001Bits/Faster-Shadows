"""Bind verification and test evidence to the exact current compilation inputs."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_inputs():
    paths = [ROOT / 'CMakeLists.txt', ROOT / 'package/FasterShadows.ini']
    for folder in ('src', 'tests', 'tools', 'external/CommonLibF4', 'external/dependencies'):
        paths.extend(path for path in (ROOT / folder).rglob('*')
                     if path.is_file() and not any(part in {'.git', '__pycache__'} for part in path.parts))
    return {path.relative_to(ROOT).as_posix(): sha(path) for path in sorted(set(paths))}


def verify_sources(recorded):
    if recorded != source_inputs():
        raise RuntimeError('Current source/dependency inputs differ from the tested build')
