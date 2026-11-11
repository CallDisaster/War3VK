"""CPU-only counterexamples against the real RH2 GPU receipt checker.

Run with ``python -B AutoTest/test_render_host_gpu_gate.py``. No compilation,
GPU calls, game launch, fixture files or monkeypatched gate implementation.
Passing here validates rejection predicates, not actual GPU execution.
"""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_render_host_gpu_gate import CASES, check_case, oracle_image_hash


COLORS = ((255, 32, 16, 255), (16, 255, 32, 255),
          (32, 16, 255, 255), (255, 255, 16, 255))
CLEAR = (4, 8, 16, 255)
# Frozen contract vectors, not values obtained from the checker under test.
# At 64x64 the quad covers [11,53)x[11,53) pixel centres. At 96x80 it
# covers [17,79)x[14,66); none of the quad edges passes through a centre.
RECTS = {(64, 64): (11, 53, 11, 53), (96, 80): (17, 79, 14, 66)}
DIGESTS = {
    (64, 64): ('c2c6425b9d5ca61d', 'f563a9e56dd207a5',
               '547200ed1cfc8a7d', '2d17f1d49e1c4ee5'),
    (96, 80): ('30d0c4e97270cc15', '3a6ced2701ab7025',
               '96680e767c26e275', '89c89166ba57dfa5'),
}


def reference_image(width, height, variant):
    """Integer-rectangle reference independent of the gate's NDC predicate."""
    left, right, top, bottom = RECTS[width, height]
    image = bytearray(bytes(CLEAR) * (width * height))
    row = bytes(COLORS[variant]) * (right - left)
    for y in range(top, bottom):
        image[4 * (y * width + left):4 * (y * width + right)] = row
    return image


def fnv1a(image):
    value = 0xcbf29ce484222325
    for byte in image:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f'{value:016x}'


def stats(draws, width, height, *, importer):
    readback_bytes = width * height * 4 if importer else 0
    submits = draws if importer else 1 + 2 * draws
    return {
        'device': 'fixture-only, not a GPU run',
        'deviceUUID': '01' * 16, 'driverUUID': '02' * 16,
        'ownedDeviceBytes': readback_bytes if importer else 65536 + 256,
        'sharedImageBytes': 65536,
        'geometryBytes': 0 if importer else 256,
        'geometryPayloadBytes': 0 if importer else 56,
        'readbackBytes': readback_bytes,
        'uploadStagingPeakBytes': 0 if importer else 256,
        'drawCount': 0 if importer else draws,
        'submitCount': submits, 'fenceWaitCount': submits,
        'consumerWaitCount': 0 if importer else draws,
        'readbackCount': draws if importer else 0,
        'imageExternalFeatures': 6, 'semaphoreExternalFeatures': 3,
        'faulted': False,
    }


def fixture(name):
    if name not in CASES:
        raise ValueError(name)
    full = name == 'normal'
    incompatible = name == 'wrong-adapter'
    graceful = full or incompatible
    accepted = 38 if full else 4
    client = {
        'case': name, 'bits': 32, 'peerVerified': True,
        'failure': '', 'childPid': 4242, 'childCreation': 123456789,
        'childExit': 0 if graceful else 2,
        'exchanges': accepted, 'adapterMismatchRejected': incompatible,
        'frames': [], 'imports': [],
    }
    host = {
        'bits': 64, 'peerVerified': True, 'failure': '',
        'closed': graceful, 'accepted': accepted,
        'protocol': {'normal': 'None', 'wrong-adapter': 'None',
                     'bad-generation': 'Scope', 'duplicate-frame': 'Frame',
                     'bad-nonce': 'Nonce'}[name],
        'epochs': [],
    }
    for process in (client, host):
        process['validation'] = {'enabled': False, 'instances': 0,
                                 'errors': 0, 'warnings': 0}
        for key in ('memoryBefore', 'memoryActive', 'memoryAfter'):
            process[key] = {'valid': True}
    for epoch in range(1, 3 if full else 2):
        width, height = (64, 64) if epoch == 1 else (96, 80)
        count = 8 if full else (0 if incompatible else 1)
        host['epochs'].append(stats(count, width, height, importer=False))
        if not incompatible:
            client['imports'].append(stats(count, width, height, importer=True))
        for frame in range(1, count + 1):
            variant = (frame - 1) % 4
            client['frames'].append({
                'epoch': epoch, 'frame': frame, 'variant': variant,
                'center': list(COLORS[variant]), 'corner': list(CLEAR),
                'fnv64': DIGESTS[width, height][variant],
            })
    return client, host


class GpuReceiptGateTests(unittest.TestCase):
    def assert_rejected(self, name, client, host):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            check_case(name, client, host)

    def assert_positive(self, name):
        client, host = fixture(name)
        check_case(name, client, host)

    def test_declared_case_population(self):
        self.assertEqual(set(CASES), {'normal', 'bad-generation',
            'duplicate-frame', 'bad-nonce', 'wrong-adapter'})

    def test_normal_positive(self):
        self.assert_positive('normal')

    def test_bad_generation_positive(self):
        self.assert_positive('bad-generation')

    def test_duplicate_frame_positive(self):
        self.assert_positive('duplicate-frame')

    def test_bad_nonce_positive(self):
        self.assert_positive('bad-nonce')

    def test_wrong_adapter_positive(self):
        self.assert_positive('wrong-adapter')

    def test_full_image_oracle_matches_frozen_vectors(self):
        for (width, height), hashes in DIGESTS.items():
            for variant, expected in enumerate(hashes):
                with self.subTest(extent=(width, height), variant=variant):
                    self.assertEqual(fnv1a(reference_image(width, height, variant)), expected)
                    self.assertEqual(oracle_image_hash(width, height, variant), expected)

    def test_interior_pixel_mutation_with_samples_unchanged_is_rejected(self):
        client, host = fixture('bad-generation')
        image = reference_image(64, 64, 0)
        image[4 * (12 * 64 + 12)] ^= 1
        centre = 4 * (32 * 64 + 32)
        self.assertEqual(tuple(image[centre:centre + 4]), COLORS[0])
        self.assertEqual(tuple(image[:4]), CLEAR)
        client['frames'][0]['fnv64'] = fnv1a(image)
        self.assert_rejected('bad-generation', client, host)

    def test_missing_quad_edge_with_samples_unchanged_is_rejected(self):
        client, host = fixture('bad-generation')
        image = reference_image(64, 64, 0)
        for y in range(11, 53):
            image[4 * (y * 64 + 11):4 * (y * 64 + 12)] = bytes(CLEAR)
        client['frames'][0]['fnv64'] = fnv1a(image)
        self.assert_rejected('bad-generation', client, host)

    def test_clear_only_image_with_forged_samples_is_rejected(self):
        client, host = fixture('bad-generation')
        client['frames'][0]['fnv64'] = fnv1a(bytes(CLEAR) * (64 * 64))
        self.assert_rejected('bad-generation', client, host)

    def test_wrong_variant_full_image_is_rejected(self):
        client, host = fixture('bad-generation')
        client['frames'][0]['fnv64'] = DIGESTS[64, 64][1]
        self.assert_rejected('bad-generation', client, host)

    def test_repeat_only_fake_digests_do_not_replace_full_image_oracle(self):
        client, host = fixture('normal')
        for frame in client['frames']:
            frame['fnv64'] = f"{frame['variant'] + 1:016x}"
        self.assert_rejected('normal', client, host)

    def test_faulted_host_is_rejected_in_every_case(self):
        for name in CASES:
            with self.subTest(case=name):
                client, host = fixture(name)
                host['epochs'][0]['faulted'] = True
                self.assert_rejected(name, client, host)

    def test_faulted_importer_is_rejected_in_every_applicable_case(self):
        for name in CASES:
            client, host = fixture(name)
            if client['imports']:
                with self.subTest(case=name):
                    client['imports'][0]['faulted'] = True
                    self.assert_rejected(name, client, host)

    def test_extra_host_submit_is_rejected_in_every_case(self):
        for name in CASES:
            with self.subTest(case=name):
                client, host = fixture(name)
                host['epochs'][0]['submitCount'] += 1
                self.assert_rejected(name, client, host)

    def test_extra_importer_submit_is_rejected_in_every_applicable_case(self):
        for name in CASES:
            client, host = fixture(name)
            if client['imports']:
                with self.subTest(case=name):
                    client['imports'][0]['submitCount'] += 1
                    self.assert_rejected(name, client, host)

    def test_missing_host_fence_wait_is_rejected_in_every_case(self):
        for name in CASES:
            with self.subTest(case=name):
                client, host = fixture(name)
                host['epochs'][0]['fenceWaitCount'] -= 1
                self.assert_rejected(name, client, host)

    def test_missing_importer_fence_wait_is_rejected_in_every_applicable_case(self):
        for name in CASES:
            client, host = fixture(name)
            if client['imports']:
                with self.subTest(case=name):
                    client['imports'][0]['fenceWaitCount'] -= 1
                    self.assert_rejected(name, client, host)

    def test_unobserved_exchanges_are_rejected_in_every_case(self):
        for name in CASES:
            with self.subTest(case=name):
                client, host = fixture(name)
                client['exchanges'] -= 1
                self.assert_rejected(name, client, host)

    def test_bad_packet_cannot_hide_gpu_work_behind_valid_frame_counts(self):
        client, host = fixture('bad-generation')
        for item in (host['epochs'][0], client['imports'][0]):
            item.update(submitCount=999, fenceWaitCount=0, faulted=True)
        self.assert_rejected('bad-generation', client, host)

    def test_wrong_protocol_rejection_is_rejected(self):
        client, host = fixture('bad-generation')
        host['protocol'] = 'Nonce'
        self.assert_rejected('bad-generation', client, host)

    def test_extra_accepted_command_is_rejected(self):
        for name in CASES:
            with self.subTest(case=name):
                client, host = fixture(name)
                host['accepted'] += 1
                self.assert_rejected(name, client, host)

    def test_generic_adapter_exception_cannot_replace_typed_witness(self):
        client, host = fixture('wrong-adapter')
        client['adapterMismatchRejected'] = False
        self.assert_rejected('wrong-adapter', client, host)

    def test_fault_output_cannot_be_accepted_as_protocol_rejection(self):
        client, host = fixture('bad-nonce')
        host['failure'] = 'injected unrelated driver exception'
        self.assert_rejected('bad-nonce', client, host)

    def test_missing_actual_draw_readback_or_consumer_wait_is_rejected(self):
        original_client, original_host = fixture('bad-generation')
        for owner, key in (('host', 'drawCount'), ('host', 'consumerWaitCount'),
                           ('client', 'readbackCount')):
            with self.subTest(owner=owner, counter=key):
                client, host = copy.deepcopy((original_client, original_host))
                item = host['epochs'][0] if owner == 'host' else client['imports'][0]
                item[key] = 0
                self.assert_rejected('bad-generation', client, host)

    def test_counterbalancing_epochs_cannot_hide_extra_gpu_work(self):
        client, host = fixture('normal')
        host['epochs'][0]['submitCount'] += 1
        host['epochs'][1]['submitCount'] -= 1
        self.assert_rejected('normal', client, host)


if __name__ == '__main__':
    unittest.main()
