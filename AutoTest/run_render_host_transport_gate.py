"""RH0 P1 actual Win32 client -> Win64 CPU host, isolated from the game.

Every output directory is fresh. A test watchdog kills only its owned client;
the client's atomically assigned kill-on-close Job owns helper settlement.
Watchdog termination is a failed gate, never successful I/O cancellation.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke

CASES = ('normal', 'fragment', 'bad-nonce', 'bad-sequence', 'stale-epoch',
         'bad-capability', 'oversize', 'disconnect-header', 'timeout-header',
         'timeout-body', 'nonreading-peer', 'wrong-peer', 'parent-exit', 'first-instance',
         'connect-timeout', 'lifecycle-soak')


def require(value, message):
    if not value:
        raise ValueError(message)


def strict_json(text):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate result key')
            result[key] = value
        return result
    def finite_only(value):
        raise ValueError(f'non-finite JSON constant: {value}')
    return json.loads(text, object_pairs_hook=unique, parse_constant=finite_only)


def child_settled(pid, creation):
    """Read-only check of an exact reported child; never kill a PID from text."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        require(error == 87, f'child identity unreadable: {error}; not proof of exit')
        return 'process-object-absent'
    try:
        stamps = [wintypes.FILETIME() for _ in range(4)]
        require(kernel.GetProcessTimes(handle, *(ctypes.byref(s) for s in stamps)), 'child times unreadable')
        actual = (stamps[0].dwHighDateTime << 32) | stamps[0].dwLowDateTime
        if actual != creation:
            return 'pid-reused-original-object-gone'
        require(kernel.WaitForSingleObject(handle, 7000) == 0, 'exact child not settled')
        return 'exact-process-handle-signaled'
    finally:
        kernel.CloseHandle(handle)


def check_case(name, parent, host, elapsed):
    require(parent['case'] == name and parent['bits'] == 32, 'parent identity')
    require(parent['childPid'] > 0 and parent['childCreation'] > 0, 'child lifetime pin')
    if name == 'parent-exit':
        require(parent['abruptParentExit'] and parent['exchanges'] == 1, 'abrupt parent test not reached')
        return
    require(parent['handlesBefore'] == parent['handlesAfter'], 'parent handle leak')
    require(host['bits'] == 64 and not parent['abruptParentExit'], 'host width/exit')
    if name == 'lifecycle-soak':
        require(len(parent['cycles']) == 64 and parent['exchanges'] == 215, 'soak population')
        require(all(c['handles'] == parent['handlesBefore'] for c in parent['cycles']), 'soak handle growth')
        return
    if name not in ('wrong-peer', 'first-instance', 'connect-timeout'):
        require(parent['peerVerified'] and host['peerVerified'], 'two-way peer identity')
    if name in ('normal', 'fragment'):
        require(host['state'] == 'closed' and host['fault'] == 'none' and host['protocol'] == 'None', 'normal close')
        require(parent['childExit'] == 0 and parent['exchanges'] == 11 and host['accepted'] == 11, 'normal message population')
        require(host['cancelRequests'] == 0, 'normal cancellation unexpected')
        if name == 'fragment':
            require(parent['writes'] > 270, 'client did not really split writes')
        return
    require(parent['childExit'] == 2 and host['state'] == 'fault', 'fault failed open')
    expected = {'bad-nonce': ('Session', 0), 'bad-sequence': ('Sequence', 1),
                'stale-epoch': ('Scope', 2), 'bad-capability': ('Capabilities', 0),
                'oversize': ('PayloadLimit', 0)}
    if name in expected:
        protocol, accepted = expected[name]
        require(host['protocol'] == protocol and host['accepted'] == accepted, 'wrong protocol rejection')
        if name == 'oversize':
            require(host['stage'] == 'header' and host['reads'] == 1, 'oversized body read attempted')
    elif name in ('timeout-header', 'timeout-body', 'nonreading-peer', 'connect-timeout'):
        stage = {'timeout-header': 'header', 'timeout-body': 'body', 'nonreading-peer': 'write',
                 'connect-timeout': 'connect'}[name]
        require(host['fault'] == 'timeout' and host['stage'] == stage, f'expected actual {stage} timeout')
        require(host['cancelRequests'] == host['cancelCompletions'] == 1, 'cancel not actually drained')
        require(4.5 <= elapsed < 12, 'whole packet deadline unexpected')
        require(host['accepted'] == (3 if name == 'nonreading-peer' else 0), 'timeout committed bad input')
    elif name == 'disconnect-header':
        require(host['stage'] == 'header' and host['accepted'] == 0 and host['fault'] in
                ('system', 'end-of-stream'), 'disconnect did not fail closed')
    elif name == 'wrong-peer':
        require(not host['peerVerified'] and host['accepted'] == 0 and host['fault'] == 'peer-identity', 'intruder admitted')
    elif name == 'first-instance':
        require(not host['peerVerified'] and host['accepted'] == 0 and host['stage'] == 'setup'
                and host['fault'] == 'system' and host['win32'] == 5, 'duplicate pipe instance not denied')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows lab only')
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'RH0_ACTUAL_CPU_IPC_LAB_ONLY', 'ok': False,
               'ipcTransportTested': False, 'gpuTested': False, 'gameLaunched': False, 'shippingDllBuilt': False,
               'crossLogonAclTested': False, 'cancellationDrainRequiresProcessWatchdog': True,
               'sources': [], 'builds': [], 'cases': []}
    try:
        names = ('protocol.h', 'protocol.cpp', 'win32_transport.h', 'win32_transport.cpp',
                 'win32_security.h', 'win32_security.cpp', 'owned_child.h', 'owned_child.cpp', 'host_main.cpp')
        sources = [ROOT / 'tools/render_host' / name for name in names]
        sources += [Path(__file__).resolve(), ROOT / 'AutoTest/test_render_host_transport.cpp',
                    ROOT / 'AutoTest/test_render_host_random.cpp',
                    ROOT / 'AutoTest/run_render_host_protocol_gate.py']
        receipt['sources'] = [identity(path) for path in sources]
        common = ['tools/render_host/protocol.cpp', 'tools/render_host/win32_transport.cpp',
                  'tools/render_host/win32_security.cpp']
        executables = {}
        for bits, compiler in ((64, args.compiler64), (32, args.compiler32)):
            compiler = compiler.resolve(strict=True)
            exe = output / f'rh0-transport-{bits}.exe'
            source = ['tools/render_host/host_main.cpp'] if bits == 64 else [
                'tools/render_host/owned_child.cpp', 'AutoTest/test_render_host_transport.cpp']
            stdout, stderr = invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                                     '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX', '-static', '-municode',
                                     *common, *source, '-lbcrypt', '-ladvapi32', '-o', exe], 120)
            require(pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'wrong PE architecture')
            receipt['builds'].append({'bits': bits, 'compiler': identity(compiler), 'exe': identity(exe),
                                      'stdout': stdout, 'stderr': stderr})
            rng_exe = output / f'rh0-random-{bits}.exe'
            invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                    *common, 'AutoTest/test_render_host_random.cpp', '-lbcrypt', '-ladvapi32', '-o', rng_exe], 120)
            require(pe_identity(rng_exe) == pe_identity(exe), 'random lifetime test wrong PE')
            result, errors = invoke([rng_exe], 10)
            require(not errors, 'random lifetime test stderr')
            random = strict_json(result)
            require(random['bits'] == bits and random['samples'] == 64
                    and random['handlesInitialized'] == random['handlesLast'], 'random dependency steady growth')
            receipt['builds'][-1]['randomLifetime'] = {'exe': identity(rng_exe), 'result': random}
            executables[bits] = exe
        for name in args.cases:
            case_dir = output / name
            case_dir.mkdir(exist_ok=False)
            host_log = case_dir / 'host.json'
            start = time.monotonic()
            process = subprocess.Popen([str(executables[32]), str(executables[64]), name, str(host_log)],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            try:
                stdout, stderr = process.communicate(timeout=23)
            except subprocess.TimeoutExpired:
                process.kill()  # Exact Popen process handle; its Job owns the helper.
                stdout, stderr = process.communicate(timeout=10)
                raise RuntimeError(f'{name}: outer watchdog terminated client; cancellation gate FAILED')
            elapsed = time.monotonic() - start
            (case_dir / 'client.stdout').write_bytes(stdout)
            (case_dir / 'client.stderr').write_bytes(stderr)
            require(process.returncode == 0 and not stderr, f'{name}: native client failed: {stdout!r} {stderr!r}')
            parent = strict_json(stdout.decode('utf-8'))
            init = parent['dependencyInit']
            init_settlement = child_settled(init['pid'], init['creation'])
            init_log = Path(str(host_log) + '.dependency-init')
            require(strict_json(init_log.read_text(encoding='utf-8')) == {'dependencyInit': True, 'bits': 32},
                    'dependency initialization fixture did not exit normally')
            settlement = child_settled(parent['childPid'], parent['childCreation'])
            host = None if name == 'parent-exit' else strict_json(host_log.read_text(encoding='utf-8'))
            check_case(name, parent, host, elapsed)
            if parent['exchanges']:
                receipt['ipcTransportTested'] = True
            cycle_proofs = []
            for i, cycle in enumerate(parent['cycles']):
                log = host_log if i == 0 else Path(str(host_log) + f'.cycle-{i}')
                row = strict_json(log.read_text(encoding='utf-8'))
                require(row['peerVerified'] and row['bits'] == 64, 'soak host peer')
                if i % 3 == 1:
                    require(cycle['exit'] == 2 and row['state'] == 'fault' and row['accepted'] == 0
                            and row['protocol'] == 'Session', 'soak bad nonce not rejected')
                else:
                    require(cycle['exit'] == 0 and row['state'] == 'closed' and row['accepted'] == 5
                            and row['fault'] == 'none', 'soak valid recovery did not close')
                cycle_proofs.append({'index': i, 'hostLog': identity(log),
                    'settlement': child_settled(cycle['pid'], cycle['creation']), 'host': row})
            receipt['cases'].append({'name': name, 'ok': True, 'seconds': elapsed,
                'parent': parent, 'host': host, 'settlement': settlement,
                'dependencyInitSettlement': init_settlement, 'dependencyInitLog': identity(init_log),
                'cycles': cycle_proofs,
                'hostLog': identity(host_log), 'stdout': identity(case_dir / 'client.stdout')})
            print(json.dumps({'case': name, 'ok': True, 'seconds': round(elapsed, 3)}), flush=True)
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during proof')
        for build in receipt['builds']:
            require(identity(Path(build['exe']['path'])) == build['exe'], 'executable changed during proof')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'passed': len(receipt['cases']), 'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
