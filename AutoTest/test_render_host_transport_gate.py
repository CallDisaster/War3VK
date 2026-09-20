"""Synthetic tests of proof acceptance, separate from actual paired IPC tests."""
import copy
import unittest

from run_render_host_transport_gate import check_case, strict_json


class ProofGate(unittest.TestCase):
    def setUp(self):
        self.parent = dict(case='normal', bits=32, childPid=123, childCreation=456,
                           childExit=0, exchanges=11, peerVerified=True,
                           handlesBefore=7, handlesAfter=7, writes=11, abruptParentExit=False)
        self.host = dict(bits=64, peerVerified=True, state='closed', accepted=11,
                         fault='none', stage='none', protocol='None', win32=0,
                         reads=16, writes=11, cancelRequests=0, cancelCompletions=0)

    def test_normal(self):
        check_case('normal', self.parent, self.host, .1)

    def test_normal_mutations(self):
        for target, key, bad in [('parent', 'bits', 64), ('host', 'bits', 32),
                ('parent', 'peerVerified', False), ('host', 'peerVerified', False),
                ('parent', 'handlesAfter', 8), ('host', 'state', 'fault'),
                ('host', 'accepted', 10), ('parent', 'exchanges', 10),
                ('parent', 'childExit', 2), ('host', 'cancelRequests', 1)]:
            with self.subTest(target=target, key=key):
                parent, host = copy.deepcopy(self.parent), copy.deepcopy(self.host)
                (parent if target == 'parent' else host)[key] = bad
                with self.assertRaises(ValueError):
                    check_case('normal', parent, host, .1)

    def test_fragment_requires_actual_writes(self):
        self.parent['case'] = 'fragment'
        with self.assertRaises(ValueError):
            check_case('fragment', self.parent, self.host, .1)
        self.parent['writes'] = 300
        check_case('fragment', self.parent, self.host, .1)

    def timeout(self):
        self.parent.update(case='nonreading-peer', childExit=2, exchanges=2)
        self.host.update(state='fault', fault='timeout', stage='write', accepted=3,
                         cancelRequests=1, cancelCompletions=1)

    def test_actual_write_backpressure(self):
        self.timeout()
        check_case('nonreading-peer', self.parent, self.host, 5.1)
        self.host['stage'] = 'header'
        with self.assertRaises(ValueError):
            check_case('nonreading-peer', self.parent, self.host, 5.1)

    def test_cancel_request_not_completion(self):
        self.timeout()
        self.host['cancelCompletions'] = 0
        with self.assertRaises(ValueError):
            check_case('nonreading-peer', self.parent, self.host, 5.1)

    def test_deadline_not_reset(self):
        self.timeout()
        for seconds in (0, 4.49, 12, 23):
            with self.subTest(seconds=seconds), self.assertRaises(ValueError):
                check_case('nonreading-peer', self.parent, self.host, seconds)

    def test_oversize_does_not_read_body(self):
        self.parent.update(case='oversize', childExit=2)
        self.host.update(state='fault', protocol='PayloadLimit', accepted=0, stage='header', reads=1)
        check_case('oversize', self.parent, self.host, .1)
        self.host['reads'] = 2
        with self.assertRaises(ValueError):
            check_case('oversize', self.parent, self.host, .1)

    def test_wrong_peer_rejected_before_protocol(self):
        self.parent.update(case='wrong-peer', childExit=2)
        self.host.update(state='fault', fault='peer-identity', peerVerified=False, accepted=0)
        check_case('wrong-peer', self.parent, self.host, .1)
        self.host['accepted'] = 1
        with self.assertRaises(ValueError):
            check_case('wrong-peer', self.parent, self.host, .1)

    def test_abrupt_exit_requires_handshake(self):
        self.parent.update(case='parent-exit', abruptParentExit=True, exchanges=1)
        check_case('parent-exit', self.parent, None, .1)
        self.parent['exchanges'] = 0
        with self.assertRaises(ValueError):
            check_case('parent-exit', self.parent, None, .1)

    def test_strict_duplicate_and_nonfinite(self):
        for text in ('{"ok":true,"ok":false}', '{"data":{"x":1,"x":2}}',
                     '{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                strict_json(text)
        self.assertEqual(strict_json('{"ok":true}'), {'ok': True})


if __name__ == '__main__':
    unittest.main()
