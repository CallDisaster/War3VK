"""Build/test the real RH0 CPU codec on both widths; never builds a WarVK DLL.

Fresh output directory is required. No deployment, Vulkan device, game launch,
network connection, automatic toolchain download or PATH mutation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def identity(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': h.hexdigest().upper()}

def pe_identity(path):
    with path.open('rb') as stream:
        dos = stream.read(64)
        if len(dos) != 64 or dos[:2] != b'MZ':
            raise ValueError('not PE')
        stream.seek(struct.unpack_from('<I', dos, 60)[0])
        pe = stream.read(26)
    if len(pe) != 26 or pe[:4] != b'PE\0\0':
        raise ValueError('invalid PE header')
    return struct.unpack_from('<H', pe, 4)[0], struct.unpack_from('<H', pe, 24)[0]

def invoke(argv, timeout):
    flags = 0
    if os.name == 'nt':
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    result = subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True,
                            text=True, encoding='utf-8', errors='replace',
                            timeout=timeout, creationflags=flags)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} exited {result.returncode}: {result.stdout}\n{result.stderr}')
    return result.stdout.strip(), result.stderr.strip()

def golden():
    return struct.pack('<4sHHHHIII8QII', b'WVK0', 0, 1, 96, 1, 0, 24, 0,
                       0x0807060504030201, 0x1817161514131211,
                       1, 0, 0, 0, 0, 0, 0, 0) + struct.pack('<IIQQ', 32, 64, 1, 1)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    compilers = {32: args.compiler32.resolve(strict=True), 64: args.compiler64.resolve(strict=True)}
    output = args.output.resolve()
    # This tool cannot recursively clean/replace previous proof.
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'RH0_CPU_CODEC_ONLY', 'ok': False,
               'ipcTransportTested': False, 'gpuTested': False, 'gameLaunched': False,
               'shippingDllBuilt': False, 'source': [], 'runs': []}
    try:
        for relative in ('tools/render_host/protocol.h', 'tools/render_host/protocol.cpp',
                         'AutoTest/test_render_host_protocol.cpp', 'AutoTest/run_render_host_protocol_gate.py'):
            receipt['source'].append(identity(ROOT / relative))
        for bits, compiler in compilers.items():
            exe = output / f'rh0-protocol-{bits}.exe'
            _, warnings = invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                                  '-static', 'tools/render_host/protocol.cpp',
                                  'AutoTest/test_render_host_protocol.cpp', '-o', exe], 120)
            expected_pe = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            if pe_identity(exe) != expected_pe:
                raise ValueError(f'{bits}-bit requested but PE identity differs')
            result, stderr = invoke([exe], 30)
            match = re.fullmatch(r'RH0 codec/session checks=(\d+) bits=(32|64) PASS', result)
            if not match or int(match[2]) != bits or stderr:
                raise ValueError(f'Unexpected test output: {result!r} {stderr!r}')
            vector, stderr = invoke([exe, '--golden'], 10)
            if stderr or bytes.fromhex(vector) != golden():
                raise ValueError('Native wire bytes differ from independent Python struct vector')
            receipt['runs'].append({'bits': bits, 'compiler': identity(compiler),
                                    'exe': identity(exe), 'checks': int(match[1]),
                                    'goldenBytes': len(golden()), 'goldenSha256': hashlib.sha256(golden()).hexdigest(),
                                    'compilerStderr': warnings})
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'checks': [r['checks'] for r in receipt['runs']],
                      'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1

if __name__ == '__main__':
    sys.exit(main())
