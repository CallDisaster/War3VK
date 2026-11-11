"""Actual x86/x64 tests of the bounded render-command queue candidate.

This is an in-process CPU component gate, not a product transport worker, IPC,
GPU completion, game-memory benchmark or a replacement for D3D9 compatibility.
"""
import argparse
import json
from pathlib import Path
import re
import sys

from run_render_host_protocol_gate import ROOT, identity, invoke, pe_identity
from run_render_host_transport_gate import require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'X1_RENDER_COMMAND_QUEUE_CPU_COMPONENT', 'ok': False,
               'productIntegrated': False, 'transportWorkerImplemented': False,
               'ipcTested': False, 'gpuCompletionProved': False,
               'gameLaunched': False, 'shippingDllBuilt': False, 'runs': []}
    paths = [ROOT / p for p in ('tools/render_host/render_command_queue.h',
        'AutoTest/test_render_command_queue.cpp', 'AutoTest/run_render_command_queue_gate.py',
        'AutoTest/run_render_host_protocol_gate.py', 'AutoTest/run_render_host_transport_gate.py')]
    try:
        receipt['sources'] = [identity(p) for p in paths]
        for bits, compiler in ((32, args.compiler32), (64, args.compiler64)):
            compiler = compiler.resolve(strict=True)
            exe = output / f'render-command-queue-{bits}.exe'
            command = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-static', '-pthread', ROOT/'AutoTest/test_render_command_queue.cpp', '-o', exe]
            stdout, stderr = invoke(command, 180)
            (output/f'build-{bits}.stdout').write_text(stdout, encoding='utf-8')
            (output/f'build-{bits}.stderr').write_text(stderr, encoding='utf-8')
            expected = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            require(pe_identity(exe) == expected, 'queue test executable architecture mismatch')
            run = {'bits': bits, 'compiler': identity(compiler), 'exe': identity(exe),
                   'command': list(map(str, command)), 'repeats': []}
            for repetition in range(3):
                stdout, stderr = invoke([exe], 30)
                match = re.fullmatch(r'render command queue checks=(\d+) failures=0(?: [A-Za-z][A-Za-z0-9_]*=\d+)*', stdout)
                require(match and int(match[1]) >= 20 and not stderr,
                        f'queue test did not provide a nonvacuous clean result: {stdout!r} {stderr!r}')
                run['repeats'].append({'repetition': repetition+1, 'checks': int(match[1]),
                                       'stdout': stdout, 'stderr': stderr})
            receipt['runs'].append(run)
        require(receipt['sources'] == [identity(p) for p in paths], 'queue sources changed during gate')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2, ensure_ascii=False)
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output/'receipt.json'),
                      'widths': len(receipt['runs']), 'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
