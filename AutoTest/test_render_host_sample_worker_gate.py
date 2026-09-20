"""Synthetic acceptance mutations. Not a replacement for actual worker IPC."""
import copy
import hashlib
import json
import unittest

from run_render_host_sample_worker_gate import check_cycle, parse_parent, parse_host, sample_bytes


def fixture(mode='normal'):
    sources = [(7000, 1, 97)]
    for r in range(8 if mode == 'normal' else 1):
        sources.extend((7000 + r, 3 + r * 4 + i, size) for i, size in enumerate((1, 65456, 97)))
    slow = mode == 'slow'
    if slow:
        sources = [(7000, 1, 97)] + [(7001, i, 97) for i in range(2, 6)] + [(7002, 1002, 65456), (7002, 1004, 1)]
    good = mode in ('normal', 'slow')
    p = dict(cycle=0, mode=mode, bits=32, nonce='0123456789abcdef1032547698badcfe', map=101, device=201,
             settled=True, containersGone=True, childPid=123, childCreation=456,
             attempted=1004 if slow else 33 if good else 5, published=7 if slow else 25 if good else 4,
             full=996 if slow else 0, invalid=1 if slow else 8 if good else 1,
             popped=7 if slow else 25 if good else 3, queued=0 if good else 1, acknowledged=7 if slow else 25 if good else 2,
             failedInFlight=0 if good else 1, completed=good, fault=not good, stopped=not good,
             childExit=0 if good else 2, failure='None' if good else 'system', frequency=1000,
             batchStart=1100 if slow else 0, batchEnd=1200 if slow else 0, writes=7 if slow else 25 if good else 3)
    produced = [dict(produced=True, sourceFrame=f, attempt=a, payloadBytes=b) for f, a, b in sources]
    p['writtenBytes'] = sum(80 + r['payloadBytes'] for r in produced[:p['popped']])
    acks, rows = [], []
    for i, source in enumerate(produced[:p['acknowledged']]):
        data = sample_bytes(p, source, i + 1)
        ack = dict(ack=True, **{k: source[k] for k in ('sourceFrame', 'attempt', 'payloadBytes')}, transfer=i + 1,
                   slot=i % 4, generation=i // 4 + 1, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        acks.append(ack)
        rows.append(dict(envelope=True, **{k: ack[k] for k in ('sourceFrame', 'attempt', 'payloadBytes', 'transfer')}))
        if slow and i == 0:
            rows.extend([dict(delayStart=1000, frequency=1000), dict(delayEnd=2400, frequency=1000)])
        rows.append(dict(sample=True, map=p['map'], device=p['device'], frame=i + 1,
                         **{k: ack[k] for k in ('slot', 'generation', 'bytes', 'sha256')}))
    if mode == 'disconnect':
        rows.append(dict(envelope=True, sourceFrame=produced[2]['sourceFrame'], attempt=produced[2]['attempt'],
                         payloadBytes=produced[2]['payloadBytes'], transfer=3))
    host = dict(bits=64, peerVerified=True, readersDone=True, poolBytes=262144, viewProtection=2,
                state='closed' if good else 'fault', reason='None' if good else 'InjectedDisconnect', fault='none',
                copies=p['writes'], copiedBytes=p['writtenBytes'], accepted=18 if slow else 54 if good else 7,
                abandoned=0, mutexTimeouts=0, cancelRequests=0, cancelCompletions=0)
    written = []
    for i, source in enumerate(produced[:p['popped']]):
        data = sample_bytes(p, source, i + 1)
        written.append(dict(written=True, **{k: source[k] for k in ('sourceFrame', 'attempt', 'payloadBytes')},
                            transfer=i + 1, slot=i % 4, generation=i // 4 + 1,
                            bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    return dict(parent=p, produced=produced, written=written, acks=acks), host, rows


class Acceptance(unittest.TestCase):
    def reject(self, mutate, mode='normal'):
        data = fixture(mode); mutate(*data)
        with self.assertRaises((ValueError, KeyError, StopIteration)):
            check_cycle(*data)

    def test_positive_realistic_shape(self):
        check_cycle(*fixture()); check_cycle(*fixture('disconnect')); check_cycle(*fixture('slow'))

    def test_independent_bytes_not_just_supplied_digest(self):
        self.reject(lambda c, h, r: c['acks'][0].update(sha256='0' * 64))
        self.reject(lambda c, h, r: r[1].update(sha256='0' * 64))
        self.reject(lambda c, h, r: c['parent'].update(nonce='0' * 32))

    def test_scope_source_and_lease_pairing(self):
        for key, value in [('sourceFrame', 9999), ('attempt', 2), ('payloadBytes', 98),
                           ('transfer', 2), ('generation', 99), ('slot', 3), ('bytes', 81)]:
            with self.subTest(key=key):
                self.reject(lambda c, h, r: c['acks'][0].update({key: value}))
        self.reject(lambda c, h, r: c['parent'].update(map=102))
        self.reject(lambda c, h, r: r[0].update(attempt=2))
        self.reject(lambda c, h, r: c['produced'][1].update(attempt=1))

    def test_population_and_copy_algebra(self):
        for key in ('attempted', 'published', 'full', 'invalid', 'popped', 'queued', 'acknowledged',
                    'failedInFlight', 'writtenBytes', 'writes'):
            with self.subTest(key=key):
                self.reject(lambda c, h, r: c['parent'].update({key: c['parent'][key] + 1}))
        for key in ('copies', 'copiedBytes', 'accepted'):
            self.reject(lambda c, h, r: h.update({key: h[key] + 1}))

    def test_retire_settlement_and_readonly(self):
        for key in ('settled', 'containersGone'):
            self.reject(lambda c, h, r: c['parent'].update({key: False}))
        for key, value in [('viewProtection', 4), ('peerVerified', False), ('readersDone', False),
                           ('state', 'fault'), ('reason', 'Order'), ('fault', 'system'), ('mutexTimeouts', 1)]:
            self.reject(lambda c, h, r: h.update({key: value}))
        self.reject(lambda c, h, r: c['parent'].update(childExit=0), 'disconnect')
        self.reject(lambda c, h, r: c['parent'].update(failedInFlight=0), 'disconnect')

    def test_trace_order_and_missing_host_input(self):
        self.reject(lambda c, h, r: r.reverse())
        self.reject(lambda c, h, r: r.pop())
        self.reject(lambda c, h, r: r.pop(), 'disconnect')
        self.reject(lambda c, h, r: c['written'].pop(), 'disconnect')
        self.reject(lambda c, h, r: c['written'][-1].update(sha256='0' * 64), 'disconnect')

    def test_backpressure_and_actual_qpc_window(self):
        for key, value in [('full', 995), ('batchStart', 1001), ('batchEnd', 2399), ('frequency', 999),
                           ('batchStart', 1201), ('queued', 1)]:
            with self.subTest(key=key, value=value):
                self.reject(lambda c, h, r: c['parent'].update({key: value}), 'slow')
        self.reject(lambda c, h, r: r[2].update(delayEnd=1900), 'slow')
        self.reject(lambda c, h, r: r.pop(1), 'slow')
        self.reject(lambda c, h, r: c['produced'][-1].update(attempt=1003), 'slow')

    def test_strict_parsers(self):
        for text in ('{"summary":true,"summary":true}', '{"summary":true,"bits":NaN}', '{}'):
            with self.assertRaises(ValueError): parse_host(text)
        with self.assertRaises(ValueError):
            parse_host('{"envelope":true,"sample":true}\n{"summary":true}')
        with self.assertRaises(ValueError):
            parse_parent('{"summary":true,"handlesBefore":1,"handlesAfter":2}')
        c, h, rows = fixture()
        out = '\n'.join(map(json.dumps, c['produced'] + c['written'] + c['acks'] + [c['parent'],
                            dict(summary=True, handlesBefore=10, handlesAfter=10, initAfter=10, imageHandles=[7, 10, 10])]))
        self.assertEqual(parse_parent(out)[1][0], c)
        h['summary'] = True
        self.assertEqual(parse_host('\n'.join(map(json.dumps, rows + [h]))), (h, rows))


if __name__ == '__main__':
    unittest.main()
