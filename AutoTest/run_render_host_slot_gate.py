"""Compile and exercise the real RH1 slot ownership core on Win32 and Win64.

No shared mapping, game, renderer, Vulkan device, deployment or source mutation.
Small conservative counter limits test exhaustion in the same actual core.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke


def golden():
    return struct.pack('<4sHH6QII', b'WVL1', 1, 64,
                       0x0807060504030201, 0x1817161514131211, 3, 7, 9, 15, 2, 65536)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'RH1_ACTUAL_SLOT_CORE_ONLY', 'ok': False,
               'sharedMappingTested': False, 'ipcTransportTested': False,
               'gpuTested': False, 'gameLaunched': False, 'shippingDllBuilt': False,
               'source': [], 'runs': []}
    paths = [ROOT / name for name in ('tools/render_host/protocol.h', 'tools/render_host/protocol.cpp',
             'tools/render_host/slot_ledger.h', 'tools/render_host/slot_ledger.cpp',
             'AutoTest/test_render_host_slot_ledger.cpp', 'AutoTest/run_render_host_slot_gate.py',
             'AutoTest/run_render_host_protocol_gate.py')]
    try:
        receipt['source'] = [identity(path) for path in paths]
        for bits, compiler in ((32, args.compiler32), (64, args.compiler64)):
            compiler = compiler.resolve(strict=True)
            exe = output / f'rh1-slot-core-{bits}.exe'
            stdout, stderr = invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                                    'tools/render_host/protocol.cpp', 'tools/render_host/slot_ledger.cpp',
                                    'AutoTest/test_render_host_slot_ledger.cpp', '-o', exe], 120)
            expected = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            if pe_identity(exe) != expected:
                raise ValueError('PE architecture does not match claimed width')
            result, errors = invoke([exe], 30)
            match = re.fullmatch(r'RH1 slot/core checks=(\d+) bits=(32|64) positiveRecords=10000 PASS', result)
            if not match or int(match[2]) != bits or errors:
                raise ValueError(f'invalid native test result: {result!r} {errors!r}')
            vector, errors = invoke([exe, '--golden'], 10)
            if errors or bytes.fromhex(vector) != golden():
                raise ValueError('descriptor differs from independent Python byte encoding')
            receipt['runs'].append({'bits': bits, 'compiler': identity(compiler), 'exe': identity(exe),
                'checks': int(match[1]), 'positiveRecords': 10000, 'conservativeCounterLimitsTested': True,
                'goldenBytes': len(golden()), 'goldenSha256': hashlib.sha256(golden()).hexdigest(),
                'compilerStdout': stdout, 'compilerStderr': stderr})
        if receipt['source'] != [identity(path) for path in paths]:
            raise ValueError('source changed during proof')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'checks': [r['checks'] for r in receipt['runs']], 'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
