#!/usr/bin/env python3
"""Build and run the CPU test of the product's emulated-TLS runtime.

Builds AutoTest/test_war3_emutls.cpp four ways with the x86 llvm-mingw compiler
named in build-win32-weather-llvm.txt (all -femulated-tls, -static):
payload.dll (with src/d3d9/war3/memory/war3_emutls.cpp), payload_rt.dll
(compiler-rt's runtime instead), late.dll and emutls_test.exe. Then:

  1. emutls_test.exe payload.dll: threads, thread exit, churn, last error,
     many variables, alignment, process-termination path, FreeLibrary path;
     again with --high-index (TLS slots 0..63 taken first, so the runtime's
     slot is in the TEB's expansion array, as in a game with many DLLs).
  2. --exit late.dll payload.dll: thread_locals touched after the payload's
     DLL_PROCESS_DETACH at process exit; must exit 0.
  3. --exit late.dll payload_rt.dll: the same with compiler-rt's runtime; must
     abort (0xC0000409), or the scenario would prove nothing.
  4. --exit-fresh late.dll payload.dll: the payload's TLS first touched during
     the exit (TLS index, thread storage and objects made then); must exit 0.
  5. llvm-nm: payload.dll carries no compiler-rt emutls object.

usage: run_war3_emutls_test.py --out <empty or new directory>
Prints one JSON line; exit 0 when everything holds.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CROSS = ROOT / 'build-win32-weather-llvm.txt'
TEST = ROOT / 'AutoTest' / 'test_war3_emutls.cpp'
RUNTIME = ROOT / 'src' / 'd3d9' / 'war3' / 'memory' / 'war3_emutls.cpp'
BELOW_NORMAL = 0x00004000
ABORT = 0xC0000409


def compiler():
    match = re.search(r"^cpp\s*=\s*'([^']+)'", CROSS.read_text(encoding='utf-8'), re.M)
    if not match:
        raise SystemExit(f'no cpp compiler in {CROSS}')
    return Path(match.group(1))


def run(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=600,
                          creationflags=BELOW_NORMAL, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    clang = compiler()
    nm = clang.parent / 'llvm-nm.exe'
    common = [clang, '-std=c++17', '-O2', '-femulated-tls', '-DNOMINMAX', '-Wall',
              '-I', ROOT / 'src', '-I', ROOT / 'src' / 'd3d9', '-static']
    builds = {
        'payload.dll': ['-shared', '-DWAR3_EMUTLS_PAYLOAD', TEST, RUNTIME],
        'payload_rt.dll': ['-shared', '-DWAR3_EMUTLS_PAYLOAD', '-DWAR3_EMUTLS_COMPILER_RT', TEST],
        'late.dll': ['-shared', '-DWAR3_EMUTLS_LATE', TEST],
        'emutls_test.exe': ['-municode', TEST, '-lpsapi'],
    }
    result = {'schema': 'warvk-emutls-cpu-test/v1', 'compiler': str(clang), 'ok': False}
    for name, extra in builds.items():
        built = run(common + extra + ['-o', out / name])
        if built.returncode:
            result['buildError'] = {name: built.stdout[-2000:] + built.stderr[-4000:]}
            print(json.dumps(result))
            return 1
    exe = out / 'emutls_test.exe'
    unit = run([exe, out / 'payload.dll'], cwd=out)
    match = re.search(r'RESULT (\{.*\})', unit.stdout)
    result['unit'] = json.loads(match.group(1)) if match else None
    result['unitExit'] = unit.returncode
    result['unitLog'] = [line for line in unit.stdout.splitlines() if not line.startswith('RESULT')]
    high = run([exe, '--high-index', out / 'payload.dll'], cwd=out)
    match = re.search(r'RESULT (\{.*\})', high.stdout)
    result['unitHighIndex'] = json.loads(match.group(1)) if match else None
    result['unitHighIndexExit'] = high.returncode
    result['unitHighIndexLog'] = [line for line in high.stdout.splitlines() if not line.startswith('RESULT')]
    ours = run([exe, '--exit', out / 'late.dll', out / 'payload.dll'], cwd=out)
    control = run([exe, '--exit', out / 'late.dll', out / 'payload_rt.dll'], cwd=out)
    fresh = run([exe, '--exit-fresh', out / 'late.dll', out / 'payload.dll'], cwd=out)
    result['exitOurs'] = ours.returncode & 0xFFFFFFFF
    result['exitOursFresh'] = fresh.returncode & 0xFFFFFFFF
    result['exitCompilerRt'] = control.returncode & 0xFFFFFFFF
    result['compilerRtStderr'] = control.stderr.strip()[-200:]

    def symbols(path):
        listed = run([nm, path])
        return listed.stdout
    ours_nm, rt_nm = symbols(out / 'payload.dll'), symbols(out / 'payload_rt.dll')
    result['symbols'] = {
        'oursHasGetAddress': '___emutls_get_address' in ours_nm,
        'oursHasCompilerRtExit': '_emutls_exit' in ours_nm,
        'oursHasTlsCallback': '_war3_emutls_thread_exit' in ours_nm,
        'compilerRtHasExit': '_emutls_exit' in rt_nm,
    }
    s = result['symbols']
    result['ok'] = bool(result['unit'] and result['unit'].get('ok') and unit.returncode == 0
                        and result['unitHighIndex'] and result['unitHighIndex'].get('ok') and high.returncode == 0
                        and result['exitOurs'] == 0 and result['exitCompilerRt'] == ABORT
                        and result['exitOursFresh'] == 0
                        and s['oursHasGetAddress'] and s['oursHasTlsCallback']
                        and not s['oursHasCompilerRtExit'] and s['compilerRtHasExit'])
    print(json.dumps(result))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
