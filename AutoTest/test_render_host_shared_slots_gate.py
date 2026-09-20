"""Synthetic negative acceptance tests for RH1 receipts; not a replacement for native IPC."""
import copy
import hashlib
import unittest

from run_render_host_shared_slots_gate import check_case, check_sample, parse_lines, parse_soak, check_soak, golden
import json


def fixture():
    data = bytes((i * 17 + (i >> 8) * 3 + 13 + 7 + 19) & 255 for i in range(65536))
    samples = [{'sample': True, 'map': 1, 'device': 1, 'frame': 1, 'generation': 1,
                'slot': 0, 'bytes': 65536, 'sha256': hashlib.sha256(data).hexdigest()}]
    parent = {'case': 'duplicate-write', 'bits': 32, 'abruptParentExit': False, 'containerGone': True,
              'handlesBefore': 10, 'handlesAfter': 10, 'exceptionInit': {'eighth': 12, 'last': 12, 'steadySamples': 64},
              'childPid': 123, 'childExit': 0, 'exchanges': 6, 'writes': 1, 'writtenBytes': 65536, 'duplicateRejected': 1}
    host = {'bits': 64, 'peerVerified': True, 'readersDone': True, 'poolBytes': 262144,
            'viewProtection': 2, 'state': 'closed', 'copies': 1, 'copiedBytes': 65536, 'accepted': 6,
            'fault': 'none', 'reason': 'None', 'abandoned': 0, 'mutexTimeouts': 0, 'cancelRequests': 0, 'cancelCompletions': 0}
    return parent, host, samples, copy.deepcopy(samples)


def soak_fixture():
    parent, template, _, _ = fixture()
    parent.update(case='lifecycle-soak', exchanges=564, writes=204, writtenBytes=4829208)
    cycles = []
    for index in range(48):
        kind = index % 4
        row = dict(cycle=True, index=index, nonce=f'{index + 1:032x}', childPid=1000 + index,
                   childCreation=2000 + index, childExit=0 if kind < 2 else 2, handles=parent['handlesBefore'],
                   exchanges=(22, 11, 11, 3)[kind], writes=(8, 3, 5, 1)[kind], writtenBytes=(393218, 3072, 5120, 1024)[kind])
        samples = []
        if kind != 3:
            for epoch in range(1, 3 if kind == 0 else 2):
                for slot in range(1 if kind == 1 else 0, 4):
                    size = (1 if slot == 0 else 65536) if kind == 0 else 1024
                    sample = dict(sample=True, map=epoch, device=epoch, frame=slot + 1,
                                  generation=epoch, slot=slot, bytes=size)
                    data = bytes((i * 17 + (i >> 8) * 3 + (slot + 1) * 13 + epoch * 26) & 255 for i in range(size))
                    sample['sha256'] = hashlib.sha256(data).hexdigest()
                    samples.append(sample)
        host = copy.deepcopy(template)
        host.update(accepted=row['exchanges'], copies=(8, 3, 4, 1)[kind], copiedBytes=(393218, 3072, 4096, 1024)[kind],
                    state=('closed', 'retired', 'fault', 'fault')[kind], reason=('None', 'None', 'Lease', 'Digest')[kind])
        cycles.append(dict(parent=row, host=host, samples=samples, hostSamples=copy.deepcopy(samples)))
    parent.update({key: cycles[-1]['parent'][key] for key in ('childPid', 'childCreation', 'childExit', 'nonce')})
    return parent, cycles


class ReceiptTests(unittest.TestCase):
    def test_valid(self):
        check_case('duplicate-write', *fixture(), 0.1)

    def test_parent_evidence(self):
        for key, value in [('bits', 64), ('childExit', 2), ('exchanges', 5), ('handlesAfter', 11),
                           ('containerGone', False), ('duplicateRejected', 0), ('writes', 2), ('writtenBytes', 1)]:
            with self.subTest(key=key):
                parent, host, samples, actual = fixture(); parent[key] = value
                with self.assertRaises(ValueError): check_case('duplicate-write', parent, host, samples, actual, 0.1)

    def test_host_evidence(self):
        for key, value in [('bits', 32), ('peerVerified', False), ('readersDone', False), ('viewProtection', 4),
                           ('poolBytes', 524288), ('state', 'fault'), ('copies', 2), ('copiedBytes', 1),
                           ('accepted', 5), ('fault', 'timeout'), ('reason', 'Digest'), ('abandoned', 1)]:
            with self.subTest(key=key):
                parent, host, samples, actual = fixture(); host[key] = value
                with self.assertRaises(ValueError): check_case('duplicate-write', parent, host, samples, actual, 0.1)

    def test_exception_initialization_not_ongoing_growth(self):
        parent, host, samples, actual = fixture(); parent['exceptionInit']['last'] += 1
        with self.assertRaises(ValueError): check_case('duplicate-write', parent, host, samples, actual, 0.1)

    def test_cpu_copy_and_ack_must_match(self):
        parent, host, samples, actual = fixture(); actual[0]['generation'] += 1
        with self.assertRaises(ValueError): check_case('duplicate-write', parent, host, samples, actual, 0.1)

    def test_independent_digest(self):
        parent, host, samples, actual = fixture()
        samples[0]['sha256'] = actual[0]['sha256'] = '0' * 64
        with self.assertRaises(ValueError): check_case('duplicate-write', parent, host, samples, actual, 0.1)

    def test_sample_shape(self):
        for key, value in [('slot', 4), ('bytes', 0), ('bytes', 65537), ('map', 0), ('device', 0), ('frame', 0), ('generation', 0)]:
            with self.subTest(key=key, value=value):
                row = fixture()[2][0]; row[key] = value
                with self.assertRaises(ValueError): check_sample(row)

    def test_duplicates_nonfinite_and_missing_summary(self):
        for text in ('{"summary":true,"bits":32,"bits":64}', '{"summary":true,"x":NaN}',
                     '{"sample":true}', '{"summary":true}\n{"summary":true}'):
            with self.subTest(text=text):
                with self.assertRaises(ValueError): parse_lines(text)

    def test_golden(self):
        self.assertEqual(len(golden()), 72)
        self.assertEqual(golden()[:4], b'WVS1')
        self.assertEqual(golden()[48:56], b'\x20\0\0\0\x40\0\0\0')

    def test_soak_valid_and_parser(self):
        parent, cycles = soak_fixture()
        check_soak(parent, cycles)
        rows = []
        for cycle in cycles:
            rows.extend(cycle['samples']); rows.append(cycle['parent'])
        parent['summary'] = True; rows.append(parent)
        actual, parsed = parse_soak('\n'.join(json.dumps(r) for r in rows))
        self.assertEqual(actual, parent)
        self.assertEqual(len(parsed), 48)

    def test_soak_each_cycle_checked(self):
        original, cycles = soak_fixture()
        for index, target, key, value in [(47, 'parent', 'handles', 11), (26, 'host', 'reason', 'None'),
                (13, 'host', 'state', 'closed'), (24, 'host', 'copies', 0), (9, 'parent', 'writes', 0),
                (11, 'host', 'readersDone', False), (4, 'host', 'viewProtection', 4)]:
            with self.subTest(index=index, key=key):
                changed = copy.deepcopy(cycles); changed[index][target][key] = value
                with self.assertRaises(ValueError): check_soak(original, changed)

    def test_soak_no_missing_or_reused_cycle(self):
        parent, cycles = soak_fixture()
        with self.assertRaises(ValueError): check_soak(parent, cycles[:-1])
        changed = copy.deepcopy(cycles); changed[1]['parent']['nonce'] = changed[0]['parent']['nonce']
        with self.assertRaises(ValueError): check_soak(parent, changed)
        changed = copy.deepcopy(cycles); changed[1]['parent']['childCreation'] = changed[0]['parent']['childCreation']
        changed[1]['parent']['childPid'] = changed[0]['parent']['childPid']
        with self.assertRaises(ValueError): check_soak(parent, changed)

    def test_soak_no_relabelled_generation_or_missing_ack(self):
        parent, cycles = soak_fixture()
        changed = copy.deepcopy(cycles)
        for field in ('samples', 'hostSamples'): changed[32][field][4]['generation'] = 1
        with self.assertRaises(ValueError): check_soak(parent, changed)
        changed = copy.deepcopy(cycles); changed[3]['samples'] = copy.deepcopy(changed[1]['samples'])
        with self.assertRaises(ValueError): check_soak(parent, changed)

    def test_soak_parser_rejects_dropped_cycle(self):
        parent, cycles = soak_fixture(); parent['summary'] = True
        text = '\n'.join(json.dumps(c['parent']) for c in cycles[1:]) + '\n' + json.dumps(parent)
        with self.assertRaises(ValueError): parse_soak(text)

    def test_abrupt_writer_requires_real_write(self):
        parent = dict(case='parent-exit-writing', bits=32, abruptParentExit=True, exchanges=3, writes=1, writtenBytes=65536)
        check_case('parent-exit-writing', parent, None, [], [], 0.1)
        parent['writes'] = 0
        with self.assertRaises(ValueError): check_case('parent-exit-writing', parent, None, [], [], 0.1)


if __name__ == '__main__':
    unittest.main()
