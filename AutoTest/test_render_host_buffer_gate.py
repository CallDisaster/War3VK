"""CPU-only counterexamples against the actual X1 resource receipt checker.

Run with ``python -B AutoTest/test_render_host_buffer_gate.py``. This imports
the production check_case, not a replacement checker. No build, GPU, game,
subprocess, receipt-file mutation, or monkeypatching occurs in this test.

Fixtures are transcribed from x64-render-20261011-buffers-r1; the three exact
hashes are frozen here, not calculated by the gate under test. Fixture success
is a predicate test, not another physical GPU run or a memory-savings claim.
"""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_render_host_buffer_gate import CASES, check_case, check_child_identity


NAMES = ('normal', 'stale-generation', 'wrong-map', 'bad-range')
ERRORS = {'normal': 'None', 'stale-generation': 'Generation',
          'wrong-map': 'Scope', 'bad-range': 'Range'}
HASHES = (1222312238053058050, 16156330116523515587, 17049398311894857306)
UINT64_MAX = (1 << 64) - 1
PARENT_NUMERIC = ('bits', 'childPid', 'childCreation', 'childExit', 'exchanges',
                  'published', 'cpuAcknowledged', 'fullRefusals', 'accepted',
                  'submitted', 'gpuCompleted', 'retired', 'liveBytes',
                  'hashCount', 'queueBytes')
HOST_NUMERIC = ('bits', 'accepted', 'backendCalls', 'submitted', 'gpuCompleted',
                'retired', 'liveBytes', 'peakLiveBytes', 'created', 'destroyed',
                'uploadedBytes', 'verifiedBytes', 'vendorId', 'deviceId')
PARENT_BOOLEAN = ('ok', 'peerVerified', 'expectedRejected', 'parentVulkanAbsent',
                  'productIntegrated', 'gameTouched', 'memorySavingsProven')
HOST_BOOLEAN = ('ok', 'actualDxvkCore', 'peerVerified', 'normalTeardown',
                'productIntegrated', 'gameTouched', 'memorySavingsProven')


def fixture(name):
    if name not in NAMES:
        raise ValueError(name)
    normal = name == 'normal'
    parent = {
        'schema': 'warvk-buffer-bridge/v1', 'ok': True, 'bits': 32,
        'childPid': 21576, 'childCreation': 134361682720938619,
        'childExit': 0 if normal else 2, 'peerVerified': True,
        'exchanges': 14 if normal else 10, 'published': 14 if normal else 10,
        'cpuAcknowledged': 14 if normal else 9, 'fullRefusals': 2118591,
        'expectedRejected': not normal, 'error': ERRORS[name],
        'accepted': 14 if normal else 9, 'submitted': 9 if normal else 6,
        'gpuCompleted': 9 if normal else 6, 'retired': 9 if normal else 6,
        'liveBytes': 0 if normal else 98307, 'hashCount': 3 if normal else 2,
        'hashes': [HASHES[0], HASHES[1], HASHES[2] if normal else 0],
        'queueBytes': 262528, 'parentVulkanAbsent': True,
        'productIntegrated': False, 'gameTouched': False,
        'memorySavingsProven': False,
    }
    host = {
        'schema': 'warvk-buffer-host/v1', 'ok': normal, 'bits': 64,
        'actualDxvkCore': True, 'peerVerified': True,
        'accepted': 14 if normal else 9, 'backendCalls': 14 if normal else 9,
        'error': ERRORS[name], 'submitted': 9 if normal else 6,
        'gpuCompleted': 9 if normal else 6, 'retired': 9 if normal else 6,
        'liveBytes': 0 if normal else 98307, 'peakLiveBytes': 131101,
        'created': 2, 'destroyed': 2 if normal else 1,
        'uploadedBytes': 229505 if normal else 131198,
        'verifiedBytes': 360509 if normal else 262202,
        'normalTeardown': normal, 'vendorId': 4318, 'deviceId': 10245,
        'productIntegrated': False, 'gameTouched': False,
        'memorySavingsProven': False,
    }
    return parent, host


def reference_payload(size, salt, patched=False):
    """Materialized bytes, independent of the checker's on-the-fly hash loop."""
    result = bytearray((index * 29 + (index // 256) * 7 + salt * 43) % 256
                       for index in range(size))
    if patched:
        result[3:100] = reference_payload(97, 9)
    return result


def reference_hash(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & UINT64_MAX
    return value


class BufferReceiptGateTests(unittest.TestCase):
    def assert_rejected(self, name, parent, host):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            check_case(name, parent, host)

    def mutate(self, side, field, value, names=NAMES):
        for name in names:
            with self.subTest(case=name, side=side, field=field, value=value):
                parent, host = fixture(name)
                (parent, host)[side][field] = copy.deepcopy(value)
                self.assert_rejected(name, parent, host)

    def test_case_population_is_not_empty_or_a_subset(self):
        self.assertEqual(tuple(CASES), NAMES)

    def test_normal_positive(self):
        check_case('normal', *fixture('normal'))

    def test_stale_generation_positive(self):
        check_case('stale-generation', *fixture('stale-generation'))

    def test_wrong_map_positive(self):
        check_case('wrong-map', *fixture('wrong-map'))

    def test_bad_range_positive(self):
        check_case('bad-range', *fixture('bad-range'))

    def test_frozen_full_hashes_match_independent_payloads(self):
        self.assertEqual(reference_hash(reference_payload(131101, 1)), HASHES[0])
        self.assertEqual(reference_hash(reference_payload(131101, 1, True)), HASHES[1])
        self.assertEqual(reference_hash(reference_payload(98307, 2)), HASHES[2])

    def test_empty_or_unknown_case_is_rejected(self):
        for name in ('', 'NORMAL', 'normal ', 'unknown', None, False, 0, [], {}):
            with self.subTest(case=name):
                self.assert_rejected(name, *fixture('normal'))

    def test_empty_or_non_object_reports_are_rejected(self):
        for side in (0, 1):
            for empty in ({}, [], None, '', 0, False):
                with self.subTest(side=side, value=empty):
                    reports = list(fixture('normal'))
                    reports[side] = empty
                    self.assert_rejected('normal', *reports)

    def test_every_emitted_required_field_is_required(self):
        for name in NAMES:
            for side in (0, 1):
                for field in fixture(name)[side]:
                    with self.subTest(case=name, side=side, field=field):
                        reports = fixture(name)
                        del reports[side][field]
                        self.assert_rejected(name, *reports)

    def test_numeric_fields_do_not_accept_equal_floats_or_booleans(self):
        for name in NAMES:
            for side, fields in enumerate((PARENT_NUMERIC, HOST_NUMERIC)):
                for field in fields:
                    value = fixture(name)[side][field]
                    for bad in (float(value), True, False, str(value), None, [], {}):
                        self.mutate(side, field, bad, (name,))

    def test_numeric_fields_are_unsigned_64_bit_values(self):
        for side, fields in enumerate((PARENT_NUMERIC, HOST_NUMERIC)):
            for field in fields:
                for bad in (-1, 1 << 64, 1 << 100):
                    self.mutate(side, field, bad)

    def test_native_32_bit_identity_fields_do_not_truncate(self):
        for side, field in ((0, 'childPid'), (1, 'vendorId'), (1, 'deviceId')):
            for bad in (1 << 32, UINT64_MAX):
                self.mutate(side, field, bad)

    def test_child_identity_helper_accepts_native_boundaries_without_os_calls(self):
        check_child_identity({'childPid': 1, 'childCreation': 1})
        check_child_identity({'childPid': (1 << 32) - 1, 'childCreation': UINT64_MAX})
        check_child_identity(fixture('normal')[0])

    def test_child_identity_helper_rejects_pid_before_win32_conversion(self):
        for bad in (None, True, False, 1.0, '1', [], {}, 0, -1, 1 << 32, UINT64_MAX):
            with self.subTest(pid=bad):
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    check_child_identity({'childPid': bad, 'childCreation': 1})

    def test_child_identity_helper_rejects_creation_before_win32_conversion(self):
        for bad in (None, True, False, 1.0, '1', [], {}, 0, -1, 1 << 64):
            with self.subTest(creation=bad):
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    check_child_identity({'childPid': 1, 'childCreation': bad})

    def test_child_identity_helper_requires_both_fields(self):
        for bad in ({}, {'childPid': 1}, {'childCreation': 1}, None, []):
            with self.subTest(parent=bad):
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    check_child_identity(bad)

    def test_boolean_fields_require_actual_booleans(self):
        for name in NAMES:
            for side, fields in enumerate((PARENT_BOOLEAN, HOST_BOOLEAN)):
                for field in fields:
                    value = fixture(name)[side][field]
                    for bad in (int(value), float(value), None, [], {}, '', str(value)):
                        self.mutate(side, field, bad, (name,))

    def test_expected_rejected_is_exact_opposite_boolean_in_every_case(self):
        for name in NAMES:
            correct = name != 'normal'
            for bad in (not correct, None, 0, 1, []):
                self.mutate(0, 'expectedRejected', bad, (name,))

    def test_wrong_boolean_claims_are_rejected(self):
        for name in NAMES:
            for side, fields in enumerate((PARENT_BOOLEAN, HOST_BOOLEAN)):
                for field in fields:
                    self.mutate(side, field, not fixture(name)[side][field], (name,))

    def test_schema_and_bitness_cannot_be_swapped(self):
        for side, schema in ((0, 'warvk-buffer-host/v1'), (1, 'warvk-buffer-bridge/v1')):
            for bad in (schema, '', None, 1, 'warvk-buffer-bridge/v2'):
                self.mutate(side, 'schema', bad)
            self.mutate(side, 'bits', 64 if side == 0 else 32)

    def test_wrong_rejection_reasons_are_rejected_per_side(self):
        for name in NAMES:
            for side in (0, 1):
                for error in ('None', 'Generation', 'Scope', 'Range', 'Timeout', '', None, 0):
                    if error != ERRORS[name]:
                        self.mutate(side, 'error', error, (name,))

    def test_extra_or_missing_per_side_gpu_work_is_rejected(self):
        for name in NAMES:
            for side in (0, 1):
                for field in ('accepted', 'submitted', 'gpuCompleted', 'retired', 'liveBytes'):
                    value = fixture(name)[side][field]
                    for bad in (value - 1, value + 1):
                        self.mutate(side, field, bad, (name,))

    def test_cross_process_count_compensation_is_rejected(self):
        for name in NAMES:
            for field in ('accepted', 'submitted', 'gpuCompleted', 'retired', 'liveBytes'):
                with self.subTest(case=name, field=field):
                    parent, host = fixture(name)
                    parent[field] += 1
                    host[field] -= 1
                    self.assert_rejected(name, parent, host)

    def test_cpu_ack_does_not_prove_gpu_completion(self):
        for name in NAMES:
            with self.subTest(case=name):
                parent, host = fixture(name)
                for report in (parent, host):
                    report['gpuCompleted'] = parent['cpuAcknowledged']
                    report['retired'] = parent['cpuAcknowledged']
                self.assert_rejected(name, parent, host)

    def test_gpu_completion_without_usage_retirement_is_rejected(self):
        for side in (0, 1):
            self.mutate(side, 'retired', 0)

    def test_extra_or_missing_parent_queue_accounting_is_rejected(self):
        for name in NAMES:
            for field in ('exchanges', 'published', 'cpuAcknowledged', 'hashCount', 'childExit'):
                value = fixture(name)[0][field]
                for bad in (value - 1, value + 1):
                    self.mutate(0, field, bad, (name,))

    def test_rejected_command_cannot_receive_cpu_acceptance_ack(self):
        self.mutate(0, 'cpuAcknowledged', 10, NAMES[1:])

    def test_backend_must_not_execute_one_extra_command(self):
        for name in NAMES:
            self.mutate(1, 'backendCalls', fixture(name)[1]['backendCalls'] + 1, (name,))

    def test_resource_and_transfer_accounting_is_exact(self):
        for name in NAMES:
            for field in ('created', 'destroyed', 'peakLiveBytes', 'uploadedBytes', 'verifiedBytes'):
                value = fixture(name)[1][field]
                for bad in (value - 1, value + 1, 0):
                    self.mutate(1, field, bad, (name,))

    def test_backpressure_and_queue_bound_must_be_witnessed(self):
        for bad in (0, -1, 0.5, float('inf')):
            self.mutate(0, 'fullRefusals', bad)
        for bad in (0, 262143, 263169, 1 << 30):
            self.mutate(0, 'queueBytes', bad)

    def test_nonzero_child_and_adapter_identity_is_required(self):
        for side, field in ((0, 'childPid'), (0, 'childCreation'),
                            (1, 'vendorId'), (1, 'deviceId')):
            self.mutate(side, field, 0)

    def test_parent_vulkan_and_false_teardown_claims_are_rejected(self):
        self.mutate(0, 'parentVulkanAbsent', False)
        self.mutate(1, 'normalTeardown', False, ('normal',))
        self.mutate(1, 'normalTeardown', True, NAMES[1:])

    def test_every_full_hash_word_is_checked(self):
        for name in NAMES:
            for index in range(3):
                for bit in (0, 31, 63):
                    with self.subTest(case=name, index=index, bit=bit):
                        parent, host = fixture(name)
                        parent['hashes'][index] ^= 1 << bit
                        self.assert_rejected(name, parent, host)

    def test_hashes_are_not_truncated_or_repeat_only_oracles(self):
        for name in NAMES:
            expected = fixture(name)[0]['hashes']
            for bad in ([value & 0xffffffff for value in expected],
                        [expected[0]] * 3, [1, 2, 3], [0, 0, 0], list(reversed(expected))):
                self.mutate(0, 'hashes', bad, (name,))

    def test_hash_vector_shape_and_types_are_strict(self):
        for bad in ([], [HASHES[0]], [HASHES[0], HASHES[1]], list(HASHES) + [0],
                    None, '', {}, tuple(HASHES)):
            self.mutate(0, 'hashes', bad)
        for name in NAMES:
            for index in range(3):
                value = fixture(name)[0]['hashes'][index]
                for bad in (True, False, float(value), str(value), None, -1, 1 << 64):
                    with self.subTest(case=name, index=index, value=bad):
                        parent, host = fixture(name)
                        parent['hashes'][index] = bad
                        self.assert_rejected(name, parent, host)

    def test_no_event_or_all_zero_population_is_not_success(self):
        for name in NAMES:
            with self.subTest(case=name):
                parent, host = fixture(name)
                for report in (parent, host):
                    for field in ('accepted', 'submitted', 'gpuCompleted', 'retired'):
                        report[field] = 0
                parent.update(exchanges=0, published=0, cpuAcknowledged=0, hashCount=0, hashes=[])
                host.update(backendCalls=0, created=0, destroyed=0, uploadedBytes=0, verifiedBytes=0)
                self.assert_rejected(name, parent, host)

    def test_different_case_receipts_cannot_be_mixed(self):
        for expected in NAMES:
            for other in NAMES:
                if expected != other:
                    with self.subTest(expected=expected, other=other):
                        parent, _ = fixture(expected)
                        _, host = fixture(other)
                        self.assert_rejected(expected, parent, host)


if __name__ == '__main__':
    unittest.main()
