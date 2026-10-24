"""Configure/build/test without changing system or game configuration."""
import argparse
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def clean_env():
    # MSBuild's .NET Framework host rejects duplicate Path/PATH entries.
    result = {}
    for key, value in os.environ.items():
        result[key.upper()] = value
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dependencies', type=Path, required=True,
                   help='vcpkg installed triplet directory containing share/mmio and share/spdlog')
    p.add_argument('--configure-only', action='store_true')
    p.add_argument('--workers',type=int,default=1,choices=[1,2])
    a = p.parse_args()
    env = clean_env()
    build = ROOT / 'build-release'
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build), '-G',
                    'Visual Studio 17 2022', '-A', 'x64',
                    '-DCMAKE_PREFIX_PATH='+str(a.dependencies.resolve()),
                    '-DBUILD_TESTING=ON'], cwd=ROOT, env=env, check=True)
    if not a.configure_only:
        subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', str(a.workers), '--','/nr:false'],
                       cwd=ROOT, env=env, check=True)
        subprocess.run(['ctest', '--test-dir', str(build), '-C', 'Release', '--output-on-failure'],
                       cwd=ROOT, env=env, check=True)


if __name__ == '__main__':
    main()
