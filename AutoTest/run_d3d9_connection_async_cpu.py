"""Exact development Connection CPU gate on non-input Win32 Desktops.

Compile x86/x64 sequentially; run synthetic-reply tests with the real queue,
admission and barriers. This is NOT COM, IPC, GPU, or game acceptance.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_core_gate import execute
from run_render_host_transport_gate import require, strict_json
from run_render_host_presenter_gate import check_launch
from run_render_host_d3d9_gate import json_lines, native_uint
from render_lab_raw_artifacts import capture_raw_artifacts, raw_manifests, verify_raw_artifacts
from isolated_render_desktop import IsolatedRenderDesktop
from diagnose_render_host_initialization import ENV_ALLOW

COMPILERS = Path('E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/llvm-mingw-20260616-ucrt-x86_64/bin')
WORKERS = {'readback-backpressure', 'clear-failure-waiting-barrier', 'draw-own-reply',
    'slot-release-before-ack', 'empty-close', 'empty-fault', 'stale-hint-then-fault',
    'settled-reply-before-fault', 'bounded-draw-batch','gpu-group-end-scene','bounded-fixed-batch',
    'annotated-draw-batch'}


def read_result(text, launch, desktop, input_desktop, bits):
    require(type(bits) is int and bits in (32, 64), 'exact x86/x64 test architecture')
    require(type(text) is str and text.endswith('\n'), 'complete CPU raw text required')
    lines = text.splitlines()
    witnesses = []
    for line in lines:
        if 'warvk-x1a-cpu-desktop' in line:
            require(line.startswith('{'), 'malformed CPU Desktop witness')
            row = strict_json(line)
            require(row['schema'] == 'warvk-x1a-cpu-desktop/v1', 'exact witness version')
            witnesses.append(row)
    require(len(witnesses) == len(WORKERS)+1, 'owner and exact CPU worker desktop witness population')
    require(witnesses[0]['role'] == 'owner' and witnesses[0]['tid'] == launch['initialThreadId'], 'actual initial owner thread')
    require({r.get('case') for r in witnesses[1:]} == WORKERS
            and all(r['role'] == 'worker' for r in witnesses[1:]), 'exact CPU worker cases')
    for row in witnesses:
        keys = {'schema', 'role', 'pid', 'tid', 'bits', 'desktop', 'inputDesktop', 'isolatedDesktop', 'gpuTested'}
        require(set(row) == (keys | {'case'} if row['role'] == 'worker' else keys), 'exact CPU witness fields')
        require(type(row['pid']) is int and row['pid'] == launch['pid']
                and native_uint(row['tid'], 32) and row['tid'] > 0 and type(row['bits']) is int and row['bits'] == bits,
                'exact process/thread/bitness witness')
        require(row['desktop'] == desktop and row['inputDesktop'] == input_desktop
                and row['isolatedDesktop'] == 'WinSta0\\'+desktop and desktop.casefold() != input_desktop.casefold()
                and desktop.lower() != 'default' and row['gpuTested'] is False, 'actual CPU isolation witness')
        require(row['role'] != 'worker' or row['tid'] != launch['initialThreadId'], 'worker cannot be the live owner thread')
    completed = re.findall(r'^X1A_CPU checks=([1-9][0-9]*) ipcTested=0 gpuTested=0 productOwnerComplete=0$', text, re.M)
    require(len(completed) == 1 and int(completed[0]) >= 300, 'one nonvacuous completed notification CPU test')
    require(lines[-1] == f'X1A_CPU checks={completed[0]} ipcTested=0 gpuTested=0 productOwnerComplete=0',
            'completion must follow all desktop witnesses')
    return dict(checks=int(completed[0]), desktopWitnesses=witnesses)


def source_set():
    paths = {p for p in (ROOT/'tools/render_host').rglob('*.h')}
    paths.add(ROOT/'tools/render_host/protocol.cpp')
    paths.update(ROOT/'AutoTest'/name for name in ('test_d3d9_connection_async_cpu.cpp','test_render_host_d3d9_wire.cpp',
        'run_d3d9_connection_async_cpu.py', 'test_d3d9_connection_async_cpu_reader.py'))
    paths.update(Path(m.__file__).resolve() for m in list(sys.modules.values()) if getattr(m, '__file__', None)
        and Path(m.__file__).suffix == '.py' and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
    return sorted(paths)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args(); out = args.output.resolve()
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh artifact directory')
    out.mkdir()
    receipt = dict(schema='warvk-x1a-cpu-gate/v1', ok=False, ipcTested=False, gpuTested=False,
        gameTouched=False, productOwnerComplete=False, steps=[], stepRawArtifacts=[], cases=[], wireCases=[], priority='BelowNormal')
    frozen = []
    env = {k:v for k,v in os.environ.items() if k.upper() in ENV_ALLOW}
    try:
        frozen = [identity(p) for p in source_set()]
        receipt['sources'] = list(frozen)
        for bits, arch in ((32, 'i686'), (64, 'x86_64')):
            compiler = COMPILERS/(arch+'-w64-mingw32-clang++.exe')
            frozen.append(identity(compiler))
            exe = out/f'x1a-cpu-{bits}.exe'; stem = f'build-{bits}'
            argv = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread', '-municode',
                    '-I'+str(ROOT/'tools/render_host'), ROOT/'AutoTest/test_d3d9_connection_async_cpu.cpp',
                    ROOT/'tools/render_host/protocol.cpp',
                    '-o', exe, '-luser32']
            try:
                receipt['steps'].append(execute(argv, ROOT, out/stem, 120, env))
            finally:
                receipt['stepRawArtifacts'].append(capture_raw_artifacts(out, [stem+'.stdout', stem+'.stderr']))
            require(pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'test architecture')
            frozen.append(identity(exe))
            wire=out/f'd3d9-wire-{bits}.exe';stem=f'build-wire-{bits}'
            argv=[compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
                  ROOT/'AutoTest/test_render_host_d3d9_wire.cpp',ROOT/'tools/render_host/protocol.cpp','-o',wire]
            try:receipt['steps'].append(execute(argv,ROOT,out/stem,120,env))
            finally:receipt['stepRawArtifacts'].append(capture_raw_artifacts(out,[stem+'.stdout',stem+'.stderr']))
            require(pe_identity(wire)==((0x14c,0x10b) if bits==32 else (0x8664,0x20b)),'wire architecture')
            frozen.append(identity(wire))
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            for bits in (32, 64):
                case_dir = out/f'cpu-{bits}'; case_dir.mkdir()
                row = dict(bits=bits, ok=False); receipt['cases'].append(row)
                try:
                    row['process'] = desktop.run([out/f'x1a-cpu-{bits}.exe', '--expect-desktop', desktop.desktop_name,
                        '--input-desktop', desktop.input_before], cwd=case_dir,
                        stdout_path=case_dir/'case.stdout', stderr_path=case_dir/'case.stderr', timeout_s=30, env=env)
                finally:
                    row['initialRawArtifacts'] = capture_raw_artifacts(case_dir, ['case.stdout', 'case.stderr'])
                check_launch(row['process'], desktop.desktop_name, desktop.input_before)
                row.update(read_result((case_dir/'case.stdout').read_text('utf-8'), row['process'],
                    desktop.desktop_name, desktop.input_before, bits))
                row['ok'] = True
                wire_dir=out/f'wire-{bits}';wire_dir.mkdir()
                wire=dict(bits=bits,ok=False);receipt['wireCases'].append(wire)
                try:
                    wire['process']=desktop.run([out/f'd3d9-wire-{bits}.exe'],cwd=wire_dir,
                        stdout_path=wire_dir/'case.stdout',stderr_path=wire_dir/'case.stderr',timeout_s=30,env=env)
                finally:wire['initialRawArtifacts']=capture_raw_artifacts(wire_dir,['case.stdout','case.stderr'])
                check_launch(wire['process'],desktop.desktop_name,desktop.input_before)
                match=re.fullmatch(r'd3d9 wire checks=([1-9][0-9]*) failures=0 bits='+str(bits)+r' drawBytes=1164\s*',
                                   (wire_dir/'case.stdout').read_text('utf-8'))
                require(match is not None and int(match[1])>=1300,'complete D3D9 wire CPU tests')
                wire.update(ok=True,checks=int(match[1]))
        require(receipt['desktopSafety']['ok'] is True, 'isolated CPU cleanup')
        receipt['ok'] = len(receipt['cases']) == 2 and all(r['ok'] for r in receipt['cases'])
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            require(all(identity(Path(r['path'])) == r for r in frozen), 'input/output identity drift')
            receipt['preserved'] = frozen; receipt['inputsPreserved'] = bool(frozen)
            for raw in raw_manifests(receipt):
                verify_raw_artifacts(raw)
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('ok', 'error', 'preservationError', 'inputsPreserved')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
