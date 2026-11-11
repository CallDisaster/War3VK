"""x86 bounded command queue -> x64 ACTUAL DXVK resource owner (isolated lab).

This does not call D3D9 or a game. The blocking transport's final kernel drain
is still externally supervised. Passing proves resource-subset integration, not
a production transport owner, D3D9 compatibility, gameplay or memory savings.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_core_gate import source_paths, execute

CASES = ('normal', 'stale-generation', 'wrong-map', 'bad-range')


def golden_hash(size, salt, patched=False):
    value = 14695981039346656037
    for i in range(size):
        byte = ((i-3)*29 + ((i-3) >> 8)*7 + 9*43) & 255 if patched and 3 <= i < 100 else (
            i*29 + (i >> 8)*7 + salt*43) & 255
        value = ((value ^ byte) * 1099511628211) & ((1 << 64)-1)
    return value


def wire_golden():
    return struct.pack('<4sHHII8Q16x', b'WRC1', 1, 2, 3, 0,
        0x0807060504030201, 0x1817161514131211, 3, 7, 9, 2, 5, 11) + bytes((1, 2, 254))


def check_child_identity(parent):
    # Check before the Win32 read-only settlement call: ctypes DWORD must not
    # silently truncate a malformed report into an unrelated process ID.
    require(type(parent['childPid']) is int and 0 < parent['childPid'] <= 0xffffffff, 'native child PID')
    require(type(parent['childCreation']) is int and 0 < parent['childCreation'] <= (1 << 64)-1,
            'native child creation time')


def check_case(case, parent, host):
    require(case in CASES, 'unrecognized case')
    check_child_identity(parent)
    for result, fields in ((parent, ('bits', 'childPid', 'childCreation', 'childExit', 'exchanges',
            'published', 'cpuAcknowledged', 'fullRefusals', 'accepted', 'submitted', 'gpuCompleted',
            'retired', 'liveBytes', 'hashCount', 'queueBytes')),
            (host, ('bits', 'accepted', 'backendCalls', 'submitted', 'gpuCompleted', 'retired',
            'liveBytes', 'peakLiveBytes', 'created', 'destroyed', 'uploadedBytes', 'verifiedBytes',
            'vendorId', 'deviceId'))):
        for field in fields:
            require(type(result[field]) is int and 0 <= result[field] <= (1 << 64)-1,
                    f'exact uint64 JSON field required: {field}')
    require(type(parent['hashes']) is list and len(parent['hashes']) == 3 and
            all(type(h) is int and 0 <= h <= (1 << 64)-1 for h in parent['hashes']), 'exact hash array')
    require(0 < host['vendorId'] <= 0xffffffff and 0 < host['deviceId'] <= 0xffffffff, 'native GPU identity')
    normal = case == 'normal'
    require(parent['schema'] == 'warvk-buffer-bridge/v1' and host['schema'] == 'warvk-buffer-host/v1', 'schema')
    require(parent['ok'] is True and host['ok'] is normal and parent['bits'] == 32 and host['bits'] == 64, 'success/width')
    require(parent['peerVerified'] is True and host['peerVerified'] is True and host['actualDxvkCore'] is True,
            'real peer/core missing')
    require(parent['parentVulkanAbsent'] is True and parent['childPid'] > 0 and parent['childCreation'] > 0,
            'parent owner/child identity')
    expected_error = {'normal': 'None', 'stale-generation': 'Generation', 'wrong-map': 'Scope', 'bad-range': 'Range'}[case]
    for result in (parent, host):
        require(result['error'] == expected_error, 'wrong rejection reason')
        require(result['accepted'] == (14 if normal else 9), 'accepted stream count')
        require(all(result[k] == (9 if normal else 6) for k in ('submitted', 'gpuCompleted', 'retired')),
                'exact GPU/submission/retirement count (no work on rejected command)')
        require(result['liveBytes'] == (0 if normal else 98307), 'logical live bytes')
        for field in ('productIntegrated', 'gameTouched', 'memorySavingsProven'):
            require(result[field] is False, f'unsupported claim: {field}')
    require(parent['childExit'] == (0 if normal else 2) and parent['expectedRejected'] is (not normal), 'terminal status')
    require(parent['exchanges'] == parent['published'] == (14 if normal else 10) and
            parent['cpuAcknowledged'] == (14 if normal else 9), 'queue/remote ack counts')
    require(parent['fullRefusals'] > 0 and 262144 <= parent['queueBytes'] <= 263168, 'bounded backpressure witness')
    require(parent['hashCount'] == (3 if normal else 2), 'verification population')
    hashes = [golden_hash(131101, 1), golden_hash(131101, 1, True), golden_hash(98307, 2) if normal else 0]
    require(parent['hashes'] == hashes, 'independent full-GPU-buffer byte oracle')
    require(host['backendCalls'] == (14 if normal else 9), 'bad command reached backend')
    require(host['created'] == 2 and host['destroyed'] == (2 if normal else 1), 'resource/ABA life cycle')
    require(host['peakLiveBytes'] == 131101, 'logical peak is not VRAM accounting')
    require(host['uploadedBytes'] == 131101 + 97 + (98307 if normal else 0), 'upload coverage')
    require(host['verifiedBytes'] == 131101*2 + (98307 if normal else 0), 'GPU verification coverage')
    require(host['normalTeardown'] is normal, 'normal teardown cannot be inferred from a fault')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', required=True, type=Path)
    parser.add_argument('--compiler64', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'X1_BOUNDED_X86_TO_ACTUAL_DXVK_X64_RESOURCE_LAB', 'ok': False,
               'productIntegrated': False, 'gameLaunched': False, 'shippingDllBuilt': False,
               'gameMemorySavingsProved': False, 'gpuValidationLayerTested': False,
               'watchdogTerminationIsFailure': True, 'codecRuns': [], 'steps': [], 'cases': []}
    try:
        require(os.name == 'nt', 'Windows lab required')
        build = args.build.resolve(strict=True)
        options = {o['name']: o['value'] for o in strict_json((build/'meson-info/intro-buildoptions.json').read_text('utf-8'))}
        require(options['warvk_render_host_core_dev'] is True and options['enable_d3d9'] is False,
                'only explicit standalone core build allowed')
        for key in ('enable_dxgi', 'enable_d3d8', 'enable_d3d10', 'enable_d3d11'):
            require(options[key] is False, 'not the isolated core build')
        extra = ('tools/render_host/buffer_commands.h', 'tools/render_host/buffer_commands.cpp',
                 'tools/render_host/render_command_queue.h', 'tools/render_host/buffer_lab_protocol.h',
                 'tools/render_host/protocol.h', 'tools/render_host/protocol.cpp',
                 'tools/render_host/win32_transport.h', 'tools/render_host/win32_transport.cpp',
                 'tools/render_host/win32_security.h', 'tools/render_host/win32_security.cpp',
                 'tools/render_host/owned_child.h', 'tools/render_host/owned_child.cpp',
                 'AutoTest/test_render_host_buffer_commands.cpp', 'AutoTest/test_render_host_buffer_parent.cpp',
                 'AutoTest/run_render_host_buffer_gate.py', 'AutoTest/test_render_host_buffer_gate.py')
        paths = sorted(set(source_paths()) | {ROOT/p for p in extra})
        receipt['sources'] = [identity(p) for p in paths]
        compilers = {32: args.compiler32.resolve(strict=True), 64: args.compiler64.resolve(strict=True)}
        for bits, compiler in compilers.items():
            exe = output/f'buffer-codec-{bits}.exe'
            cmd = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                   ROOT/'tools/render_host/buffer_commands.cpp', ROOT/'AutoTest/test_render_host_buffer_commands.cpp', '-o', exe]
            stdout, stderr = invoke(cmd, 180)
            require(not stdout and not stderr, 'codec compiler diagnostics')
            require(pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'codec width')
            stdout, stderr = invoke([exe], 30)
            match = re.fullmatch(r'buffer commands checks=(\d+) failures=0 bits=(32|64)(?: [A-Za-z][A-Za-z0-9_]*=\d+)*', stdout)
            require(match and int(match[1]) >= 100 and int(match[2]) == bits and not stderr, 'codec actual tests')
            golden, stderr = invoke([exe, '--golden'], 10)
            require(bytes.fromhex(golden) == wire_golden() and not stderr, 'independent little-endian wire vector')
            receipt['codecRuns'].append({'bits': bits, 'checks': int(match[1]), 'compiler': identity(compiler),
                                         'exe': identity(exe), 'goldenBytes': len(wire_golden()), 'stdout': stdout})
        ninja = shutil.which('ninja')
        require(ninja, 'Ninja required')
        target = Path('tools/render_host/dxvk_core/warvk-dxvk-buffer-host.exe')
        receipt['steps'].append(execute([ninja, '-C', build, '-j2', target.as_posix()], ROOT, output/'host-build', 600))
        receipt['steps'].append(execute([ninja, '-C', build, '-n', target.as_posix()], ROOT, output/'host-no-work', 30))
        require('no work to do' in (output/'host-no-work.stdout').read_text('utf-8'), 'host still dirty')
        host_exe = build/target
        require(pe_identity(host_exe) == (0x8664, 0x20b), 'host must be x64')
        receipt['hostExecutable'] = identity(host_exe)
        from run_render_host_core_gate import check_host_compile_boundary
        receipt['allowedSharedD3D9Inputs'] = check_host_compile_boundary(build)
        parent_exe = output/'buffer-parent-32.exe'
        common = ['protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp', 'owned_child.cpp', 'buffer_commands.cpp']
        cmd = [compilers[32], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread',
               '-municode', '-DNOMINMAX', '-D_WIN32_WINNT=0x0a00',
               *(ROOT/'tools/render_host'/p for p in common), ROOT/'AutoTest/test_render_host_buffer_parent.cpp',
               '-lbcrypt', '-ladvapi32', '-o', parent_exe]
        stdout, stderr = invoke(cmd, 180)
        require(not stdout and not stderr and pe_identity(parent_exe) == (0x14c, 0x10b), 'parent build/width')
        receipt['parentExecutable'] = identity(parent_exe)
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('DXVK_', 'VK_'))}
        env.update(DXVK_LOG_PATH=str(output), DXVK_LOG_LEVEL='none', DXVK_SHADER_CACHE='0',
                   DXVK_CONFIG_FILE=str(output/'intentionally-absent-lab.conf'))
        receipt['runtimeOverrides'] = {k: v for k, v in env.items() if k.startswith('DXVK_')}
        for case in CASES:
            host_log = output/f'{case}-host.json'
            started = time.monotonic()
            proc = subprocess.Popen([str(parent_exe), str(host_exe), str(host_log), case], cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            timed_out = False
            try:
                stdout, stderr = proc.communicate(timeout=50)
            except subprocess.TimeoutExpired:
                timed_out = True
                proc.kill()  # Only this exact test parent. Its Job contains the exact host.
                stdout, stderr = proc.communicate(timeout=10)
            (output/f'{case}-parent.stdout').write_bytes(stdout)
            (output/f'{case}-parent.stderr').write_bytes(stderr)
            result = {'case': case, 'seconds': time.monotonic()-started, 'exit': proc.returncode,
                      'watchdogTerminated': timed_out}
            receipt['cases'].append(result)
            require(not timed_out, 'whole-process watchdog: failed gate')
            parent = strict_json(stdout.decode('utf-8'))
            result['parent'] = parent
            if parent.get('childPid', 0) and parent.get('childCreation', 0):
                check_child_identity(parent)
                result['settlement'] = child_settled(parent['childPid'], parent['childCreation'])
            require(host_log.is_file(), 'host did not write report')
            result['hostLog'] = identity(host_log)
            host = strict_json(host_log.read_text('utf-8'))
            result['host'] = host
            require(proc.returncode == 0 and not stderr, 'native parent failed; inspect exact reports')
            check_case(case, parent, host)
            result['ok'] = True
        require(receipt['sources'] == [identity(p) for p in paths], 'source changed during gate')
        require(identity(host_exe) == receipt['hostExecutable'] and identity(parent_exe) == receipt['parentExecutable'], 'executable drift')
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
