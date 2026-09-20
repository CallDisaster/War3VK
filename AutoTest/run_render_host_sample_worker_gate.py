"""Actual Win32 SPSC -> sole worker -> Win64 envelope consumer CPU laboratory.

Fresh outputs only; no game or GPU and no shipping DLL. A watchdog is failure,
not successful cancellation. Synthetic bytes, never Game/native struct layouts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_shared_slots_gate import containers_gone

CASES = ('normal', 'slow', 'disconnect', 'recovery', 'soak', 'p3-to-p2', 'p2-to-p3',
         'p3-to-p2-soak', 'p2-to-p3-soak', 'wrong-scope', 'wrong-transfer', 'repeat-attempt', 'source-backward', 'length')
REASONS = {'disconnect': 'InjectedDisconnect', 'p3-to-p2': 'HelloContract', 'p2-to-p3': 'HelloContract',
           'wrong-scope': 'Scope', 'wrong-transfer': 'Lease', 'repeat-attempt': 'Order',
           'source-backward': 'Order', 'length': 'Size'}


def sample_bytes(scope, row, transfer):
    n = scope['nonce']
    require(len(n) == 32 and all(c in '0123456789abcdef' for c in n), 'nonce shape')
    high, low = int(n[:16], 16), int(n[16:], 16)
    require(0 < row['payloadBytes'] <= 65456 and 0 < row['attempt'] <= 0xffffffff
            and row['sourceFrame'] > 0 and transfer > 0, 'source bounds')
    body = bytes((i * 17 + (i >> 8) * 3 + row['sourceFrame'] * 13 + row['attempt'] * 19) & 255
                 for i in range(row['payloadBytes']))
    return struct.pack('<4sHHIIII7Q', b'WVP1', 1, 0, 80, 0, 80 + len(body), len(body),
                       low, high, scope['map'], scope['device'], row['sourceFrame'], row['attempt'], transfer) + body


def parse_parent(text):
    rows = [strict_json(line) for line in text.splitlines()]
    require(rows and rows[-1].get('summary') is True, 'missing parent summary')
    summary = rows[-1]
    require(summary['handlesBefore'] == summary['handlesAfter'], 'parent handle leak')
    require(len(summary['imageHandles']) == 3 and summary['imageHandles'][1] == summary['imageHandles'][2]
            == summary['initAfter'] == summary['handlesBefore'], 'exact-image init unstable')
    produced, written, acks, cycles = [], [], [], []
    for row in rows[:-1]:
        if row.get('produced') is True:
            require(not written and not acks and 'cycle' not in row and 'ack' not in row and 'written' not in row,
                    'source after write/ACK/ambiguous row')
            produced.append(row)
        elif row.get('written') is True:
            require(not acks and 'cycle' not in row and 'ack' not in row, 'ambiguous written row')
            written.append(row)
        elif row.get('ack') is True:
            require('cycle' not in row, 'ambiguous ACK'); acks.append(row)
        else:
            require(row.get('cycle') == len(cycles), 'cycle sequence')
            cycles.append({'parent': row, 'produced': produced, 'written': written, 'acks': acks})
            produced, written, acks = [], [], []
    require(not produced and not written and not acks and cycles, 'unowned evidence')
    return summary, cycles


def parse_host(text):
    rows = [strict_json(line) for line in text.splitlines()]
    require(rows and rows[-1].get('summary') is True, 'missing host summary')
    allowed = ('envelope', 'sample', 'delayStart', 'delayEnd')
    for row in rows[:-1]:
        require(sum(key in row for key in allowed) == 1 and 'summary' not in row, 'ambiguous host row')
    return rows[-1], rows[:-1]


def check_cycle(cycle, host, host_rows):
    p, produced, acks = cycle['parent'], cycle['produced'], cycle['acks']
    written = cycle['written']
    mode = p['mode']; good = mode in ('normal', 'slow'); mismatch = mode in ('p3-to-p2', 'p2-to-p3')
    require(mode in CASES and mode not in ('recovery', 'soak') and p['bits'] == 32 and host['bits'] == 64, 'mode/width')
    require(p['settled'] and p['containersGone'] and p['childPid'] > 0 and p['childCreation'] > 0, 'child settlement')
    require(p['map'] == 101 + p['cycle'] and p['device'] == 201 + p['cycle'], 'fixed cycle scope')
    require(host['peerVerified'] and host['readersDone'] and host['poolBytes'] == 262144, 'host ownership')
    require(host['viewProtection'] == (0 if mismatch else 2), 'read-only host mapping')
    require(p['attempted'] == p['published'] + p['full'] + p['invalid'], 'producer algebra')
    require(p['published'] == p['acknowledged'] + p['failedInFlight'] + p['queued'], 'ACK algebra')
    require(p['popped'] == p['acknowledged'] + p['failedInFlight'] and p['failedInFlight'] in (0, 1), 'in-flight algebra')
    require(p['published'] == len(produced) and p['acknowledged'] == len(acks), 'actual population')
    require(p['completed'] is good and p['fault'] is not good and p['stopped'] is not good, 'terminal state')
    require(p['childExit'] == (0 if good else 2) and host['state'] == ('closed' if good else 'fault'), 'host exit')
    require(host['reason'] == ('None' if good else REASONS[mode]) and host['fault'] == 'none', 'fault boundary')
    require(host['abandoned'] == host['mutexTimeouts'] == host['cancelRequests'] == host['cancelCompletions'] == 0,
            'unexpected transport fault')
    envelopes = [r for r in host_rows if r.get('envelope') is True]
    samples = [r for r in host_rows if r.get('sample') is True]
    delays = [r for r in host_rows if 'delayStart' in r or 'delayEnd' in r]
    require(len(samples) == len(acks) and len(envelopes) == len(acks) + (mode == 'disconnect'), 'actual host consumption')
    previous_frame = previous_attempt = 0
    for row in produced:
        require(row['sourceFrame'] >= previous_frame and row['attempt'] > previous_attempt, 'source order/gaps')
        previous_frame, previous_attempt = row['sourceFrame'], row['attempt']
    for index, ack in enumerate(acks):
        source = produced[index]
        require(all(ack[k] == source[k] for k in ('sourceFrame', 'attempt', 'payloadBytes')), 'ACK source pairing')
        require(ack['transfer'] == index + 1 and ack['slot'] == index % 4 and ack['generation'] == index // 4 + 1,
                'lease/transfer order')
        data = sample_bytes(p, source, index + 1)
        require(ack['bytes'] == len(data) and ack['sha256'] == hashlib.sha256(data).hexdigest(), 'independent complete SHA')
        e, s = envelopes[index], samples[index]
        require(all(e[k] == ack[k] for k in ('sourceFrame', 'attempt', 'payloadBytes', 'transfer')), 'host source pairing')
        require(s == dict(sample=True, map=p['map'], device=p['device'], frame=index + 1,
                          generation=ack['generation'], slot=ack['slot'], bytes=len(data), sha256=ack['sha256']),
                'host private-copy receipt')
    copies = len(acks) + p['failedInFlight']
    require(len(written) == copies, 'written-but-unacknowledged population')
    for index, row in enumerate(written):
        require(all(row[k] == produced[index][k] for k in ('sourceFrame', 'attempt', 'payloadBytes')), 'written source identity')
        data = bytearray(sample_bytes(p, produced[index], index + 1))
        if index == 2:
            edits = {'wrong-scope': (40, '<Q', p['map'] + 1), 'wrong-transfer': (72, '<Q', 4),
                     'repeat-attempt': (64, '<Q', produced[index - 1]['attempt']), 'source-backward': (56, '<Q', 1),
                     'length': (20, '<I', produced[index]['payloadBytes'] + 1)}
            if mode in edits:
                offset, fmt, value = edits[mode]; struct.pack_into(fmt, data, offset, value)
        require(row['transfer'] == index + 1 and row['slot'] == index % 4 and row['generation'] == index // 4 + 1,
                'written lease order')
        require(row['bytes'] == len(data) and row['sha256'] == hashlib.sha256(data).hexdigest(), 'actual written envelope SHA')
        if index < len(acks):
            require({k: v for k, v in row.items() if k != 'written'} ==
                    {k: v for k, v in acks[index].items() if k != 'ack'}, 'ACK does not identify written bytes')
    trace = [next(key for key in ('envelope', 'sample', 'delayStart', 'delayEnd') if key in r) for r in host_rows]
    expected_trace = []
    for index in range(copies):
        if index < len(acks) or mode == 'disconnect':
            expected_trace.append('envelope')
        if mode == 'slow' and index == 0:
            expected_trace.extend(('delayStart', 'delayEnd'))
        if index < len(acks):
            expected_trace.append('sample')
    require(trace == expected_trace, 'host validation/copy trace order')
    require(host['copies'] == p['writes'] == copies, 'actual copy population')
    require(host['copiedBytes'] == p['writtenBytes'] == sum(80 + r['payloadBytes'] for r in produced[:copies]),
            'byte population')
    require(host['accepted'] == (4 + 2 * len(acks) if good else 0 if mismatch else 3 + 2 * len(acks)),
            'control message population')
    if good:
        require(p['failedInFlight'] == p['queued'] == 0 and p['failure'] == 'None', 'normal drain')
        if mode == 'normal':
            require((p['attempted'], p['published'], p['full'], p['invalid']) == (33, 25, 0, 8), 'normal positive controls')
            expected = [(7000, 1, 97)]
            for r in range(8):
                expected.extend((7000 + r, 3 + r * 4 + i, size) for i, size in enumerate((1, 65456, 97)))
        else:
            require((p['attempted'], p['published'], p['full'], p['invalid']) == (1004, 7, 996, 1), 'slow backpressure')
            expected = [(7000, 1, 97)] + [(7001, i, 97) for i in range(2, 6)] + [(7002, 1002, 65456), (7002, 1004, 1)]
        require([(r['sourceFrame'], r['attempt'], r['payloadBytes']) for r in produced] == expected, 'exact producer trace')
    elif mismatch:
        require(p['attempted'] == p['published'] == p['popped'] == p['failedInFlight'] == 0, 'mismatch touched data')
    else:
        require((p['attempted'], p['published'], p['full'], p['invalid'], p['queued'], p['failedInFlight']) == (5, 4, 0, 1, 1, 1),
                'deterministic fault population')
        if mode == 'disconnect':
            require(all(envelopes[-1][k] == produced[2][k] for k in ('sourceFrame', 'attempt', 'payloadBytes'))
                    and envelopes[-1]['transfer'] == 3, 'unacknowledged host input')
    if mode == 'slow':
        require(len(delays) == 2 and 'delayStart' in delays[0] and 'delayEnd' in delays[1], 'actual delay trace')
        require(delays[0]['frequency'] == delays[1]['frequency'] == p['frequency'] > 0, 'cross-process QPC domain')
        require(delays[0]['delayStart'] + 1 < p['batchStart'] <= p['batchEnd'] < delays[1]['delayEnd'] - 1,
                'producer did not complete inside actual host stall')
        require(delays[1]['delayEnd'] - delays[0]['delayStart'] >= p['frequency'], 'delay too short')
    else:
        require(not delays and p['batchStart'] == p['batchEnd'] == 0, 'unexpected delay')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    parser.add_argument('--diagnostic-case', choices=CASES)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows only')
    output = args.output.resolve(); output.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='P3B2_SYNTHETIC_CPU_WORKER_ONLY', ok=False, builds=[], cases=[],
                   gpuTested=False, gameLaunched=False, shippingDllBuilt=False, productIntegration=False,
                   cancellationDrainRequiresProcessWatchdog=True)
    names = ('protocol', 'win32_transport', 'win32_security', 'owned_child', 'slot_ledger', 'slot_wire',
             'shared_slots', 'slot_host_runtime', 'sample_envelope')
    sources = [ROOT / 'tools/render_host' / (n + suffix) for n in names for suffix in ('.h', '.cpp')]
    sources += [ROOT / 'tools/render_host' / n for n in ('slot_channel.h', 'producer_inbox.h', 'sample_host_main.cpp', 'slot_host_main.cpp')]
    sources += [ROOT / 'AutoTest' / n for n in ('test_render_host_sample_worker.cpp', 'render_host_handle_probe.h', 'run_render_host_sample_worker_gate.py',
                'run_render_host_protocol_gate.py', 'run_render_host_transport_gate.py', 'run_render_host_shared_slots_gate.py')]
    try:
        receipt['sources'] = [identity(p) for p in sources]
        common = [f'tools/render_host/{n}.cpp' for n in ('protocol', 'win32_transport', 'win32_security',
                  'slot_ledger', 'slot_wire', 'shared_slots')]
        binaries = {}
        for name, bits, compiler, extra in (
            ('host', 64, args.compiler64, ['tools/render_host/slot_host_runtime.cpp', 'tools/render_host/sample_envelope.cpp', 'tools/render_host/sample_host_main.cpp']),
            ('legacy', 64, args.compiler64, ['tools/render_host/slot_host_runtime.cpp', 'tools/render_host/slot_host_main.cpp']),
            ('client', 32, args.compiler32, ['tools/render_host/owned_child.cpp', 'tools/render_host/sample_envelope.cpp', 'AutoTest/test_render_host_sample_worker.cpp'])):
            exe = output / f'p3-{name}-{bits}.exe'; compiler = compiler.resolve(strict=True)
            stdout, stderr = invoke([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread',
                '-municode', '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX', *common, *extra, '-lbcrypt', '-ladvapi32', '-o', exe], 120)
            require(not stderr and pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'build/PE')
            binaries[name] = exe
            receipt['builds'].append(dict(name=name, bits=bits, compiler=identity(compiler), exe=identity(exe), stdout=stdout, stderr=stderr))
        for name in args.cases:
            directory = output / name; directory.mkdir(exist_ok=False)
            log = directory / 'host.ndjson'; start = time.monotonic()
            host_exe = binaries['legacy' if name.startswith('p3-to-p2') else 'host']
            previous_probe = os.environ.get('WARVK_HOST_LAB_HANDLE_PROBE')
            try:
                if name == args.diagnostic_case:
                    os.environ['WARVK_HOST_LAB_HANDLE_PROBE'] = '1'
                stdout, stderr = invoke([binaries['client'], host_exe, name, log], 23)
            finally:
                if previous_probe is None:
                    os.environ.pop('WARVK_HOST_LAB_HANDLE_PROBE', None)
                else:
                    os.environ['WARVK_HOST_LAB_HANDLE_PROBE'] = previous_probe
            (directory / 'client.stdout').write_text(stdout + '\n', encoding='utf-8')
            (directory / 'client.stderr').write_text(stderr + '\n', encoding='utf-8')
            require(not stderr, 'client stderr (preserved in case directory)')
            parent, cycles = parse_parent(stdout)
            init_settlement = child_settled(parent['initPid'], parent['initCreation'])
            require(len(parent['imageInit']) == 2, 'image init count')
            image_initialization = []
            for index, row in enumerate(parent['imageInit']):
                require(row['exit'] == (2 if name.startswith('p3-to-p2') else 3), 'invalid-arg startup must reject')
                path = Path(str(log) + f'.host-init-{index}')
                if name.startswith('p3-to-p2'):
                    init_host, init_rows = parse_host(path.read_text(encoding='utf-8'))
                    require(init_host['reason'] == 'Argument' and init_host['accepted'] == 0 and not init_host['peerVerified']
                            and init_host['copies'] == 0 and not init_rows, 'startup unexpectedly used IPC')
                else:
                    require(path.stat().st_size == 0, 'sample host argc guard output')
                image_initialization.append(dict(log=identity(path), settlement=child_settled(row['pid'], row['creation'])))
            require(len(cycles) == (16 if name.endswith('soak') else 2 if name == 'recovery' else 1), 'cycle count')
            require(len({c['parent']['nonce'] for c in cycles}) == len(cycles), 'reused connection')
            if name == 'recovery':
                require(cycles[0]['parent']['nonce'] != cycles[1]['parent']['nonce'] and
                        cycles[0]['parent']['mode'] == 'disconnect' and cycles[1]['parent']['mode'] == 'normal', 'fresh recovery')
            for index, cycle in enumerate(cycles):
                path = log if index == 0 else Path(str(log) + f'.cycle-{index}')
                require(cycle['parent']['handles'] == parent['handlesBefore'], 'per-cycle handle growth')
                if name == 'soak':
                    require(cycle['parent']['mode'] == ('disconnect' if index % 2 else 'normal'), 'soak order')
                elif name.endswith('-soak'):
                    require(cycle['parent']['mode'] == name[:-5], 'mismatch soak order')
                host, rows = parse_host(path.read_text(encoding='utf-8'))
                check_cycle(cycle, host, rows)
                p = cycle['parent']; containers_gone(p['nonce'])
                cycle.update(host=host, hostRows=rows, log=identity(path),
                             settlement=child_settled(p['childPid'], p['childCreation']))
            receipt['cases'].append(dict(name=name, ok=True, seconds=time.monotonic() - start, parent=parent,
                                         cycles=cycles, initSettlement=init_settlement,
                                         imageInitialization=image_initialization,
                                         stdout=identity(directory / 'client.stdout')))
            print(json.dumps({'case': name, 'ok': True}), flush=True)
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during run')
        for build in receipt['builds']:
            require(identity(Path(build['exe']['path'])) == build['exe'], 'binary changed during run')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2); stream.write('\n')
    print(json.dumps(dict(ok=receipt['ok'], passed=len(receipt['cases']), error=receipt.get('error'), receipt=str(output / 'receipt.json'))))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
