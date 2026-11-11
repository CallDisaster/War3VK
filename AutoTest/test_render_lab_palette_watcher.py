"""Pure PE/PDB and injected-Windows counterexamples. Never opens a real PID."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import uuid

import render_lab_palette_watcher as watcher


def fixture():
    pe = bytearray(2048)
    pe[:2] = b'MZ'
    struct.pack_into('<I', pe, 60, 0x80)
    pe[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', pe, 0x84, 0x8664, 3, 123, 0, 0, 240, 0)
    opt = 0x98
    struct.pack_into('<H', pe, opt, 0x20b)
    struct.pack_into('<Q', pe, opt + 24, 0x140000000)
    struct.pack_into('<III', pe, opt + 56, 0x4000, 512, 456)
    struct.pack_into('<II', pe, opt + 112 + 48, 0x2000, 28)
    sections = []
    for i, (name, rva) in enumerate(((b'.text', 0x1000), (b'.rdata', 0x2000), (b'.data', 0x3000))):
        row = struct.pack('<8sIIIIIIHHI', name, 512, rva, 512, 512 * (i + 1), 0, 0, 0, 0, 0)
        pe[0x188 + 40 * i:0x188 + 40 * (i + 1)] = row
        sections.append(row)
    cv = b'RSDS' + uuid.UUID(watcher.GUID).bytes_le + struct.pack('<I', 1) + b'test.pdb\0'
    struct.pack_into('<IIIII', pe, 1024 + 12, 2, len(cv), 0x2040, 1088, 0)
    pe[1088:1088 + len(cv)] = cv
    symbols = {'tidPaletteWatcherThread': 0x3010, 'PaletteWatcher': 0x1010,
               'PaletteWatcherProc': 0x1020, 'StartPaletteWatcher': 0x1030}
    records = bytearray()
    for name, rva in symbols.items():
        section, offset = (3, rva - 0x3000) if name.startswith('tid') else (1, rva - 0x1000)
        body = struct.pack('<HIIH', 0x110e, 0 if section == 3 else 2, offset, section) + name.encode() + b'\0'
        body += b'\0' * (-(len(body) + 2) % 4)
        records += struct.pack('<H', len(body)) + body
    info = struct.pack('<III', 20000404, 0, 3) + uuid.UUID(watcher.GUID).bytes_le
    dbi = bytearray(86)
    struct.pack_into('<iII', dbi, 0, -1, 19990903, 1)
    struct.pack_into('<H', dbi, 20, 5)
    struct.pack_into('<i', dbi, 48, 22)
    struct.pack_into('<H', dbi, 58, 0x8664)
    struct.pack_into('<11H', dbi, 64, *(4 if i == 5 else 0xffff for i in range(11)))
    streams = (b'', info, b'', bytes(dbi), b''.join(sections), bytes(records))
    block = 512
    blocks, allocation = [], []
    for raw in streams:
        ids = []
        for offset in range(0, len(raw), block):
            ids.append(4 + len(blocks))
            blocks.append(raw[offset:offset + block].ljust(block, b'\0'))
        allocation.append(ids)
    directory = struct.pack('<I', len(streams)) + struct.pack('<6I', *(len(s) for s in streams))
    directory += b''.join(struct.pack('<I', value) for ids in allocation for value in ids)
    pdb = bytearray((4 + len(blocks)) * block)
    pdb[:32] = b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0'
    struct.pack_into('<6I', pdb, 32, block, 1, len(pdb) // block, len(directory), 0, 2)
    struct.pack_into('<I', pdb, 2 * block, 3)
    pdb[3 * block:3 * block + len(directory)] = directory
    for i, raw in enumerate(blocks):
        pdb[(4 + i) * block:(5 + i) * block] = raw
    expected = dict(timestamp=123, checksum=456, size=0x4000, guid=watcher.GUID, age=1, symbols=symbols)
    return bytes(pe), bytes(pdb), expected


def frozen(pe=None, pdb=None, expected=None):
    standard_pe, standard_pdb, standard_expected = fixture()
    pe, pdb, expected = pe or standard_pe, pdb or standard_pdb, expected or standard_expected
    return watcher.FrozenImage(pe, pdb, pe_sha=hashlib.sha256(pe).hexdigest().upper(),
        pdb_sha=hashlib.sha256(pdb).hexdigest().upper(), path=watcher.PE_PATH, expected=expected)


class FakeApi:
    def __init__(self, profile=None):
        self.profile = profile or frozen()
        self.bound = dict(pid=100, creation100ns=1000, image='E:/frozen/host.exe', active=True)
        self.base, self.process_handle = 0x180000000, 999
        self.rows = [dict(path=str(watcher.PE_PATH), base=self.base, bytes=self.profile.header['size'])]
        pe, _, _ = fixture()
        self.memory = bytearray(self.profile.header['size'])
        self.memory[:512] = pe[:512]
        for section in self.profile.header['sections']:
            self.memory[section['rva']:section['rva'] + section['diskSize']] = pe[
                section['diskOffset']:section['diskOffset'] + section['diskSize']]
        self.tid_values = [1234, 1234]
        self.thread_row = dict(pid=100, tid=1234, creation100ns=1100, exit100ns=0,
                               kernel100ns=0, user100ns=0, active=True)
        self.start = self.base + 0x1010
        self.closed, self.reads, self.opened_threads = [], [], []
        self.on_read = self.on_modules = self.on_process = None
        self.modules_count = self.process_count = 0
        self.protect, self.region_type, self.region_state = 4, 0x1000000, 0x1000
        self.start_error = self.thread_error = None
        self.thread_query_error = None
        self.on_thread = None
        self.thread_calls = 0

    def open_process(self, pid):
        if pid != self.bound['pid']:
            raise ValueError('unexpected process open')
        return self.process_handle

    def process(self, handle):
        self.process_count += 1
        row = dict(self.bound)
        return self.on_process(self.process_count, row) if self.on_process else row

    def modules(self, pid):
        self.modules_count += 1
        rows = copy.deepcopy(self.rows)
        return self.on_modules(self.modules_count, rows) if self.on_modules else rows

    def close(self, handle):
        self.closed.append(handle)

    def region(self, handle, address):
        return dict(base=self.base, allocationBase=self.base, bytes=len(self.memory),
                    state=self.region_state, type=self.region_type, protect=self.protect)

    def read(self, handle, address, count):
        self.reads.append((address, count))
        if self.on_read:
            replacement = self.on_read(address, count)
            if replacement is not None:
                return replacement
        if address == self.base + self.profile.tid_rva:
            return struct.pack('<I', self.tid_values.pop(0) if len(self.tid_values) > 1 else self.tid_values[0])
        return bytes(self.memory[address - self.base:address - self.base + count])

    def open_thread(self, tid):
        self.opened_threads.append(tid)
        if self.thread_error:
            raise self.thread_error
        return 76 + len(self.opened_threads)

    def thread(self, handle):
        self.thread_calls += 1
        if self.thread_query_error:
            raise self.thread_query_error
        row = dict(self.thread_row)
        return self.on_thread(self.thread_calls, row) if self.on_thread else row

    def thread_start(self, handle):
        if self.start_error:
            raise self.start_error
        return self.start


class PinnedFilesTests(unittest.TestCase):
    def test_pure_valid_profile(self):
        value = frozen()
        self.assertEqual(value.tid_rva, 0x3010)
        self.assertEqual(value.start_symbols[0x1010], 'PaletteWatcher')
        self.assertEqual(value.receipt()['codeView']['guid'], watcher.GUID)

    def test_hashes_not_only_names(self):
        pe, pdb, expected = fixture()
        for kwargs in (dict(pe_sha='0' * 64, pdb_sha=hashlib.sha256(pdb).hexdigest().upper()),
                       dict(pe_sha=hashlib.sha256(pe).hexdigest().upper(), pdb_sha='0' * 64)):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                watcher.FrozenImage(pe, pdb, path=watcher.PE_PATH, expected=expected, **kwargs)

    def test_header_identity_mutations(self):
        pe, pdb, expected = fixture()
        for offset, fmt, value in ((0, '<H', 0), (60, '<I', 0xffff), (0x84, '<H', 0x14c),
                (0x86, '<H', 33), (0x88, '<I', 999), (0x98, '<H', 0x10b),
                (0x98 + 56, '<I', 0x1234), (0x98 + 64, '<I', 0)):
            changed = bytearray(pe)
            struct.pack_into(fmt, changed, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                frozen(bytes(changed), pdb, expected)

    def test_pdb_guid_age_sections_symbols_are_required(self):
        pe, pdb, expected = fixture()
        for offset, value in ((4 * 512 + 12, 0), (4 * 512 + 8, 0),
                              (5 * 512 + 8, 2), (6 * 512, ord('!')), (7 * 512 + 8, 0x99)):
            changed = bytearray(pdb)
            changed[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                frozen(pe, bytes(changed), expected)

    def test_declared_symbol_rva_and_data_section(self):
        pe, pdb, expected = fixture()
        for key in expected['symbols']:
            bad = copy.deepcopy(expected)
            bad['symbols'][key] += 4
            with self.subTest(key=key), self.assertRaises(ValueError):
                frozen(pe, pdb, bad)

    def test_truncated_msf_no_unbounded_read(self):
        _, pdb, _ = fixture()
        for size in (0, 31, 55, len(pdb) - 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                watcher.pdb_streams(pdb[:size])


class ObserverTests(unittest.TestCase):
    def observer(self, api=None):
        api = api or FakeApi()
        obj = watcher.PaletteWatcherObserver(api, api.bound, api.profile)
        self.addCleanup(obj.close)
        return api, obj

    def test_actual_pair_thread_start_and_close(self):
        api, obj = self.observer()
        row = obj.sample('pre-bound', 0, 0)
        self.assertEqual(row['status'], 'observed-equal-pair')
        self.assertEqual(row['values'], [1234, 1234])
        self.assertEqual(row['thread']['start']['exactPublicSymbol'], 'PaletteWatcher')
        self.assertFalse(row['atomicSnapshot'])
        self.assertEqual(obj.result['dataBytesRequested'], 8)
        obj.close()
        self.assertEqual(api.closed, [77, 999])
        obj.close()
        self.assertEqual(api.closed, [77, 999])

    def test_exact_process_identity_on_initial_handle(self):
        for key, value in (('pid', 101), ('creation100ns', 1001), ('image', 'E:/other.exe'), ('active', False)):
            api = FakeApi()
            api.on_process = lambda n, row, k=key, v=value: row | {k: v}
            with self.subTest(key=key), self.assertRaises(ValueError):
                watcher.PaletteWatcherObserver(api, api.bound, api.profile)
            self.assertEqual(api.closed, [999])
            self.assertEqual(api.reads, [])

    def test_after_process_changed_is_unknown_not_success(self):
        api, obj = self.observer()
        api.on_process = lambda n, row: row | {'active': False} if n >= 3 else row
        row = obj.sample('pre-bound')
        self.assertEqual(row['status'], 'unknown')
        self.assertFalse(row['valueCertified'])

    def test_zero_is_real_read_and_no_thread_lookup(self):
        api, obj = self.observer()
        api.tid_values = [0, 0]
        row = obj.sample('pre-bound')
        self.assertEqual(row['values'], [0, 0])
        self.assertTrue(row['valueCertified'])
        self.assertEqual(api.opened_threads, [])

    def test_short_read_does_not_become_zero(self):
        api, obj = self.observer()
        api.on_read = lambda address, count: b'' if address == api.base + api.profile.tid_rva else None
        row = obj.sample('pre-bound')
        self.assertEqual(row['values'], [])
        self.assertFalse(row['valueCertified'])
        self.assertEqual(obj.result['dataBytesRequested'], 4)

    def test_changing_value_is_not_atomic(self):
        api, obj = self.observer()
        api.tid_values = [1234, 4321]
        row = obj.sample('pre-bound')
        self.assertEqual(row['status'], 'observed-changing-pair')
        self.assertEqual(row['values'], [1234, 4321])

    def test_missing_module_before_first_load_can_later_be_observed(self):
        api, obj = self.observer()
        api.on_modules = lambda n, rows: [] if n == 1 else rows
        self.assertFalse(obj.sample('pre-bound')['valueCertified'])
        self.assertTrue(obj.sample('pre-250')['valueCertified'])

    def test_seen_unload_permanently_revokes_binding(self):
        api, obj = self.observer()
        self.assertTrue(obj.sample('pre-bound')['valueCertified'])
        api.on_modules = lambda n, rows: [] if n == 3 else rows
        self.assertFalse(obj.sample('pre-250')['valueCertified'])
        count = len(api.reads)
        self.assertFalse(obj.sample('pre-1000')['valueCertified'])
        self.assertEqual(len(api.reads), count)

    def test_loaded_identity_mismatch_never_reads_global(self):
        for key, value in (('path', 'C:/Other/opengl32.dll'), ('bytes', 0x5000), ('base', 2 ** 64 - 1)):
            api, obj = self.observer()
            api.rows[0][key] = value
            with self.subTest(key=key):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(obj.result['dataBytesRequested'], 0)

    def test_ambiguous_module_and_after_module_change(self):
        api, obj = self.observer()
        api.rows.append(dict(api.rows[0]))
        self.assertFalse(obj.sample('pre-bound')['valueCertified'])
        api2, obj2 = self.observer()
        api2.on_modules = lambda n, rows: [rows[0] | {'base': api2.base + 0x10000}] if n == 2 else rows
        self.assertFalse(obj2.sample('pre-bound')['valueCertified'])

    def test_loaded_header_debug_cv_corruption(self):
        for offset in (0x88, 0x188, 0x2000, 0x2044):
            api, obj = self.observer()
            api.memory[offset] ^= 1
            with self.subTest(offset=offset):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(obj.result['dataBytesRequested'], 0)

    def test_cached_small_guard_detects_change(self):
        api, obj = self.observer()
        obj.sample('pre-bound')
        api.memory[0x2044] ^= 1
        self.assertFalse(obj.sample('pre-250')['valueCertified'])
        self.assertEqual(obj.result['dataBytesRequested'], 8)

    def test_nt_image_base_mismatch_is_explained_not_exempted_with_no_extra_read(self):
        api, obj = self.observer()
        image_base_rva = api.profile.header['pe'] + 24 + 24
        # An address that is neither the pinned preferred base nor the exact
        # independently bound module base is never accepted or normalized.
        wrong_base = api.base + 0x10000
        struct.pack_into('<Q', api.memory, image_base_rva, wrong_base)
        row = obj.sample('pre-bound')
        self.assertEqual(row['status'], 'unknown')
        self.assertFalse(row['valueCertified'])
        self.assertEqual(row['values'], [])
        detail = row['guardMismatch']
        self.assertEqual(detail['guardIndex'], 1)
        self.assertEqual(detail['guardBytes'], 136)
        self.assertEqual(detail['field']['name'], 'IMAGE_OPTIONAL_HEADER64.ImageBase')
        self.assertEqual(detail['field']['rva'], hex(image_base_rva))
        self.assertEqual(detail['field']['bytes'], 8)
        self.assertEqual(detail['expectedFieldHex'], struct.pack('<Q', api.profile.preferred_image_base).hex())
        self.assertEqual(detail['actualFieldHex'], struct.pack('<Q', wrong_base).hex())
        self.assertEqual(detail['mutationLegitimacy'], 'not-determined')
        self.assertFalse(detail['guardExempted'])
        self.assertEqual(detail['additionalRemoteReads'], 0)
        expected_reads = [(api.base, api.profile.header['headerSize']),
            (api.base + api.profile.header['debugRva'], len(api.profile.debug_bytes)),
            (api.base + api.profile.cv_rva, len(api.profile.cv))]
        self.assertEqual(api.reads, expected_reads)
        self.assertEqual(obj.result['metadataBytesRequested'], sum(count for _, count in expected_reads))
        self.assertEqual(obj.result['dataBytesRequested'], 0)
        self.assertTrue(obj.discontinued)
        reads = list(api.reads)
        self.assertFalse(obj.sample('pre-250')['valueCertified'])
        self.assertEqual(api.reads, reads)

    def test_every_guard_byte_has_exact_bounded_field_attribution(self):
        profile = frozen()
        for guard_index, (rva, expected) in enumerate(profile.guards):
            for offset in range(len(expected)):
                changed = bytearray(expected)
                changed[offset] ^= 1
                with self.subTest(guard=guard_index, offset=offset):
                    error = watcher.GuardMismatch(profile, guard_index, rva, expected, bytes(changed))
                    detail = error.detail
                    self.assertEqual(detail['firstDifferingOffset'], offset)
                    self.assertEqual(detail['firstDifferingRva'], hex(rva + offset))
                    self.assertEqual(detail['mismatchingBytes'], 1)
                    self.assertEqual(detail['differences'][0]['actualByte'], changed[offset])
                    self.assertEqual(detail['expectedSha256'], hashlib.sha256(expected).hexdigest().upper())
                    self.assertEqual(detail['actualSha256'], hashlib.sha256(changed).hexdigest().upper())
                    self.assertLessEqual(detail['fieldBytesShown'], watcher.MAX_GUARD_FIELD_BYTES)
                    self.assertEqual(len(detail['expectedFieldHex']), 2 * detail['fieldBytesShown'])
                    self.assertEqual(detail['additionalRemoteReads'], 0)
                    self.assertFalse(detail['guardExempted'])

    def test_guard_diagnostic_many_differences_are_capped(self):
        profile = frozen()
        rva, expected = profile.guards[1]
        changed = bytes(value ^ 0xff for value in expected)
        detail = watcher.GuardMismatch(profile, 1, rva, expected, changed).detail
        self.assertEqual(detail['mismatchingBytes'], 136)
        self.assertEqual(len(detail['differences']), watcher.MAX_GUARD_DIFFERENCES)
        self.assertTrue(detail['differencesTruncated'])
        self.assertFalse(detail['guardExempted'])
        for field in ('expectedFieldHex', 'actualFieldHex'):
            self.assertLessEqual(len(detail[field]), 2 * watcher.MAX_GUARD_FIELD_BYTES)

    def test_dos_and_rsds_guard_mismatch_remain_unknown_and_do_not_read_global(self):
        for index, offset, name in ((0, 28, 'IMAGE_DOS_HEADER.e_res'),
                                    (2, 4, 'CodeView.RSDS.Guid')):
            api, obj = self.observer()
            self.assertTrue(obj.sample('pre-bound')['valueCertified'])
            rva, _ = api.profile.guards[index]
            api.memory[rva + offset] ^= 1
            with self.subTest(index=index):
                row = obj.sample('pre-250')
                self.assertFalse(row['valueCertified'])
                self.assertEqual(row['guardMismatch']['guardIndex'], index)
                self.assertEqual(row['guardMismatch']['field']['name'], name)
                self.assertTrue(obj.discontinued)
                self.assertEqual(obj.result['dataBytesRequested'], 8)

    def test_post_value_guard_mismatch_keeps_raw_pair_but_refuses_certification(self):
        api, obj = self.observer()
        tid_reads = 0
        def change_after_second_read(address, count):
            nonlocal tid_reads
            if address == api.base + api.profile.tid_rva:
                tid_reads += 1
                if tid_reads == 2:
                    api.memory[api.profile.header['pe'] + 48] ^= 1
        api.on_read = change_after_second_read
        row = obj.sample('pre-bound')
        self.assertEqual(row['values'], [1234, 1234])
        self.assertFalse(row['valueCertified'])
        self.assertEqual(row['status'], 'unknown')
        self.assertEqual(row['guardMismatch']['field']['name'], 'IMAGE_OPTIONAL_HEADER64.ImageBase')
        self.assertEqual(obj.result['dataBytesRequested'], 8)
        self.assertTrue(obj.discontinued)

    def test_guard_noaccess_uncommitted_nonimage(self):
        for attr, value in (('protect', 0x104), ('protect', 1), ('protect', 0x10),
                            ('region_state', 0x2000), ('region_type', 0x20000)):
            api, obj = self.observer()
            setattr(api, attr, value)
            with self.subTest(attr=attr, value=value):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(api.reads, [])

    def test_thread_reuse_other_pid_old_creation_or_exited_not_owned(self):
        for key, value in (('pid', 101), ('tid', 999), ('creation100ns', 999), ('active', False)):
            api, obj = self.observer()
            api.thread_row[key] = value
            row = obj.sample('pre-bound')
            with self.subTest(key=key):
                self.assertTrue(row['valueCertified'])
                self.assertEqual(row['thread']['status'], 'unavailable-not-owner-proof')
                self.assertEqual(obj.result['threadOwners'], [])
                self.assertIn(77, api.closed)

    def test_thread_unavailable_does_not_invent_owner(self):
        api, obj = self.observer()
        api.thread_error = OSError(87, 'not available')
        row = obj.sample('pre-bound')
        self.assertEqual(row['values'], [1234, 1234])
        self.assertEqual(row['thread']['status'], 'unavailable-not-owner-proof')

    def test_retained_exited_tid_is_historical_not_current_numeric_owner(self):
        api, obj = self.observer()
        first = obj.sample('pre-bound')
        self.assertTrue(first['thread']['liveOwnerObserved'])
        api.thread_row.update(active=False, exit100ns=1200)
        second = obj.sample('pre-250')
        self.assertTrue(second['valueCertified'])
        self.assertEqual(second['thread']['status'],
                         'retained-exited-instance-current-tid-owner-unknown')
        self.assertFalse(second['thread']['liveOwnerObserved'])
        self.assertEqual(second['thread']['creation100ns'], 1100)
        self.assertFalse(second['thread']['atomicOwnerSnapshot'])
        self.assertEqual(api.opened_threads, [1234])  # Do not bind a reused numeric TID.

    def test_exit_during_start_query_is_not_current_owner_proof(self):
        api, obj = self.observer()
        api.on_thread = lambda n, row: row | {'active': False, 'exit100ns': 1200} if n == 2 else row
        row = obj.sample('pre-bound')
        self.assertTrue(row['valueCertified'])
        self.assertEqual(row['thread']['status'], 'exact-owned-exited-instance-current-tid-owner-unknown')
        self.assertFalse(row['thread']['liveOwnerObserved'])

    def test_retained_query_failure_does_not_revoke_module_or_invent_owner(self):
        api, obj = self.observer()
        obj.sample('pre-bound')
        api.thread_query_error = OSError(5, 'thread information unavailable')
        row = obj.sample('pre-250')
        self.assertTrue(row['valueCertified'])
        self.assertEqual(row['thread']['status'], 'unavailable-not-owner-proof')
        self.assertFalse(row['thread']['liveOwnerObserved'])
        self.assertFalse(obj.discontinued)
        api.thread_query_error = None
        self.assertEqual(obj.sample('pre-1000')['thread']['status'], 'retained-live-instance')

    def test_retained_handle_identity_change_never_rebinds(self):
        api, obj = self.observer()
        obj.sample('pre-bound')
        api.thread_row['creation100ns'] = 1200
        row = obj.sample('pre-250')
        self.assertTrue(row['valueCertified'])
        self.assertEqual(row['thread']['status'], 'unavailable-not-owner-proof')
        self.assertEqual(api.opened_threads, [1234])

    def test_thread_handle_budget_never_opens_third_instance(self):
        api, obj = self.observer()
        obj.sample('pre-bound')
        api.tid_values = [2345, 2345]
        api.thread_row['tid'] = 2345
        obj.sample('pre-250')
        api.tid_values = [3456, 3456]
        api.thread_row['tid'] = 3456
        row = obj.sample('pre-1000')
        self.assertTrue(row['valueCertified'])
        self.assertEqual(row['thread']['status'], 'thread-owner-budget-exhausted')
        self.assertEqual(api.opened_threads, [1234, 2345])
        self.assertEqual(len(obj.threads), 2)

    def test_region_must_belong_to_exact_module_and_cover_entire_read(self):
        for change in ({'allocationBase': 0x190000000}, {'base': 0x180000001}, {'bytes': 1}):
            api, obj = self.observer()
            original = api.region
            api.region = lambda h, a, change=change: original(h, a) | change
            with self.subTest(change=change):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(api.reads, [])

    def test_start_query_unavailable_or_nonpinned_address(self):
        api, obj = self.observer()
        api.start_error = OSError(5, 'query unavailable')
        row = obj.sample('pre-bound')
        self.assertEqual(row['thread']['status'], 'exact-owned-instance')
        self.assertEqual(row['thread']['start']['status'], 'unavailable')
        api2, obj2 = self.observer()
        api2.start = 0x123456
        row = obj2.sample('pre-bound')
        self.assertFalse(row['thread']['start']['insideBoundOpenGL'])
        self.assertNotIn('exactPublicSymbol', row['thread']['start'])

    def test_seven_pairs_56B_cache_and_duplicate_refusal(self):
        api, obj = self.observer()
        for label in watcher.LABELS:
            self.assertTrue(obj.sample(label)['valueCertified'])
        self.assertEqual(obj.result['dataBytesRequested'], 56)
        self.assertLessEqual(obj.result['metadataBytesRequested'], 64 * 1024)
        self.assertEqual(sum(count == 512 for _, count in api.reads), 1)  # Full validation once.
        self.assertEqual(api.opened_threads, [1234])
        for label in ('pre-bound', 'anything', 'exit-5000'):
            with self.assertRaises(ValueError): obj.sample(label)
        self.assertEqual(obj.result['dataBytesRequested'], 56)

    def test_metadata_budget_and_pointer_overflow(self):
        api, obj = self.observer()
        obj.result['metadataBytesRequested'] = watcher.MAX_METADATA
        self.assertFalse(obj.sample('pre-bound')['valueCertified'])
        self.assertEqual(api.reads, [])
        with self.assertRaises(ValueError): obj.read(2 ** 64 - 1, 4)

    def test_empty_or_unknown_samples_do_not_claim_complete_coverage(self):
        api, obj = self.observer()
        obj.close()
        self.assertFalse(obj.result['coverage']['allSevenPairsObserved'])
        self.assertEqual(obj.result['coverage']['unrequestedLabels'], list(watcher.LABELS))
        api, obj = self.observer()
        api.rows = []
        obj.sample('pre-bound')
        obj.close()
        self.assertEqual(obj.result['coverage']['unknownSamples'], 1)
        self.assertEqual(obj.result['coverage']['observedPairs'], 0)
        self.assertFalse(obj.result['coverage']['allSevenPairsObserved'])


class ImageBaseRepresentationTests(unittest.TestCase):
    def observer(self, *, mapped=False, base=None, value=None):
        api = FakeApi()
        if base is not None:
            api.base = base
            api.rows[0]['base'] = base
            api.start = base + 0x1010
        if value is not None or mapped:
            struct.pack_into('<Q', api.memory, api.profile.image_base_rva,
                             api.base if value is None else value)
        obj = watcher.PaletteWatcherObserver(api, api.bound, api.profile)
        self.addCleanup(obj.close)
        return api, obj

    def test_two_exact_whole_templates_succeed_with_identical_read_budget(self):
        plain_api, plain = self.observer()
        mapped_api, mapped = self.observer(mapped=True)
        for label in watcher.LABELS:
            self.assertTrue(plain.sample(label)['valueCertified'])
            self.assertTrue(mapped.sample(label)['valueCertified'])
        self.assertEqual(plain.result['imageBaseBinding']['representation'], 'preferred')
        binding = mapped.result['imageBaseBinding']
        self.assertEqual(binding['representation'], 'exact-bound-loaded-base')
        self.assertEqual(binding['lockedImageBase'], hex(mapped_api.base))
        self.assertEqual(binding['boundModuleBase'], hex(mapped_api.base))
        self.assertEqual(binding['preferredImageBase'], hex(mapped_api.profile.preferred_image_base))
        self.assertFalse(binding['systemMutationCauseProven'])
        self.assertEqual(binding['additionalRemoteReads'], 0)
        self.assertEqual(plain_api.reads, mapped_api.reads)
        self.assertEqual(plain.result['metadataBytesRequested'], mapped.result['metadataBytesRequested'])
        self.assertLessEqual(mapped.result['metadataBytesRequested'], watcher.MAX_METADATA)
        self.assertEqual(mapped.result['dataBytesRequested'], 56)
        self.assertEqual(mapped_api.opened_threads, [1234])

    def test_preferred_equal_bound_base_remains_one_exact_representation(self):
        preferred = frozen().preferred_image_base
        api, obj = self.observer(base=preferred, mapped=True)
        self.assertTrue(obj.sample('pre-bound')['valueCertified'])
        self.assertEqual(obj.result['imageBaseBinding']['representation'], 'preferred')
        self.assertEqual(obj.locked_image_base, preferred)

    def test_wrong_zero_overflow_or_other_module_base_rejected_before_tid_read(self):
        preferred = frozen().preferred_image_base
        for value in (0, (1 << 64) - 1, preferred + 0x10000, 0x180010000, 0x190000000):
            api, obj = self.observer(value=value)
            with self.subTest(value=hex(value)):
                row = obj.sample('pre-bound')
                self.assertFalse(row['valueCertified'])
                self.assertEqual(row['status'], 'unknown')
                self.assertEqual(row['guardMismatch']['field']['name'], 'IMAGE_OPTIONAL_HEADER64.ImageBase')
                self.assertEqual(obj.result['dataBytesRequested'], 0)
                self.assertIsNone(obj.locked_image_base)
                self.assertNotIn('imageBaseBinding', obj.result)
                self.assertTrue(obj.discontinued)

    def test_bytewise_preferred_and_bound_mixtures_never_form_third_allowed_value(self):
        preferred, base = frozen().preferred_image_base, 0x7ffaf51a0000
        left, right = struct.pack('<Q', preferred), struct.pack('<Q', base)
        rejected = 0
        for mask in range(256):
            mixed = bytes(right[i] if mask & (1 << i) else left[i] for i in range(8))
            value = struct.unpack('<Q', mixed)[0]
            if value in (preferred, base):
                continue
            rejected += 1
            api, obj = self.observer(base=base, value=value)
            with self.subTest(mask=mask, value=hex(value)):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(obj.result['dataBytesRequested'], 0)
                self.assertTrue(obj.discontinued)
        self.assertGreater(rejected, 0)

    def test_every_other_nt_guard_byte_stays_exact_even_with_bound_image_base(self):
        for offset in range(136):
            if 48 <= offset < 56:
                continue
            api, obj = self.observer(mapped=True)
            api.memory[api.profile.header['pe'] + offset] ^= 1
            with self.subTest(offset=offset):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertEqual(obj.result['dataBytesRequested'], 0)
                self.assertTrue(obj.discontinued)

    def test_other_guard_regions_are_not_relaxed_in_mapped_mode(self):
        for guard_index in (0, 2):
            api, obj = self.observer(mapped=True)
            self.assertTrue(obj.sample('pre-bound')['valueCertified'])
            rva, _ = api.profile.guards[guard_index]
            api.memory[rva] ^= 1
            with self.subTest(guard=guard_index):
                row = obj.sample('pre-250')
                self.assertFalse(row['valueCertified'])
                self.assertEqual(row['guardMismatch']['guardIndex'], guard_index)
                self.assertEqual(obj.result['dataBytesRequested'], 8)
                self.assertTrue(obj.discontinued)

    def test_either_direction_representation_change_is_unknown_and_never_rebinds(self):
        for mapped in (False, True):
            api, obj = self.observer(mapped=mapped)
            self.assertTrue(obj.sample('pre-bound')['valueCertified'])
            locked = obj.locked_image_base
            new_value = api.profile.preferred_image_base if mapped else api.base
            struct.pack_into('<Q', api.memory, api.profile.image_base_rva, new_value)
            with self.subTest(mapped=mapped):
                row = obj.sample('pre-250')
                self.assertFalse(row['valueCertified'])
                self.assertEqual(row['guardMismatch']['field']['name'], 'IMAGE_OPTIONAL_HEADER64.ImageBase')
                self.assertEqual(obj.locked_image_base, locked)
                self.assertEqual(obj.result['dataBytesRequested'], 8)
                self.assertTrue(obj.discontinued)
                reads = list(api.reads)
                struct.pack_into('<Q', api.memory, api.profile.image_base_rva, locked)
                self.assertFalse(obj.sample('pre-1000')['valueCertified'])
                self.assertEqual(api.reads, reads)

    def test_initial_full_header_to_first_guard_representation_change_is_unknown(self):
        api, obj = self.observer()
        def change_before_first_nt_guard(address, count):
            if address == api.base + api.profile.header['pe'] and count == 136:
                struct.pack_into('<Q', api.memory, api.profile.image_base_rva, api.base)
        api.on_read = change_before_first_nt_guard
        row = obj.sample('pre-bound')
        self.assertFalse(row['valueCertified'])
        self.assertEqual(obj.locked_image_base, api.profile.preferred_image_base)
        self.assertEqual(row['guardMismatch']['field']['name'], 'IMAGE_OPTIONAL_HEADER64.ImageBase')
        self.assertEqual(obj.result['dataBytesRequested'], 0)
        self.assertTrue(obj.discontinued)

    def test_second_tid_read_to_post_guard_representation_change_is_unknown(self):
        api, obj = self.observer()
        reads = 0
        def change_after_second_tid(address, count):
            nonlocal reads
            if address == api.base + api.profile.tid_rva:
                reads += 1
                if reads == 2:
                    struct.pack_into('<Q', api.memory, api.profile.image_base_rva, api.base)
        api.on_read = change_after_second_tid
        row = obj.sample('pre-bound')
        self.assertEqual(row['values'], [1234, 1234])
        self.assertFalse(row['valueCertified'])
        self.assertEqual(obj.result['dataBytesRequested'], 8)
        self.assertEqual(obj.locked_image_base, api.profile.preferred_image_base)
        self.assertTrue(obj.discontinued)

    def test_exact_mapped_field_does_not_bypass_post_module_or_process_identity(self):
        for fault in ('module', 'process'):
            api, obj = self.observer(mapped=True)
            if fault == 'module':
                api.on_modules = lambda n, rows: [rows[0] | {'base': api.base + 0x10000}] if n == 2 else rows
            else:
                api.on_process = lambda n, row: row | {'creation100ns': 1001} if n >= 3 else row
            with self.subTest(fault=fault):
                self.assertFalse(obj.sample('pre-bound')['valueCertified'])
                self.assertTrue(obj.discontinued)


class ScheduleTests(unittest.TestCase):
    def test_four_pre_samples_then_nothing(self):
        s = watcher.PreExitSchedule()
        self.assertEqual(s.due(0), ('pre-bound', 0))
        self.assertIsNone(s.due(20))
        self.assertEqual(s.due(260), ('pre-250', 250))
        self.assertEqual(s.due(1050), ('pre-1000', 1000))
        self.assertEqual(s.due(1200, True), ('pre-init-complete', None))
        self.assertIsNone(s.due(4000, True))

    def test_no_pre_catchup_after_exit(self):
        s = watcher.PreExitSchedule()
        self.assertIsNone(s.due(5000, True, True))
        self.assertEqual(len(s.skipped), 4)
        self.assertIsNone(s.due(6000, True, True))
        self.assertEqual(len(s.skipped), 4)

    def test_missed_250_not_replayed_at_1000(self):
        s = watcher.PreExitSchedule()
        s.due(0)
        self.assertEqual(s.due(1500), ('pre-1000', 1000))
        self.assertEqual(s.skipped[0]['label'], 'pre-250')


class WiringTests(unittest.TestCase):
    def run_monitor(self, enabled, *, initially_exiting=False, reject_binding=False):
        """Run the real production loop, with only OS boundaries replaced."""
        import diagnose_render_host_presenter_exit as diagnostic
        with tempfile.TemporaryDirectory(prefix='warvk-palette-pure-') as directory:
            directory = Path(directory)
            palette_api = FakeApi()
            parent, host = directory / 'parent.exe', directory / 'host.exe'
            palette_api.bound['image'] = str(host)
            parent_row = dict(pid=90, creation100ns=900, image=str(parent), active=True)
            launch = dict(pid=90, creation100ns=900, actualExecutable=str(parent), argv=[str(parent)],
                          jobAtomic=True, handleListApplied=True, desktop='PalettePure', inputBefore='Default')
            witness = dict(schema='warvk-d3d9-desktop/v1', beforeGpu=True, pid=100, peerPid=90,
                           threadId=101, windowThreadId=91, desktop='PalettePure', inputDesktop='Default',
                           windowStation='WinSta0')
            if reject_binding:
                witness['inputDesktop'] = 'WrongDesktop'
            exit_line = 'D3D9 host exit-intent pid=100 strategy=normal-return code=0 processElapsedMs=1500\n'
            log = directory / 'host.log'
            log.write_text(json.dumps(witness) + '\n' + (exit_line if initially_exiting else ''), 'utf-8')
            clock = SimpleNamespace(now=0.0, init=False, exit=initially_exiting)
            opened, closed, snapshots = [], [], []

            class FakeEvent:
                def is_set(self):
                    return clock.now >= 8.0

                def wait(self, duration):
                    clock.now += duration
                    with log.open('a', encoding='utf-8') as output:
                        if not clock.init and clock.now >= 1.2:
                            clock.init = True
                            output.write('D3D9 host init-apply complete result=0 elapsedMs=1200\n')
                        if not clock.exit and clock.now >= 1.5:
                            clock.exit = True
                            output.write(exit_line)

            def open_process(access, inherit, pid):
                self.assertEqual(access, diagnostic.QUERY_PROCESS)
                self.assertFalse(inherit)
                self.assertIn(pid, (90, 100))
                opened.append((pid, access))
                return pid

            def snapshot(handle, bound, tid):
                self.assertEqual((handle, bound['pid'], tid), (100, 100, 101))
                snapshots.append(clock.now)
                return dict(skipped=False, nativeRun=False)

            query = SimpleNamespace(k=SimpleNamespace(OpenProcess=open_process),
                check=lambda result, operation: result,
                process=lambda handle: dict(parent_row if handle == 90 else palette_api.bound),
                parent_pid=lambda pid: 90, close=lambda handle: closed.append(handle), snapshot=snapshot)
            desktop = SimpleNamespace(receipt={'launches': [launch]}, desktop_name='PalettePure',
                                      input_before='Default', restricted_medium=False)
            monitor = diagnostic.ExitMonitor(desktop, parent, host, directory,
                                             palette_image=palette_api.profile if enabled else None)
            monitor.stop_event = FakeEvent()
            def adapter(api, module_entry_type):
                self.assertIs(api, query)
                self.assertIs(module_entry_type, diagnostic.ModuleEntry)
                if not enabled or reject_binding:
                    raise AssertionError('VM_READ adapter before explicit opt-in + exact binding')
                return palette_api
            with patch.object(diagnostic, 'ReadOnlyWindows', return_value=query), \
                 patch.object(diagnostic.time, 'monotonic', side_effect=lambda: clock.now), \
                 patch.object(watcher, 'NativePaletteApi', side_effect=adapter) as native:
                monitor.run()
                native_calls = native.call_count
            self.assertEqual(sorted(closed), [90, 100])
            self.assertEqual(opened, [(90, diagnostic.QUERY_PROCESS), (100, diagnostic.QUERY_PROCESS)])
            return monitor.result, palette_api, snapshots, native_calls

    def test_enabled_real_monitor_loop_emits_only_four_pre_and_three_exit_pairs(self):
        result, api, snapshots, calls = self.run_monitor(True)
        self.assertEqual(result['errors'], [])
        self.assertEqual(calls, 1)
        self.assertEqual([s['label'] for s in result['paletteWatcher']['samples']], list(watcher.LABELS))
        self.assertEqual(result['paletteWatcher']['dataBytesRequested'], 56)
        self.assertTrue(result['paletteWatcher']['handlesClosed'])
        self.assertTrue(result['paletteWatcher']['coverage']['allSevenPairsObserved'])
        self.assertEqual(len(snapshots), 3)
        self.assertEqual(len(result['samples']), 3)
        self.assertEqual(result['paletteWatcher']['skippedPreSamples'], [])
        self.assertFalse(result['paletteWatcher']['acceptancePassed'])
        self.assertEqual(result['paletteClockBasis']['hostProcessElapsedMeans'], 'wmain-entry-not-process-creation')
        self.assertIn(999, api.closed)

    def test_disabled_real_monitor_loop_does_not_create_vmread_adapter_or_reads(self):
        result, api, snapshots, calls = self.run_monitor(False)
        self.assertEqual(result['errors'], [])
        self.assertEqual(calls, 0)
        self.assertEqual(api.reads, [])
        self.assertEqual(api.opened_threads, [])
        self.assertNotIn('paletteWatcher', result)
        self.assertNotIn('paletteClockBasis', result)
        self.assertEqual(len(snapshots), 3)

    def test_real_monitor_binding_failure_never_opens_vmread(self):
        result, api, snapshots, calls = self.run_monitor(True, reject_binding=True)
        self.assertEqual(calls, 0)
        self.assertEqual(api.reads, [])
        self.assertEqual(snapshots, [])
        self.assertEqual(len(result['errors']), 1)
        self.assertIn('desktop identity disagreement', result['errors'][0])

    def test_real_monitor_never_catches_up_pre_reads_after_exit_intent(self):
        result, api, snapshots, calls = self.run_monitor(True, initially_exiting=True)
        self.assertEqual(result['errors'], [])
        self.assertEqual(calls, 1)
        self.assertEqual([s['label'] for s in result['paletteWatcher']['samples']], list(watcher.LABELS[4:]))
        self.assertEqual(result['paletteWatcher']['dataBytesRequested'], 24)
        self.assertEqual(len(result['paletteWatcher']['skippedPreSamples']), 4)
        self.assertFalse(result['paletteWatcher']['coverage']['allSevenPairsObserved'])
        self.assertEqual(len(snapshots), 3)

    def test_disabled_import_and_monitor_constructor_do_not_load_native(self):
        import diagnose_render_host_presenter_exit as diagnostic
        with patch.object(watcher, 'NativePaletteApi', side_effect=AssertionError('native forbidden')):
            monitor = diagnostic.ExitMonitor(None, None, None, None)
            self.assertIsNone(monitor.palette_image)
            self.assertNotIn('paletteWatcher', monitor.result)

    def test_opt_in_guard_cli_and_preserved_deadlines(self):
        import diagnose_render_host_presenter_exit as diagnostic
        source = Path(diagnostic.__file__).read_text('utf-8')
        self.assertIn('"--observe-palette-watcher"', source)
        self.assertIn('if args.observe_palette_watcher:', source)
        self.assertIn('palette_image = FrozenImage.load()', source)
        self.assertIn('nativeDeadlineMs=5000', source)
        self.assertIn('SNAPSHOT_AT_MS = (300, 1000, 3000)', source)
        self.assertNotIn('--pid', source)
        self.assertNotIn('WriteProcessMemory', Path(watcher.__file__).read_text('utf-8'))


if __name__ == '__main__':
    unittest.main()
