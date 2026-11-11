"""Fake metadata tests, not process/allocator attribution evidence."""
import copy
from pathlib import Path
import struct
import tempfile
import unittest

import native_game_memory_review as review
from native_game_va_sample import MEM_COMMIT, MEM_RESERVE, MEM_FREE, MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED
from test_native_game_va_sample import Native


def row(begin, end, state=MEM_FREE, kind=0, allocation=0):
    return dict(base=begin, size=end-begin, state=state, type=kind,
                allocationBase=allocation, protect=0, allocationProtect=0)


class Api:
    def __init__(self, rows=None):
        self.rows = rows or [row(0, 0x80000000)]
        self.process_id, self.is_wow64 = 123, True
        self.calls = 0
    def pid(self, process): return self.process_id
    def wow64(self, process): return self.is_wow64
    def filename(self, process, address): return r'\Device\HarddiskVolume9\game.dll'
    def query(self, process, address):
        self.calls += 1
        return copy.deepcopy(next(row for row in self.rows if row['base'] <= address < row['base']+row['size']))


class Tests(unittest.TestCase):
    def run_sample(self, native=None, api=None, clock=lambda: 0):
        return review.sample(native or Native(), _api=api or Api(), _clock=clock,
                             _limit=dict(end=0x80000000, largeAddressAware=False))

    def test_full_partition_and_unknown_private_owner(self):
        api = Api([row(0, 0x10000), row(0x10000, 0x20000, MEM_COMMIT, MEM_IMAGE, 0x10000),
                   row(0x20000, 0x40000, MEM_RESERVE, MEM_PRIVATE, 0x20000),
                   row(0x40000, 0x50000, MEM_COMMIT, MEM_MAPPED, 0x40000), row(0x50000, 0x80000000)])
        result = self.run_sample(api=api)
        self.assertTrue(result['complete'])
        self.assertEqual(result['summary']['unclassifiedAllocatedVaBytes'], 0x20000)
        self.assertEqual(len(result['summary']['mappedFiles']), 2)
        self.assertFalse(result['summary']['allocationOwnersProven'])
        self.assertEqual(sum(result['summary']['stateBytes'].values()), 0x80000000-0x10000)
        self.assertEqual(result['summary']['largestAlignedFreeHoleBytes'], 0x80000000-0x50000)

    def test_partial_identity_and_non_wow64(self):
        for which in ('pid', 'wow64', 'verification', 'pending'):
            native, api = Native(), Api()
            if which == 'pid': api.process_id = 999
            if which == 'wow64': api.is_wow64 = False
            if which == 'verification': native.failure_at = 2
            if which == 'pending': native.pending = [1]
            result = self.run_sample(native, api)
            self.assertFalse(result['complete'])
            self.assertTrue(result['totalsArePartial'])

    def test_query_error_and_deadline_do_not_certify(self):
        api = Api()
        api.rows[0]['size'] = 0
        self.assertFalse(self.run_sample(api=api)['complete'])
        values = iter([0, 3, 4])
        self.assertEqual(self.run_sample(clock=lambda: next(values))['stopReason'], 'deadline')
        values = iter([0, 0, 3])
        self.assertEqual(self.run_sample(clock=lambda: next(values))['stopReason'], 'deadline')

    def test_only_one_scan_and_unknown_mapping_name(self):
        native = Native()
        self.assertTrue(self.run_sample(native)['complete'])
        self.assertFalse(self.run_sample(native)['complete'])
        api = Api([row(0, 0x10000), row(0x10000, 0x80000000, MEM_COMMIT, MEM_MAPPED, 0x10000)])
        api.filename = lambda *args: None
        result = self.run_sample(api=api)
        self.assertEqual(result['summary']['unclassifiedAllocatedVaBytes'], 0x80000000-0x10000)

    def test_pe_flags_determine_range_not_file_name(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'war3.exe'
            for flag, limit in ((0, 0x80000000), (0x20, 0x100000000)):
                data = bytearray(90)
                data[:2] = b'MZ'
                struct.pack_into('<I', data, 60, 64)
                data[64:68] = b'PE\0\0'
                struct.pack_into('<H', data, 68, 0x14c)
                struct.pack_into('<H', data, 86, flag)
                struct.pack_into('<H', data, 88, 0x10b)
                path.write_bytes(data)
                self.assertEqual(review.address_limit(path)['end'], limit)
            path.write_bytes(b'bad')
            with self.assertRaises(ValueError): review.address_limit(path)

    def test_invalid_region_classification_rejected(self):
        for patch in (dict(state=7), dict(type=MEM_PRIVATE), dict(allocationBase=1),
                      dict(size=True), dict(base=-1)):
            api = Api()
            api.rows[0].update(patch)
            self.assertFalse(self.run_sample(api=api)['complete'])


if __name__ == '__main__':
    unittest.main()
