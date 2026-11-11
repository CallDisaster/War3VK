"""RH2: actual x86 -> x64 Vulkan indexed draw / external image GPU lab.

Not a shipping DLL, D3D9 proxy, game memory benchmark or presentation path.
No CPU image crosses IPC. Client readback is a test oracle only. Fresh output,
explicit compilers, BelowNormal serial builds, exact owned child + watchdog.
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
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import strict_json, require, child_settled

CASES = ('normal', 'bad-generation', 'duplicate-frame', 'bad-nonce', 'wrong-adapter')


def golden():
    return struct.pack('<4sHHHHIII8QII', b'WVG2', 1, 0, 96, 1, 0, 24, 0,
                       0x0807060504030201, 0x1817161514131211,
                       1, 0, 0, 0, 0, 0, 0, 0) + struct.pack('<IIQQ', 32, 64, 8, 8)


def oracle_image_hash(width, height, variant):
    """Independent full-frame raster oracle for the fixed non-edge-aligned quad.

    Pixel centres are strictly inside or outside all four edges for these extents.
    No shader implementation, GPU pixel result or host hash feeds this oracle.
    """
    colors = ((255, 32, 16, 255), (16, 255, 32, 255),
              (32, 16, 255, 255), (255, 255, 16, 255))
    result = 14695981039346656037
    for y in range(height):
        for x in range(width):
            inside = abs(2*(x+0.5)/width-1) < 0.65 and abs(2*(y+0.5)/height-1) < 0.65
            for b in colors[variant] if inside else (4, 8, 16, 255):
                result = ((result ^ b) * 1099511628211) & ((1 << 64)-1)
    return f'{result:016x}'


def check_case(name, client, host):
    require(client['case'] == name and client['bits'] == 32 and host['bits'] == 64, 'process width/case')
    require(client['peerVerified'] and host['peerVerified'], 'peer identity')
    require(not client['failure'] and not host['failure'], 'native failure')
    require(client['childPid'] > 0 and client['childCreation'] > 0, 'exact child identity')
    normal = name in ('normal', 'wrong-adapter')
    require(client['childExit'] == (0 if normal else 2) and host['closed'] == normal, 'exit/close')
    expected = {'normal': 'None', 'bad-generation': 'Scope', 'duplicate-frame': 'Frame',
                'bad-nonce': 'Nonce', 'wrong-adapter': 'None'}[name]
    require(host['protocol'] == expected, 'wrong rejection reason')
    require(client['adapterMismatchRejected'] == (name == 'wrong-adapter'), 'adapter rejection reason')
    require(host['accepted'] == (38 if name == 'normal' else 4), 'accepted command population')
    require(client['exchanges'] == host['accepted'], 'acknowledged command population')
    count = 16 if name == 'normal' else (0 if name == 'wrong-adapter' else 1)
    require(len(client['frames']) == count, 'frame population')
    require(sum(x['drawCount'] for x in host['epochs']) == count, 'host actual draw population')
    require(sum(x['readbackCount'] for x in client['imports']) == count, 'client GPU readback population')
    require(sum(x['consumerWaitCount'] for x in host['epochs']) == count, 'host GPU consumer completion')
    require(len(host['epochs']) == (2 if name == 'normal' else 1), 'host epoch population')
    require(len(client['imports']) == (2 if name == 'normal' else (0 if name == 'wrong-adapter' else 1)),
            'import epoch population')
    for epoch in host['epochs']:
        expected_draws = 8 if name == 'normal' else count
        require(epoch['drawCount'] == epoch['consumerWaitCount'] == expected_draws, 'per-epoch GPU work')
        require(not epoch['faulted'], 'faulted host cannot certify protocol rejection')
        # Exactly one initial geometry upload, then one render and one external
        # consumer settlement submission per frame. Bad commands add none.
        require(epoch['submitCount'] == epoch['fenceWaitCount'] == 1 + 2 * expected_draws,
                'unexpected host GPU submissions/completion witnesses')
    for imported in client['imports']:
        expected_reads = 8 if name == 'normal' else count
        require(not imported['faulted'], 'faulted importer')
        require(imported['readbackCount'] == imported['submitCount'] == imported['fenceWaitCount'] == expected_reads,
                'unexpected importer GPU submissions/completion witnesses')
    for i, frame in enumerate(client['frames']):
        require(frame['epoch'] == i // 8 + 1 and frame['frame'] == i % 8 + 1, 'frame order/scope')
        require(frame['variant'] == i % 4, 'variant order')
        expected_pixel = ((255, 32, 16, 255), (16, 255, 32, 255),
                          (32, 16, 255, 255), (255, 255, 16, 255))[i % 4]
        require(len(frame['center']) == 4 and len(frame['corner']) == 4, 'pixel channel count')
        require(all(type(a) is int and abs(a-b) <= 1 for a, b in zip(frame['center'], expected_pixel)), 'center oracle')
        require(all(type(a) is int and abs(a-b) <= 1 for a, b in zip(frame['corner'], (4, 8, 16, 255))), 'corner oracle')
        require(re.fullmatch('[0-9a-f]{16}', frame['fnv64']) is not None, 'image digest shape')
        width, height = (64, 64) if frame['epoch'] == 1 else (96, 80)
        require(frame['fnv64'] == oracle_image_hash(width, height, frame['variant']), 'independent full-image oracle mismatch')
    if name == 'normal':
        require(len(host['epochs']) == len(client['imports']) == 2, 'epoch recreation')
        for epoch, imported in zip(host['epochs'], client['imports']):
            require(epoch['drawCount'] == imported['readbackCount'] == epoch['consumerWaitCount'] == 8, 'per epoch work')
            require(epoch['deviceUUID'] == imported['deviceUUID'] and epoch['driverUUID'] == imported['driverUUID'], 'actual same GPU/driver')
            require(epoch['sharedImageBytes'] == imported['sharedImageBytes'] > 0, 'one shared payload')
            require(epoch['geometryBytes'] > 0 and epoch['geometryPayloadBytes'] > 0 and imported['geometryBytes'] == 0, 'x64 geometry owner')
            require(imported['ownedDeviceBytes'] == imported['readbackBytes'] > 0, 'no duplicate image charge')
            require(not epoch['faulted'] and not imported['faulted'], 'GPU fault hidden')
            require(epoch['imageExternalFeatures'] & 6 == 6 and epoch['semaphoreExternalFeatures'] & 3 == 3, 'actual external support')
        # Same geometry/variant must reproduce the exact bytes; resize may change
        # pixel coverage, so compare only within each epoch.
        for base in (0, 8):
            require([f['fnv64'] for f in client['frames'][base:base+4]] ==
                    [f['fnv64'] for f in client['frames'][base+4:base+8]], 'repeat frames differ')
            require(len({f['fnv64'] for f in client['frames'][base:base+4]}) == 4, 'variant ignored')
        for result in (client, host):
            require(all(result[k]['valid'] for k in ('memoryBefore', 'memoryActive', 'memoryAfter')), 'unknown process memory')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--vulkan-sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--host-validation', action='store_true', help='Require x64 layer only; x86 remains explicitly unvalidated')
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows external NT handles required')
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'RH2_ACTUAL_CROSS_BITNESS_GPU_LAB', 'ok': False,
               'gpuTested': False, 'gameLaunched': False, 'shippingDllBuilt': False,
               'productIntegrated': False, 'gameMemorySavingsProved': False,
               'readbackIsTestOracleOnly': True, 'cpuImageIpcBytes': 0,
               'validationRequested': args.validation, 'hostValidationRequested': args.host_validation,
               'fullGateComplete': False, 'selectedCasesPassed': False,
               'builds': [], 'wire': [], 'cases': []}
    source_names = ['protocol.h', 'protocol.cpp', 'win32_transport.h', 'win32_transport.cpp',
                    'win32_security.h', 'win32_security.cpp', 'owned_child.h', 'owned_child.cpp',
                    'process_memory_probe.h', 'process_memory_probe.cpp', 'gpu_wire.h', 'gpu_wire.cpp',
                    'gpu_channel.h', 'gpu_lab_common.h', 'gpu_host_main.cpp', 'gpu_render_backend.h',
                    'gpu_render_backend.cpp', 'shaders/host_mesh.vert', 'shaders/host_mesh.frag']
    sources = [ROOT / 'tools/render_host' / n for n in source_names] + [Path(__file__).resolve(),
               ROOT / 'AutoTest/test_render_host_gpu_wire.cpp', ROOT / 'AutoTest/test_render_host_gpu_client.cpp',
               ROOT / 'AutoTest/run_render_host_protocol_gate.py', ROOT / 'AutoTest/run_render_host_transport_gate.py']
    try:
        receipt['sources'] = [identity(p) for p in sources]
        sdk = args.vulkan_sdk.resolve(strict=True)
        glsl = sdk / 'Bin/glslangValidator.exe'
        require(glsl.is_file(), 'explicit SDK shader compiler missing')
        receipt['shaderCompiler'] = identity(glsl)
        generated = '#pragma once\n#include <cstdint>\n'
        for stage in ('vert', 'frag'):
            spv = output / f'rh2_mesh_{stage}.spv'
            stdout, stderr = invoke([glsl, '-V', '--target-env', 'vulkan1.1',
                ROOT / f'tools/render_host/shaders/host_mesh.{stage}', '-o', spv], 30)
            (output / f'shader-{stage}.log').write_text(stdout+'\n'+stderr, encoding='utf-8')
            data = spv.read_bytes()
            require(len(data) % 4 == 0 and len(data) >= 20, 'SPIR-V size')
            words = struct.unpack('<'+'I'*(len(data)//4), data)
            require(words[0] == 0x07230203, 'SPIR-V magic')
            generated += f'constexpr uint32_t rh2_mesh_{stage}[]={{'+','.join(f'0x{w:08x}' for w in words)+'};\n'
        (output / 'rh2_shaders.h').write_text(generated, encoding='ascii')
        common = ['tools/render_host/protocol.cpp', 'tools/render_host/gpu_wire.cpp',
                  'tools/render_host/win32_transport.cpp', 'tools/render_host/win32_security.cpp',
                  'tools/render_host/process_memory_probe.cpp', 'tools/render_host/gpu_render_backend.cpp']
        exe = {}
        for bits, compiler in ((64, args.compiler64), (32, args.compiler32)):
            compiler = compiler.resolve(strict=True)
            wire = output / f'rh2-wire-{bits}.exe'
            base = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static']
            invoke(base+['tools/render_host/protocol.cpp', 'tools/render_host/gpu_wire.cpp',
                        'AutoTest/test_render_host_gpu_wire.cpp', '-o', wire], 120)
            expected_pe = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            require(pe_identity(wire) == expected_pe, 'wire executable width')
            text, stderr = invoke([wire], 20)
            m = re.fullmatch(r'RH2 codec/session checks=(\d+) bits=(32|64) PASS', text)
            require(m is not None and int(m[2]) == bits and not stderr, 'wire tests')
            encoded, stderr = invoke([wire, '--golden'], 10)
            require(not stderr and bytes.fromhex(encoded) == golden(), 'independent Python wire golden')
            receipt['wire'].append({'bits': bits, 'checks': int(m[1]), 'exe': identity(wire)})
            exe[bits] = output / f'rh2-gpu-{bits}.exe'
            more = ['tools/render_host/gpu_host_main.cpp'] if bits == 64 else [
                'tools/render_host/owned_child.cpp', 'AutoTest/test_render_host_gpu_client.cpp']
            defines = ['-DRH2_GPU_VALIDATION=1'] if args.validation or (args.host_validation and bits == 64) else []
            stdout, stderr = invoke(base+['-Wno-missing-field-initializers', '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX',
                '-municode', '-I'+str(sdk/'Include'), '-I'+str(output), *defines, *common, *more,
                '-lbcrypt', '-ladvapi32', '-lpsapi', '-o', exe[bits]], 180)
            require(pe_identity(exe[bits]) == expected_pe, 'GPU executable width')
            receipt['builds'].append({'bits': bits, 'compiler': identity(compiler), 'exe': identity(exe[bits]),
                                      'stdout': stdout, 'stderr': stderr})
        for name in args.cases:
            case = output / name
            case.mkdir(exist_ok=False)
            host_log = case / 'host.json'
            start = time.monotonic()
            # No global environment modification; caller may specify SDK layer
            # search paths on this process only for the validation build.
            process = subprocess.Popen([str(exe[32]), str(exe[64]), name, str(host_log)], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            try:
                stdout, stderr = process.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()  # exact parent Popen handle; its Job settles its child
                stdout, stderr = process.communicate(timeout=10)
                (case/'client.stdout').write_bytes(stdout)
                (case/'client.stderr').write_bytes(stderr)
                raise RuntimeError('GPU outer watchdog fired; not a passing cancellation or retirement')
            (case/'client.stdout').write_bytes(stdout)
            (case/'client.stderr').write_bytes(stderr)
            require(process.returncode == 0 and not stderr, f'{name}: native client failed: {stdout.decode(errors="replace")} {stderr.decode(errors="replace")}')
            client = strict_json(stdout.decode('utf-8'))
            host = strict_json(host_log.read_text(encoding='utf-8'))
            check_case(name, client, host)
            if args.validation or args.host_validation:
                for proc in (client, host) if args.validation else (host,):
                    require(proc['validation']['enabled'] and proc['validation']['errors'] == 0, 'Vulkan validation not clean/enabled')
            receipt['cases'].append({'case': name, 'elapsedSeconds': time.monotonic()-start,
                'client': client, 'host': host, 'settlement': child_settled(client['childPid'], client['childCreation'])})
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during gate')
        receipt['gpuTested'] = any(case['client']['frames'] for case in receipt['cases'])
        receipt['selectedCasesPassed'] = True
        receipt['fullGateComplete'] = set(args.cases) == set(CASES)
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2, ensure_ascii=False)
    print(json.dumps({'ok': receipt['ok'], 'cases': len(receipt['cases']), 'receipt': str(output/'receipt.json'),
                      'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
