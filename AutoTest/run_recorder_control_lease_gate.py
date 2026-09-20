"""Win32 production recorder control tests, without Ninja, DLL build or game.

Fresh artifacts only; compile and run serially BelowNormal. Reuse the configured
production TU flags for syntax checks, removing every output/dependency switch.
This is CPU/syntax proof, not GPU lifetime or player exit acceptance.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess

from run_render_host_protocol_gate import ROOT, identity, pe_identity


def split_windows(command):
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    count = ctypes.c_int()
    args = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not args:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return [args[i] for i in range(count.value)]
    finally:
        kernel.LocalFree(ctypes.cast(args, ctypes.c_void_p))


def syntax_arguments(args):
    result = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ('-o', '-MF', '-MT', '-MQ'):
            if i + 1 == len(args):
                raise ValueError('missing output argument')
            i += 2
            continue
        if arg in ('-MD', '-MMD', '-MP', '-c'):
            i += 1
            continue
        if arg.startswith(('-o', '-MF', '-MT', '-MQ')) or arg in ('-M', '-MM') or arg.startswith(('@', '-Wp,', '-save-temps')):
            raise ValueError('unsupported output/response switch: ' + arg)
        result.append(arg)
        i += 1
    return result + ['-fsyntax-only', '-fdiagnostics-color=never']


def invoke(args, cwd=ROOT, env=None, timeout=180):
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True,
        text=True, encoding='utf-8', errors='replace', timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exit={result.returncode}\n{result.stdout}\n{result.stderr}')
    return result.stdout.strip(), result.stderr.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler32.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='PRODUCTION_RECORDER_CONTROL_CPU_AND_SYNTAX', ok=False,
                   gameLaunched=False, shippingDllBuilt=False, gpuTested=False,
                   builds=[], runs=[], syntax=[], source=[])
    folder = ROOT / 'src/d3d9/war3/tools'
    source = sorted(set([*folder.glob('war3_frame_*.h'), *folder.glob('war3_frame_*.cpp'),
        ROOT / 'src/d3d9/war3/render/war3_skin_palette_selection.h',
        ROOT / 'build32/src/d3d9/war3_frame_recorder_build.h',
        ROOT / 'build32/compile_commands.json', ROOT / 'AutoTest/run_render_host_protocol_gate.py',
        Path(__file__).resolve(), *ROOT.glob('AutoTest/test_frame_recorder*.cpp'),
        ROOT / 'AutoTest/test_frame_evidence_runtime.cpp',
        ROOT / 'src/d3d9/war3/tools/war3_control_plane.cpp']))
    try:
        receipt['source'] = [identity(p) for p in source]
        receipt['compiler'] = identity(compiler)
        common = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wno-misleading-indentation',
                  '-static', '-pthread', '-DNOMINMAX', '-Iinclude', '-Ibuild32/src/d3d9']
        prod = 'src/d3d9/war3/tools/war3_frame_evidence.cpp'
        memory = 'src/d3d9/war3/tools/war3_frame_recorder_memory.cpp'
        cases = [
            ('session', ['AutoTest/test_frame_recorder_session.cpp'], [], [([], None)]),
            ('runtime', ['AutoTest/test_frame_evidence_runtime.cpp', prod, memory], [],
             [([], '1'), (['disabled'], '0')]),
            ('memory', ['AutoTest/test_frame_recorder_memory.cpp', memory], [], [([], None)]),
            ('memory-control', ['AutoTest/test_frame_recorder_memory.cpp', prod],
             ['-DWARVK_RECORDER_MEMORY_CONTROL_TEST=1'], [([], '1')])]
        for profile in (0, 1):
            cases.append((f'defaults-{profile}', ['AutoTest/test_frame_recorder_defaults.cpp', prod, memory],
                [], [([mode, str(profile)], None) for mode in
                     ('default', 'disabled', 'invalid', 'external', 'explicit-on')]))
        for name, sources, defines, runs in cases:
            profile = 1 if name == 'defaults-1' else 0
            exe = output / (name + '.exe')
            command = common + [f'-DWARVK_INTERNAL_FRAME_RECORDER_DEFAULT={profile}'] + defines + sources + ['-o', exe]
            stdout, stderr = invoke(command)
            if pe_identity(exe) != (0x14c, 0x10b):
                raise ValueError('not Win32 PE32: ' + name)
            receipt['builds'].append(dict(name=name, exe=identity(exe), stdout=stdout, stderr=stderr))
            for argv, enabled in runs:
                env = {k: v for k, v in os.environ.items() if not k.startswith('DXVK_WAR3_FRAME_')}
                env.update(DXVK_WAR3_FRAME_EVIDENCE_OUTPUT=str(output), DXVK_WAR3_FRAME_EVIDENCE_INPUTS='0')
                if enabled is not None:
                    env['DXVK_WAR3_FRAME_EVIDENCE'] = enabled
                stdout, stderr = invoke([exe, *argv], env=env, timeout=30)
                match = re.search(r'checks=(\d+).* PASS$', stdout)
                if not match or stderr:
                    raise ValueError(f'invalid native test output: {stdout!r} {stderr!r}')
                receipt['runs'].append(dict(name=name, args=argv, checks=int(match[1]), stdout=stdout))
            print('PASS ' + name, flush=True)
        entries = json.loads((ROOT / 'build32/compile_commands.json').read_text())
        for name in ('war3_frame_history.cpp', 'war3_frame_evidence.cpp', 'war3_control_plane.cpp', 'war3_frame_inputs.cpp'):
            matches = [e for e in entries if Path(e['file']).name == name and 'd3d9.dll.p' in e['command']]
            if len(matches) != 1:
                raise ValueError('non-unique production TU ' + name)
            entry = matches[0]
            command = syntax_arguments(split_windows(entry['command']))
            if Path(command[0]).resolve() != compiler or Path(entry['directory']).resolve() != ROOT / 'build32':
                raise ValueError('unexpected production toolchain/build directory')
            stdout, stderr = invoke(command, cwd=entry['directory'])
            receipt['syntax'].append(dict(name=name, command=command, stdout=stdout, stderr=stderr))
            print('SYNTAX ' + name, flush=True)
        if receipt['source'] != [identity(p) for p in source]:
            raise ValueError('source changed during proof')
        receipt['exports'] = [identity(p) for p in sorted(output.glob('cpu-*.json'))]
        receipt['ok'] = len(receipt['runs']) == 15 and len(receipt['syntax']) == 4
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps(dict(ok=receipt['ok'], runs=len(receipt['runs']), syntax=len(receipt['syntax']),
                         receipt=str(output / 'receipt.json'), error=receipt.get('error')), ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
