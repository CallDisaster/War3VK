"""Independent Python oracle for the synthetic E2 CPU lab, never gameplay proof."""
import argparse
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import struct

SESSION = 0x1020304050607080
U64 = (1 << 64) - 1
MEM = 'privateBytes peakPrivateBytes workingSetBytes usedVirtualBytes privateVirtualBytes mappedVirtualBytes regions'.split()
PU = ('bits map device session capacity attempted accepted lost popped queued ackedEvents sentPackets ackPackets trigger '
      'triggerRequestedQpc triggerAppliedQpc qpcFrequency producerStartQpc producerEndQpc hostPid hostCreation '
      'hostExit ingressBytes poolBytes handlesBefore handlesAfter').split()
PB = 'workerJoined hostSettled containersGone fault completed'.split()
HU = ('bits session state nextOrdinal lastSequence trigger capacity accepted attempted lost evicted retained reason '
      'storageBytes wireError storeError runtimeExit visited firstSequence lastVisited delayStartQpc delayEndQpc qpcFrequency').split()
HB = 'recorderHost beginSeen endSeen closeSeen contentValid cpuComplete'.split()
TU = ('bits win32 accepted viewProtection poolBytes copies copiedBytes abandoned mutexTimeouts reads writes '
      'cancelRequests cancelCompletions').split()
TB = 'summary peerVerified readersDone'.split()
REASONS = {'disconnect': 'InjectedRecorderDisconnect', 'missing-seal': 'RecorderMissingSeal',
           'late-data': 'RecorderStore', 'wrong-ordinal': 'RecorderLease', 'wrong-session': 'RecorderIdentity',
           'wrong-scope': 'RecorderIdentity', 'bad-kind': 'RecorderWire', 'seal-totals': 'RecorderStore',
           **{n: 'HelloContract' for n in ('p2-to-e2', 'p3-to-e2', 'e2-to-p2', 'e2-to-p3')}}


def require(value, name):
    if not value:
        raise ValueError(name)


def finite_tree(obj):
    if isinstance(obj, float):
        require(math.isfinite(obj), 'non-finite value')
    elif isinstance(obj, dict):
        for k, v in obj.items():
            require(type(k) is str, 'non-string key'); finite_tree(v)
    elif isinstance(obj, list):
        for value in obj: finite_tree(value)


def strict_json(text):
    def pairs(values):
        out = {}
        for key, value in values:
            require(key not in out, 'duplicate key'); out[key] = value
        return out
    def reject(value):
        raise ValueError('non-finite constant: ' + value)
    result = json.loads(text, object_pairs_hook=pairs, parse_constant=reject)
    finite_tree(result)
    return result


def fields(obj, ints=(), bools=(), strings=()):
    require(type(obj) is dict, 'expected object')
    for key in ints:
        require(type(obj.get(key)) is int and 0 <= obj[key] <= U64, 'u64 field: ' + key)
    for key in bools:
        require(type(obj.get(key)) is bool, 'bool field: ' + key)
    for key in strings:
        require(type(obj.get(key)) is str, 'string field: ' + key)


def memory(obj):
    fields(obj, ['win32', *MEM], ['valid'])
    require(obj['valid'] and obj['win32'] == 0 and obj['regions'] > 0, 'memory query failed')
    require(obj['peakPrivateBytes'] >= obj['privateBytes'], 'memory peak')
    require(obj['privateVirtualBytes'] <= obj['usedVirtualBytes'] and
            obj['mappedVirtualBytes'] <= obj['usedVirtualBytes'], 'memory regions')


def event_bytes(seq):
    # Independent specification: 8*u64 + thread/kind + label + 12*u64 + 48*u32.
    qpc = 0x1234000000000000 + seq
    bits = [(seq * 73 + i) & 0xffffffff for i in range(48)]
    bits[:3] = [0x7f800000, 0x7fc12345, 0x80000000]
    return struct.pack('<8QII32s12Q48I', seq, SESSION, seq, qpc,
                       0x9876000000000000 + seq, seq // 100, 11, 17, seq % 6, 1,
                       b'recorder-e2\0', *[qpc + i for i in range(12)], *bits)


@lru_cache(maxsize=8)
def expected_digest(first, last):
    require(type(first) is int and type(last) is int and 0 < first <= last <= 300000,
            'fixture digest range')
    h = hashlib.sha256()
    for seq in range(first, last + 1): h.update(event_bytes(seq))
    return h.hexdigest()


def analyze_case(p, h, t):
    out = dict(ok=False, failedChecks=[], cpuComplete=False, gameMemoryBenefitMeasured=False)
    try:
        finite_tree([p, h, t])
        fields(p, PU, PB, ['mode', 'nonce']); fields(t, TU, TB, ['state', 'reason', 'fault', 'stage'])
        mode = p['mode']; good = mode in ('normal', 'small', 'slow'); mismatch = mode in REASONS and REASONS[mode] == 'HelloContract'
        require(good or mode in REASONS, 'unknown mode')
        require(p['bits'] == 32 and t['bits'] == 64 and re.fullmatch('[0-9a-f]{32}', p['nonce']), 'bits/nonce')
        require((p['map'], p['device'], p['session']) == (11, 17, SESSION), 'fixture identity')
        require(p['capacity'] == (262144 if mode == 'normal' else 8192), 'parent capacity')
        require(p['ingressBytes'] == 3670400 and p['poolBytes'] == t['poolBytes'] == 262144, 'fixed local budgets')
        require(p['attempted'] == p['accepted'] + p['lost'] and p['accepted'] == p['popped'] + p['queued'], 'ingress algebra')
        require(p['ackedEvents'] <= p['popped'] and p['ackPackets'] <= p['sentPackets'], 'ACK algebra')
        require(all(p[n] for n in ('workerJoined', 'hostSettled', 'containersGone')) and
                p['hostPid'] > 0 and p['hostCreation'] > 0, 'settlement')
        require(p['handlesBefore'] == p['handlesAfter'], 'handle leak')
        require(p['completed'] is good and p['fault'] is (not good) and p['hostExit'] == (0 if good else 2), 'parent terminal')
        require(t['summary'] and t['peerVerified'] and t['readersDone'], 'transport ownership')
        require(t['state'] == ('closed' if good else 'fault') and t['reason'] == ('None' if good else REASONS[mode]), 'transport terminal')
        require(t['fault'] == t['stage'] == 'none' and t['win32'] == 0, 'unexpected transport failure')
        require(t['viewProtection'] == (0 if mismatch else 2), 'read-only mapping')
        require(t['abandoned'] == t['mutexTimeouts'] == t['cancelRequests'] == t['cancelCompletions'] == 0, 'unsettled transport')
        rejected_packet = not good and not mismatch and mode != 'missing-seal'
        controls = (4 + 2*p['ackPackets'] if good else 0 if mismatch else
                    2 + 2*p['ackPackets'] if mode == 'missing-seal' else 3 + 2*p['ackPackets'])
        require(p['sentPackets'] == p['ackPackets'] + int(rejected_packet) and
                t['accepted'] == controls and t['writes'] == controls, 'control/ACK population')
        require(t['copies'] == p['sentPackets'] and
                t['copiedBytes'] == 80*p['sentPackets'] + 392*(p['popped'] + int(mode == 'late-data')), 'private copy population')
        require(t['reads'] >= (2*controls-2 if good else 2*controls+1 if mode=='missing-seal' else 2*(controls+1)), 'control read population')
        for key in ('memoryBefore', 'memoryActive', 'memoryAfter'): memory(p.get(key))
        before, active, after = (p[n] for n in ('memoryBefore', 'memoryActive', 'memoryAfter'))
        for key in ('privateBytes', 'usedVirtualBytes'):
            require(active[key] - before[key] <= 16 * 1024**2, 'x86 active memory: ' + key)
        require(after['privateBytes'] - before['privateBytes'] <= 8 * 1024**2, 'x86 retained memory')
        if mode.startswith('e2-to-'):
            require(h is None and p['attempted'] == p['sentPackets'] == 0, 'legacy rejection no history')
            out['ok'] = True
            return out
        fields(h, HU, HB, ['mode', 'digest'])
        require(h['recorderHost'] and h['bits'] == 64 and h['mode'] == (mode if mode in ('slow', 'disconnect') else 'normal'), 'host mode/width')
        require(h['runtimeExit'] == p['hostExit'] and 0 <= h['state'] <= 3 and h['wireError'] <= 15 and h['storeError'] <= 16, 'host terminal/enums')
        require(h['accepted'] == h['lastSequence'] and h['accepted'] >= h['evicted'] and
                h['retained'] == h['accepted'] - h['evicted'] and h['storageBytes'] == h['capacity'] * 392, 'history algebra')
        require(h['accepted'] == p['ackedEvents'] and h['nextOrdinal'] == p['ackPackets'] + 1, 'cross-process commit/ordinal')
        for key in ('memoryBefore', 'memoryAllocated', 'memoryActive'): memory(h.get(key))
        if mismatch:
            require(h['state'] == h['session'] == h['capacity'] == h['storageBytes'] == h['accepted'] == 0 and
                    not h['beginSeen'] and p['attempted'] == p['sentPackets'] == 0, 'hello rejection allocated history')
        else:
            require(h['session'] == SESSION and h['capacity'] == p['capacity'] and h['beginSeen'], 'host Begin identity')
        if good:
            require(h['state'] == 2 and h['endSeen'] and h['closeSeen'] and h['wireError'] == h['storeError'] == 0 and h['reason'] == 1, 'Seal/End/Close')
            require(all(h[k] == p[k] for k in ('session', 'capacity', 'trigger', 'accepted', 'attempted', 'lost')), 'Seal population')
            require(p['accepted'] == p['popped'] == p['ackedEvents'] and p['queued'] == 0, 'full drain')
            require(p['ackPackets'] >= (p['accepted']+159)//160 + 2, 'bounded event batches')
            require(p['triggerRequestedQpc'] > 0 and p['triggerAppliedQpc'] >= p['triggerRequestedQpc'] and p['qpcFrequency'] > 0, 'trigger QPC')
            require(0 < p['producerStartQpc'] < p['producerEndQpc'], 'producer QPC')
            if mode in ('normal', 'small'):
                cap = p['capacity']; cut = cap - cap // 4 + 160
                require((p['accepted'], p['trigger'], h['retained'], h['evicted'], p['lost']) ==
                        (cap + 160, cut, cap, 160, 0), 'fixed positive population')
                require(h['delayStartQpc'] == h['delayEndQpc'] == 0, 'unexpected delay')
            else:
                require(p['attempted'] == 20160 and p['lost'] > 0 and p['trigger'] == p['accepted'] and h['retained'] == min(p['accepted'], 6144), 'bounded slow/loss')
                require(h['qpcFrequency'] == p['qpcFrequency'] and
                        0 < h['delayStartQpc'] <= p['producerStartQpc'] < p['producerEndQpc'] <= h['delayEndQpc'], 'actual blocked QPC window')
            require(h['visited'] == h['retained'] > 0 and h['firstSequence'] == h['evicted'] + 1 and h['lastVisited'] == h['accepted'], 'complete visit')
            require(h['contentValid'] and re.fullmatch('[0-9a-f]{64}', h['digest']) and
                    h['digest'] == expected_digest(h['firstSequence'], h['lastVisited']), 'independent event SHA')
            require(h['cpuComplete'] is (p['lost'] == 0), 'loss cannot be complete')
            if mode == 'normal':
                require(h['memoryAllocated']['privateBytes'] - h['memoryBefore']['privateBytes'] >= h['storageBytes'] - 2*1024**2,
                        'real x64 private history allocation')
            out['cpuComplete'] = h['cpuComplete']
        else:
            require(not h['cpuComplete'] and not h['contentValid'] and not h['endSeen'] and not h['closeSeen'], 'failure false completeness')
            require(h['visited'] == h['firstSequence'] == h['lastVisited'] == 0 and h['digest'] == '', 'failure usable digest')
            if mode in ('late-data', 'seal-totals'):
                require(h['state'] == 3 and h['storeError'] != 0, 'store fault not latched')
            elif not mismatch:
                require(h['state'] == 1 and h['storeError'] == 0, 'outer failure corrupt store state')
            require((h['wireError'] != 0) is (mode == 'bad-kind'), 'wire failure evidence')
            if mode in ('wrong-ordinal','wrong-session','wrong-scope','bad-kind'):
                require(h['accepted'] == 0 and p['ackPackets'] == 1 and 0 < p['popped'] <= p['accepted'] <= 160,
                        'first Data rejection population')
            if mode == 'disconnect':
                require(p['ackPackets'] == 3 and 2 <= h['accepted'] <= 320 and
                        h['accepted'] < p['popped'] <= p['accepted'] <= 480, 'third Data rejection population')
        out['ok'] = True
    except (ValueError, KeyError, TypeError, OverflowError, struct.error) as exc:
        out['failedChecks'].append(str(exc)); out['cpuComplete'] = False
    return out


def analyze_receipt(root):
    finite_tree(root)
    require(type(root) is dict and type(root.get('schema')) is int and root['schema'] == 1 and
            root.get('scope') == 'RECORDER_OFFLOAD_E2_CPU_LAB', 'receipt identity')
    require(type(root.get('cases')) is list and root['cases'], 'empty cases')
    verdicts = [analyze_case(c.get('parent'), c.get('host'), c.get('transport')) for c in root['cases'] if type(c) is dict]
    require(len(verdicts) == len(root['cases']), 'invalid case')
    nonces = [c.get('parent', {}).get('nonce') for c in root['cases'] if type(c.get('parent')) is dict]
    require(len(nonces) == len(root['cases']) and len(set(nonces)) == len(nonces), 'connection nonce reused or absent')
    return dict(ok=all(r['ok'] for r in verdicts), cases=verdicts, gameMemoryBenefitMeasured=False)


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--receipt', type=Path, required=True)
    args = p.parse_args()
    try:
        result = analyze_receipt(strict_json(args.receipt.read_text(encoding='utf-8')))
    except (ValueError, OSError, TypeError) as exc:
        result = dict(ok=False, failedChecks=[str(exc)], gameMemoryBenefitMeasured=False)
    print(json.dumps(result, ensure_ascii=False, allow_nan=False))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
