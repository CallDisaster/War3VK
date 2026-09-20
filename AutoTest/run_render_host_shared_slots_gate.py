"""RH1 real bounded 256-KiB shared mapping; Win32 producer -> Win64 CPU consumer.

No game, DLL, GPU, arbitrary resource import or memory-saving claim. Fresh output
only. A watchdog kill is always a failed test, not a successful timeout proof.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import require, strict_json, child_settled

CASES = ('normal', 'fragment', 'cancel', 'duplicate-write', 'old-nonce', 'old-map',
         'old-device', 'old-frame', 'old-generation', 'old-size', 'old-slot', 'duplicate-publish',
         'digest', 'partial-write', 'mutex-timeout', 'mutex-abandoned', 'bad-version', 'bad-bits',
         'bad-capability', 'bad-flags', 'bad-sequence', 'bad-nonce', 'oversize', 'disconnect',
         'parent-exit', 'existing-mapping', 'existing-mutex', 'lifecycle-soak', 'parent-exit-writing', 'late-reused',
         'hello-required', 'repeated-hello', 'begin-active', 'reserve-before-begin', 'end-before-begin',
         'end-writing', 'close-active', 'capacity', 'reserve-zero', 'cancelled-publish', 'publish-shape')


def golden():
    return struct.pack('<4sHHHHIIIQQQ', b'WVS1', 1, 0, 48, 1, 0, 24, 0,
                       0x0807060504030201, 0x1817161514131211, 1) + struct.pack('<IIQII', 32, 64, 2, 4, 65536)


def parse_lines(text):
    rows = [strict_json(line) for line in text.splitlines()]
    require(rows and rows[-1].get('summary') is True, 'missing final summary')
    require(all(row.get('sample') is True and 'summary' not in row for row in rows[:-1]), 'unexpected sample row')
    return rows[-1], rows[:-1]


def parse_soak(text):
    rows = [strict_json(line) for line in text.splitlines()]
    require(rows and rows[-1].get('summary') is True, 'missing soak summary')
    pending, cycles = [], []
    for row in rows[:-1]:
        if row.get('sample') is True and 'cycle' not in row and 'summary' not in row:
            pending.append(row)
        else:
            require(row.get('cycle') is True and 'summary' not in row and 'sample' not in row, 'unexpected cycle row')
            require(row['index'] == len(cycles), 'cycle missing/repeated/out of order')
            cycles.append({'parent': row, 'samples': pending})
            pending = []
    require(not pending and len(cycles) == 48, 'incomplete soak or unowned samples')
    return rows[-1], cycles


def check_sample(row):
    require(0 <= row['slot'] < 4 and 0 < row['bytes'] <= 65536 and row['generation'] > 0, 'sample bounds')
    require(row['map'] > 0 and row['device'] > 0 and row['frame'] > 0, 'sample epoch/frame')
    data = bytes((i * 17 + (i >> 8) * 3 + row['frame'] * 13 + row['map'] * 7 + row['device'] * 19) & 255
                 for i in range(row['bytes']))
    require(hashlib.sha256(data).hexdigest() == row['sha256'], 'independent sample SHA mismatch')


def containers_gone(nonce):
    require(len(nonce) == 32 and all(c in '0123456789abcdef' for c in nonce), 'nonce encoding')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenFileMappingW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.OpenFileMappingW.restype = wintypes.HANDLE
    kernel.OpenMutexW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.OpenMutexW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    for suffix, opener, access in [('data', kernel.OpenFileMappingW, 4)] + [
            (f'slot{i}', kernel.OpenMutexW, 0x100000) for i in range(4)]:
        handle = opener(access, False, f'Local\\warvk-rh1-{nonce}-{suffix}')
        error = ctypes.get_last_error()
        if handle:
            kernel.CloseHandle(handle)
            raise ValueError('shared container still exists after process settlement')
        require(error == 2, f'container absence unproven: {error}')


def check_case(name, parent, host, samples, host_samples, elapsed):
    require(parent['bits'] == 32 and parent['case'] == name, 'actual producer identity')
    for row in samples + host_samples:
        check_sample(row)
    if name in ('parent-exit', 'parent-exit-writing'):
        writing = name == 'parent-exit-writing'
        require(parent['abruptParentExit'] and parent['exchanges'] == (3 if writing else 1)
                and not samples, 'abrupt path not reached')
        require(parent['writes'] == (1 if writing else 0) and parent['writtenBytes'] == (65536 if writing else 0),
                'abrupt exit did not reach claimed lease/write')
        return
    require(not parent['abruptParentExit'] and parent['containerGone'], 'container not settled')
    require(parent['handlesBefore'] == parent['handlesAfter'], 'parent handle leak')
    require(parent['exceptionInit']['eighth'] == parent['exceptionInit']['last']
            and parent['exceptionInit']['steadySamples'] == 64, 'exception dependency keeps growing')
    if name in ('existing-mapping', 'existing-mutex'):
        require(parent['childPid'] == 0 and parent['childExit'] == 0 and not samples and host is None, 'collision test shape')
        return
    require(host['bits'] == 64 and host['peerVerified'] and host['readersDone'], 'host/reader settlement')
    require(host['poolBytes'] == 262144, 'pool growth')
    if parent['exchanges']:
        require(host['viewProtection'] == 2, 'consumer is not read-only')
    require(samples == host_samples, 'ACK samples differ from actual private-copy samples')
    good = name in ('normal', 'fragment', 'cancel', 'duplicate-write')
    require(host['state'] == ('retired' if name == 'cancel' else 'closed') if good else host['state'] == 'fault',
            'wrong terminal state')
    require(parent['childExit'] == (0 if good else 2), 'child exit proof')
    if good:
        count = {'normal': 36, 'fragment': 36, 'cancel': 18, 'duplicate-write': 1}[name]
        require(host['fault'] == 'none' and host['reason'] == 'None', 'good run fault')
        require(len(samples) == host['copies'] == parent['writes'] == count, 'good sample population')
        require(host['copiedBytes'] == parent['writtenBytes'] == sum(row['bytes'] for row in samples), 'byte population')
        expected_exchanges = {'normal': 80, 'fragment': 80, 'cancel': 41, 'duplicate-write': 6}[name]
        require(host['accepted'] == parent['exchanges'] == expected_exchanges, 'message population')
        require(host['abandoned'] == host['mutexTimeouts'] == host['cancelRequests'] == host['cancelCompletions'] == 0,
                'normal completion concealed a failure')
        if name == 'fragment':
            require(parent['wireWrites'] > 2000, 'writes were not fragmented in reality')
        if name == 'duplicate-write':
            require(parent['duplicateRejected'] == 1, 'second write not rejected')
        if name in ('normal', 'fragment'):
            require({row['map'] for row in samples} == {1, 2, 3} and {row['slot'] for row in samples} == set(range(4)), 'epochs/slots')
            require({row['bytes'] for row in samples} == {1, 97, 4096, 65536}, 'size coverage')
        return
    reasons = {'bad-version': 'Version', 'bad-bits': 'HelloContract', 'bad-capability': 'HelloContract',
               'bad-flags': 'RequestFlags', 'bad-sequence': 'Sequence', 'bad-nonce': 'Nonce', 'oversize': 'PayloadLimit',
               'digest': 'Digest', 'partial-write': 'Digest'}
    if name in reasons:
        require(host['reason'] == reasons[name], 'wrong wire/content rejection')
        require(host['copies'] == (1 if name in ('digest', 'partial-write') else 0), 'bad input copied unexpectedly')
        require(host['accepted'] == (3 if name in ('digest', 'partial-write') else 0), 'invalid message accepted')
        if name == 'oversize':
            require(host['reads'] == 1, 'oversized body read attempted')
        if name == 'partial-write':
            require(parent['writes'] == 0, 'partial fixture went through full-size writer')
    elif name.startswith('old-') or name == 'duplicate-publish':
        require(host['reason'] in ('Lease', 'SlotState'), 'wrong lease rejection')
        require(host['copies'] == (1 if name == 'duplicate-publish' else 0), 'invalid lease copied')
        require(host['accepted'] == (4 if name == 'duplicate-publish' else 3), 'invalid key accepted')
    elif name == 'mutex-timeout':
        require(host['fault'] == 'timeout' and host['stage'] == 'slot-mutex' and host['mutexTimeouts'] == 1
                and host['copies'] == 0 and 4.5 <= elapsed < 12, 'actual mutex timeout proof')
    elif name == 'mutex-abandoned':
        require(host['fault'] == 'abandoned' and host['stage'] == 'slot-mutex' and host['abandoned'] == 1
                and host['copies'] == 0, 'actual abandoned mutex proof')
    elif name == 'disconnect':
        require(host['fault'] in ('system', 'end-of-stream') and host['stage'] == 'header' and host['copies'] == 0, 'disconnect')
    else:
        reasons = {'hello-required': ('HelloRequired', 0), 'repeated-hello': ('RepeatedHello', 1),
                   'begin-active': ('Phase', 2), 'reserve-before-begin': ('Phase', 1), 'end-before-begin': ('Phase', 1),
                   'end-writing': ('SlotState', 3), 'close-active': ('ClosePhase', 2), 'capacity': ('Capacity', 6),
                   'reserve-zero': ('Size', 2), 'cancelled-publish': ('SlotState', 4), 'publish-shape': ('Shape', 3),
                   'late-reused': ('Lease', 11)}
        require(name in reasons, 'case has no exact acceptance rule')
        reason, accepted = reasons[name]
        require(host['reason'] == reason and host['accepted'] == accepted, 'wrong state rejection/population')
        require(host['fault'] == 'none' and host['copies'] == (4 if name == 'late-reused' else 0), 'bad state copied')


def check_soak(parent, cycles):
    require(parent['case'] == 'lifecycle-soak' and parent['bits'] == 32 and not parent['abruptParentExit'], 'soak identity')
    require(parent['containerGone'] and parent['handlesBefore'] == parent['handlesAfter'], 'soak final lifetime')
    require(parent['exceptionInit']['eighth'] == parent['exceptionInit']['last']
            and parent['exceptionInit']['steadySamples'] == 64, 'soak initialization growth')
    require(len(cycles) == 48 and len({c['parent']['nonce'] for c in cycles}) == 48, 'missing/reused connection')
    require(len({(c['parent']['childPid'], c['parent']['childCreation']) for c in cycles}) == 48, 'reused process proof')
    require(parent['exchanges'] == 564 and parent['writes'] == 204 and parent['writtenBytes'] == 4829208, 'soak totals')
    for index, cycle in enumerate(cycles):
        row, host = cycle['parent'], cycle['host']
        kind = index % 4
        expected_samples = (8, 3, 4, 0)[kind]
        exchanges = (22, 11, 11, 3)[kind]
        require(row['index'] == index and row['childPid'] > 0 and row['childCreation'] > 0, 'cycle identity')
        require(row['handles'] == parent['handlesBefore'] and row['exchanges'] == exchanges, 'cycle resource/order proof')
        require(row['childExit'] == (0 if kind < 2 else 2), 'cycle exit')
        require(host['bits'] == 64 and host['peerVerified'] and host['readersDone'] and host['viewProtection'] == 2
                and host['poolBytes'] == 262144, 'cycle host/readonly/reader boundary')
        require(host['accepted'] == exchanges and host['copies'] == (8, 3, 4, 1)[kind], 'cycle message/copy population')
        require(row['writes'] == (8, 3, 5, 1)[kind] and row['writtenBytes'] == (393218, 3072, 5120, 1024)[kind], 'cycle writes')
        require(host['copiedBytes'] == (393218, 3072, 4096, 1024)[kind], 'cycle actual copied bytes')
        require(host['state'] == ('closed', 'retired', 'fault', 'fault')[kind]
                and host['reason'] == ('None', 'None', 'Lease', 'Digest')[kind] and host['fault'] == 'none', 'cycle terminal cause')
        require(host['abandoned'] == host['mutexTimeouts'] == host['cancelRequests'] == host['cancelCompletions'] == 0,
                'soak concealed transport failure')
        require(cycle['samples'] == cycle['hostSamples'] and len(cycle['samples']) == expected_samples, 'cycle ACK/input pairing')
        keys = [(s['map'], s['device'], s['frame'], s['generation'], s['slot'], s['bytes']) for s in cycle['samples']]
        if kind == 0:
            expected_keys = [(epoch, epoch, slot + 1, epoch, slot, 1 if slot == 0 else 65536)
                             for epoch in (1, 2) for slot in range(4)]
        elif kind == 1:
            expected_keys = [(1, 1, slot + 1, 1, slot, 1024) for slot in range(1, 4)]
        elif kind == 2:
            expected_keys = [(1, 1, slot + 1, 1, slot, 1024) for slot in range(4)]
        else:
            expected_keys = []
        require(keys == expected_keys, 'actual cycle frame/epoch/generation population')
        for sample in cycle['samples']:
            check_sample(sample)
    last = cycles[-1]['parent']
    require(all(parent[k] == last[k] for k in ('childPid', 'childCreation', 'childExit', 'nonce')), 'final identity differs')


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
    receipt = {'schema': 1, 'scope': 'RH1_SHARED_CPU_SLOTS_ONLY', 'ok': False, 'cases': [], 'builds': [],
               'sharedMappingTested': False, 'gpuTested': False, 'gameLaunched': False, 'shippingDllBuilt': False,
               'crossLogonAclTested': False, 'cancellationDrainRequiresProcessWatchdog': True}
    names = ('protocol.h', 'protocol.cpp', 'win32_transport.h', 'win32_transport.cpp', 'win32_security.h',
             'win32_security.cpp', 'owned_child.h', 'owned_child.cpp', 'slot_ledger.h', 'slot_ledger.cpp',
             'slot_wire.h', 'slot_wire.cpp', 'slot_channel.h', 'shared_slots.h', 'shared_slots.cpp', 'slot_host_main.cpp',
             'slot_host_runtime.h', 'slot_host_runtime.cpp')
    sources = [ROOT / 'tools/render_host' / n for n in names] + [ROOT / 'AutoTest' / n for n in (
        'test_render_host_shared_slots.cpp', 'test_render_host_slot_wire.cpp', 'run_render_host_shared_slots_gate.py',
        'run_render_host_protocol_gate.py', 'run_render_host_transport_gate.py')]
    try:
        receipt['sources'] = [identity(p) for p in sources]
        common = [f'tools/render_host/{n}' for n in ('protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp',
                  'slot_ledger.cpp', 'slot_wire.cpp', 'shared_slots.cpp')]
        executables = {}
        for bits, compiler in ((64, args.compiler64), (32, args.compiler32)):
            compiler = compiler.resolve(strict=True)
            wire = output / f'rh1-wire-{bits}.exe'
            invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                    'tools/render_host/protocol.cpp', 'tools/render_host/slot_wire.cpp',
                    'AutoTest/test_render_host_slot_wire.cpp', '-o', wire], 120)
            expected_pe = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            require(pe_identity(wire) == expected_pe, 'wrong wire PE')
            wire_out, wire_err = invoke([wire], 20)
            core = strict_json(wire_out)
            require(core['ok'] and core['bits'] == bits and core['checks'] > 10000 and not wire_err, 'wire core failure')
            vector, errors = invoke([wire, '--golden'], 10)
            require(bytes.fromhex(vector) == golden() and not errors, 'independent RH1 golden mismatch')
            exe = output / f'rh1-shared-{bits}.exe'
            extra = ['tools/render_host/slot_host_main.cpp', 'tools/render_host/slot_host_runtime.cpp'] if bits == 64 else [
                'tools/render_host/owned_child.cpp', 'AutoTest/test_render_host_shared_slots.cpp']
            out, err = invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-municode',
                '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX', *common, *extra, '-lbcrypt', '-ladvapi32', '-o', exe], 120)
            require(pe_identity(exe) == expected_pe, 'wrong shared IPC PE')
            receipt['builds'].append({'bits': bits, 'compiler': identity(compiler), 'exe': identity(exe),
                'stdout': out, 'stderr': err, 'wireTest': core, 'wireExe': identity(wire),
                'goldenBytes': len(golden()), 'goldenSha256': hashlib.sha256(golden()).hexdigest()})
            executables[bits] = exe
        for name in args.cases:
            directory = output / name; directory.mkdir(exist_ok=False)
            log = directory / 'host.ndjson'
            start = time.monotonic()
            process = subprocess.Popen([str(executables[32]), str(executables[64]), name, str(log)], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            try:
                stdout, stderr = process.communicate(timeout=23)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate(timeout=10)
                raise RuntimeError(f'{name}: outer watchdog fired; FAILED not successful cancellation')
            elapsed = time.monotonic() - start
            (directory / 'client.stdout').write_bytes(stdout)
            (directory / 'client.stderr').write_bytes(stderr)
            require(process.returncode == 0 and not stderr, f'{name}: client failed: {stdout[-1800:]!r} {stderr!r}')
            cycles = []
            if name == 'lifecycle-soak':
                parent, cycles = parse_soak(stdout.decode('utf-8'))
                samples = [sample for cycle in cycles for sample in cycle['samples']]
            else:
                parent, samples = parse_lines(stdout.decode('utf-8'))
            init_settlement = child_settled(parent['initPid'], parent['initCreation'])
            init_log = Path(str(log) + '.dependency-init')
            require(strict_json(init_log.read_text(encoding='utf-8')) == {'dependencyInit': True, 'bits': 32}, 'init fixture')
            settlement = child_settled(parent['childPid'], parent['childCreation']) if parent['childPid'] else 'not-started'
            if name in ('parent-exit', 'parent-exit-writing', 'lifecycle-soak') or not parent['childPid']:
                host, host_samples = None, []
            else:
                host, host_samples = parse_lines(log.read_text(encoding='utf-8'))
            containers_gone(parent['nonce'])
            if name == 'lifecycle-soak':
                for index, cycle in enumerate(cycles):
                    cycle_log = log if index == 0 else Path(str(log) + f'.cycle-{index}')
                    cycle['host'], cycle['hostSamples'] = parse_lines(cycle_log.read_text(encoding='utf-8'))
                    row = cycle['parent']
                    cycle['settlement'] = child_settled(row['childPid'], row['childCreation'])
                    containers_gone(row['nonce'])
                    cycle['containersGone'] = True
                    cycle['hostLog'] = identity(cycle_log)
                check_soak(parent, cycles)
            else:
                check_case(name, parent, host, samples, host_samples, elapsed)
            receipt['sharedMappingTested'] |= bool(samples)
            receipt['cases'].append({'name': name, 'ok': True, 'seconds': elapsed, 'parent': parent,
                'host': host, 'samples': samples, 'cycles': cycles, 'settlement': settlement, 'containersGone': True,
                'initSettlement': init_settlement, 'initLog': identity(init_log),
                'hostLog': identity(log) if log.exists() else None, 'stdout': identity(directory / 'client.stdout')})
            print(json.dumps({'case': name, 'ok': True, 'seconds': round(elapsed, 3)}), flush=True)
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during proof')
        for build in receipt['builds']:
            require(identity(Path(build['exe']['path'])) == build['exe'], 'exe changed during proof')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2); stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'passed': len(receipt['cases']),
                      'receipt': str(output / 'receipt.json'), 'error': receipt.get('error')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
