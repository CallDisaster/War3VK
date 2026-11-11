"""Opt-in, read-only observations of one frozen OpenGL palette watcher.

Importing this module performs no Windows calls. The only live caller is the
preserved-fixture ExitMonitor after its exact process/desktop binding. No PID
CLI, debugger, privilege adjustment, remote writes, injection or window calls.
Samples are NOT atomic and never certify a healthy exit or a working message
pump. Native API latency is not a real-time bound; the original Job/watchdog
remains authoritative. A metadata/read failure is unknown, never a zero TID.
"""
from __future__ import annotations

import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import os
from pathlib import Path
import struct
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
PE_PATH = Path('C:/Windows/System32/opengl32.dll')
PDB_PATH = ROOT / 'AutoTest/artifacts/x64-presenter-r9-exit-dump2-20261022/offline-symbols/opengl32.pdb'
PE_SHA = '659BE03CF2B88B5063A35B2637D9EF38122D7F6F30DAFFF118FA41B273AA5ECE'
PDB_SHA = '309188E31218A16F8FE8C74A1E6BC70DFB48A047A185D71B981BB39D22C237B9'
GUID = '2632AC6B-D4D0-A889-43AD-9807B88C3546'
TID_RVA = 0xE4104
MAX_METADATA = 64 * 1024
MAX_DATA = 56
MAX_THREAD_OWNERS = 2
PROCESS_ACCESS = 0x0400 | 0x0010 | 0x100000
THREAD_ACCESS = 0x0040 | 0x100000
LABELS = ('pre-bound', 'pre-250', 'pre-1000', 'pre-init-complete',
          'exit-300', 'exit-1000', 'exit-3000')
START_SYMBOLS = {0x82130: 'PaletteWatcher', 0x822E0: 'PaletteWatcherProc', 0x82308: 'StartPaletteWatcher'}
MAX_GUARD_DIFFERENCES = 8
MAX_GUARD_FIELD_BYTES = 16


def need(condition, message):
    if not condition:
        raise ValueError(message)


def normalized(path):
    return str(path).replace('\\', '/').casefold()


def span(raw, offset, size):
    need(type(offset) is int and type(size) is int and 0 <= offset <= len(raw)
         and 0 <= size <= len(raw) - offset, 'file/header span out of range')
    return raw[offset:offset + size]


def unpack(fmt, raw, offset):
    return struct.unpack(fmt, span(raw, offset, struct.calcsize(fmt)))


def pe_headers(raw):
    need(span(raw, 0, 2) == b'MZ', 'DOS signature mismatch')
    pe = unpack('<I', raw, 60)[0]
    need(64 <= pe <= 4096 and span(raw, pe, 4) == b'PE\0\0', 'PE offset/signature mismatch')
    machine, count, timestamp = unpack('<HHI', raw, pe + 4)
    optional_size = unpack('<H', raw, pe + 20)[0]
    opt = pe + 24
    need(machine == 0x8664 and 0 < count <= 32 and optional_size == 240
         and unpack('<H', raw, opt)[0] == 0x20b, 'expected bounded AMD64 PE32+')
    size, header_size, checksum = unpack('<III', raw, opt + 56)
    need(64 <= header_size <= 16384 and 0 < size <= 32 * 1024 * 1024,
         'PE header/image size outside diagnostic bound')
    table = span(raw, opt + optional_size, count * 40)
    sections = []
    for i in range(count):
        row = table[i * 40:(i + 1) * 40]
        virtual, rva, disk_size, disk_offset = unpack('<IIII', row, 8)
        need(rva <= size and virtual <= size - rva, 'section exceeds image')
        sections.append(dict(name=row[:8].split(b'\0')[0].decode('ascii'),
                             rva=rva, virtual=virtual, diskSize=disk_size, diskOffset=disk_offset))
    debug_rva, debug_size = unpack('<II', raw, opt + 112 + 6 * 8)
    need(0 < debug_size <= 4096 and debug_size % 28 == 0 and debug_rva <= size
         and debug_size <= size - debug_rva, 'debug directory outside bound')
    return dict(pe=pe, machine=machine, timestamp=timestamp, size=size, checksum=checksum,
                headerSize=header_size, sections=sections, sectionTable=table,
                debugRva=debug_rva, debugSize=debug_size)


def file_rva(raw, header, rva, size):
    matches = [s for s in header['sections'] if s['rva'] <= rva
               and rva - s['rva'] <= s['diskSize'] and size <= s['diskSize'] - (rva - s['rva'])]
    need(len(matches) == 1, 'RVA is not uniquely file-backed')
    s = matches[0]
    return span(raw, s['diskOffset'] + rva - s['rva'], size)


def rsds(raw):
    need(len(raw) >= 24 and raw[:4] == b'RSDS', 'CodeView is not RSDS')
    return dict(guid=str(uuid.UUID(bytes_le=raw[4:20])).upper(), age=unpack('<I', raw, 20)[0])


def guard_field(frozen, rva):
    """Describe pinned header bytes, without deciding whether a mutation is valid."""
    dos = ('e_magic', 'e_cblp', 'e_cp', 'e_crlc', 'e_cparhdr', 'e_minalloc',
           'e_maxalloc', 'e_ss', 'e_sp', 'e_csum', 'e_ip', 'e_cs', 'e_lfarlc', 'e_ovno')
    fields = [(2 * i, 2, 'IMAGE_DOS_HEADER.' + name) for i, name in enumerate(dos)]
    fields += [(28, 8, 'IMAGE_DOS_HEADER.e_res'), (36, 2, 'IMAGE_DOS_HEADER.e_oemid'),
               (38, 2, 'IMAGE_DOS_HEADER.e_oeminfo'), (40, 20, 'IMAGE_DOS_HEADER.e_res2'),
               (60, 4, 'IMAGE_DOS_HEADER.e_lfanew')]
    pe = frozen.header['pe']
    fields += [(pe, 4, 'IMAGE_NT_HEADERS64.Signature')]
    coff = ((0, 2, 'Machine'), (2, 2, 'NumberOfSections'), (4, 4, 'TimeDateStamp'),
            (8, 4, 'PointerToSymbolTable'), (12, 4, 'NumberOfSymbols'),
            (16, 2, 'SizeOfOptionalHeader'), (18, 2, 'Characteristics'))
    fields += [(pe + 4 + offset, size, 'IMAGE_FILE_HEADER.' + name) for offset, size, name in coff]
    optional = ((0, 2, 'Magic'), (2, 1, 'MajorLinkerVersion'), (3, 1, 'MinorLinkerVersion'),
        (4, 4, 'SizeOfCode'), (8, 4, 'SizeOfInitializedData'), (12, 4, 'SizeOfUninitializedData'),
        (16, 4, 'AddressOfEntryPoint'), (20, 4, 'BaseOfCode'), (24, 8, 'ImageBase'),
        (32, 4, 'SectionAlignment'), (36, 4, 'FileAlignment'),
        (40, 2, 'MajorOperatingSystemVersion'), (42, 2, 'MinorOperatingSystemVersion'),
        (44, 2, 'MajorImageVersion'), (46, 2, 'MinorImageVersion'),
        (48, 2, 'MajorSubsystemVersion'), (50, 2, 'MinorSubsystemVersion'),
        (52, 4, 'Win32VersionValue'), (56, 4, 'SizeOfImage'), (60, 4, 'SizeOfHeaders'),
        (64, 4, 'CheckSum'), (68, 2, 'Subsystem'), (70, 2, 'DllCharacteristics'),
        (72, 8, 'SizeOfStackReserve'), (80, 8, 'SizeOfStackCommit'),
        (88, 8, 'SizeOfHeapReserve'), (96, 8, 'SizeOfHeapCommit'),
        (104, 4, 'LoaderFlags'), (108, 4, 'NumberOfRvaAndSizes'))
    fields += [(pe + 24 + offset, size, 'IMAGE_OPTIONAL_HEADER64.' + name)
               for offset, size, name in optional]
    fields += [(frozen.cv_rva, 4, 'CodeView.RSDS.Signature'),
               (frozen.cv_rva + 4, 16, 'CodeView.RSDS.Guid'),
               (frozen.cv_rva + 20, 4, 'CodeView.RSDS.Age')]
    matches = [(start, size, name) for start, size, name in fields if start <= rva < start + size]
    need(len(matches) == 1, 'guard field is absent or ambiguous')
    start, size, name = matches[0]
    return dict(name=name, rva=hex(start), bytes=size, byteOffset=rva - start)


class GuardMismatch(ValueError):
    """Bounded receipt derived only from the already-completed guard read."""
    def __init__(self, frozen, guard_index, rva, expected, actual):
        need(len(expected) == len(actual) and len(expected) <= 136, 'guard diagnostic extent mismatch')
        differences = [i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1]]
        need(bool(differences), 'guard diagnostic requires a mismatch')
        first = differences[0]
        field = guard_field(frozen, rva + first)
        field_offset = int(field['rva'], 16) - rva
        need(0 <= field_offset and field_offset + field['bytes'] <= len(actual),
             'guard field extends outside captured guard')
        count = min(field['bytes'], MAX_GUARD_FIELD_BYTES)
        self.detail = dict(guardIndex=guard_index, guardRva=hex(rva), guardBytes=len(expected),
            firstDifferingOffset=first, firstDifferingRva=hex(rva + first), mismatchingBytes=len(differences),
            expectedSha256=hashlib.sha256(expected).hexdigest().upper(),
            actualSha256=hashlib.sha256(actual).hexdigest().upper(),
            field=field, fieldBytesShown=count, fieldBytesTruncated=count != field['bytes'],
            expectedFieldHex=expected[field_offset:field_offset + count].hex(),
            actualFieldHex=actual[field_offset:field_offset + count].hex(),
            differences=[dict(offset=i, rva=hex(rva + i), expectedByte=expected[i], actualByte=actual[i],
                              field=guard_field(frozen, rva + i)['name'])
                         for i in differences[:MAX_GUARD_DIFFERENCES]],
            differencesTruncated=len(differences) > MAX_GUARD_DIFFERENCES,
            additionalRemoteReads=0, mutationLegitimacy='not-determined', guardExempted=False)
        super().__init__('loaded module guard changed at ' + hex(rva + first) + ' (' + field['name'] + ')')


def pdb_streams(raw):
    need(len(raw) <= 2 * 1024 * 1024 and raw[:32] == b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0',
         'unexpected bounded MSF7 PDB')
    block, _, blocks, directory_size, _, block_map = unpack('<6I', raw, 32)
    need(block in (512, 1024, 2048, 4096) and blocks * block == len(raw)
         and 0 < directory_size <= len(raw), 'PDB block bounds')
    count = (directory_size + block - 1) // block
    need(count * 4 <= block, 'multi-block PDB directory map not supported')
    ids = unpack('<' + 'I' * count, raw, block_map * block)
    directory = b''.join(span(raw, i * block, block) for i in ids)[:directory_size]
    streams = unpack('<I', directory, 0)[0]
    need(4 <= streams <= 4096, 'PDB stream count outside bound')
    sizes = unpack('<' + 'I' * streams, directory, 4)
    cursor, result = 4 + 4 * streams, []
    for size in sizes:
        need(size == 0xffffffff or size <= len(raw), 'PDB stream size outside bound')
        count = 0 if size == 0xffffffff else (size + block - 1) // block
        ids = unpack('<' + 'I' * count, directory, cursor) if count else ()
        cursor += 4 * count
        result.append(b''.join(span(raw, i * block, block) for i in ids)[:size])
    need(cursor == len(directory), 'PDB directory trailing bytes')
    return result


class FrozenImage:
    """Pure parsing; production load() alone supplies the fixed allowlisted files."""
    def __init__(self, pe, pdb, *, pe_sha, pdb_sha, path, expected):
        need(hashlib.sha256(pe).hexdigest().upper() == pe_sha
             and hashlib.sha256(pdb).hexdigest().upper() == pdb_sha, 'frozen PE/PDB SHA mismatch')
        header = pe_headers(pe)
        need((header['timestamp'], header['checksum'], header['size']) ==
             (expected['timestamp'], expected['checksum'], expected['size']), 'frozen PE identity mismatch')
        debug = file_rva(pe, header, header['debugRva'], header['debugSize'])
        cvs = []
        for offset in range(0, len(debug), 28):
            if unpack('<I', debug, offset + 12)[0] == 2:
                size, rva, disk_offset = unpack('<III', debug, offset + 16)
                need(24 <= size <= 1024, 'RSDS extent outside bound')
                value = file_rva(pe, header, rva, size)
                need(value == span(pe, disk_offset, size), 'RSDS file/RVA mismatch')
                cvs.append((rva, value))
        need(len(cvs) == 1, 'ambiguous PE RSDS record')
        cv_rva, cv = cvs[0]
        need(rsds(cv) == dict(guid=expected['guid'], age=expected['age']), 'PE CodeView identity mismatch')
        streams = pdb_streams(pdb)
        info, dbi = streams[1], streams[3]
        need(str(uuid.UUID(bytes_le=span(info, 12, 16))).upper() == expected['guid']
             and unpack('<I', info, 8)[0] >= expected['age']
             and unpack('<I', dbi, 8)[0] == expected['age'], 'PDB GUID/Info/DBI age mismatch')
        optional_bytes = unpack('<i', dbi, 48)[0]
        lengths = [unpack('<i', dbi, o)[0] for o in (24, 28, 32, 36, 40, 52)]
        need(all(x >= 0 for x in lengths) and optional_bytes >= 12, 'DBI substream bounds')
        optional = span(dbi, 64 + sum(lengths), optional_bytes)
        section_index = unpack('<H', optional, 10)[0]
        need(section_index < len(streams) and streams[section_index] == header['sectionTable'],
             'PDB/PE section table mismatch')
        sym_index = unpack('<H', dbi, 20)[0]
        need(sym_index < len(streams), 'PDB symbol stream absent')
        symbols, cursor, records = {}, 0, streams[sym_index]
        while cursor < len(records):
            length, kind = unpack('<HH', records, cursor)
            need(length >= 2, 'PDB record length invalid')
            row = span(records, cursor + 2, length)
            if kind == 0x110e:
                flags, offset, section = unpack('<IIH', row, 2)
                name = span(row, 12, len(row) - 12).split(b'\0')[0].decode('utf-8')
                if name in expected['symbols']:
                    need(name not in symbols and 1 <= section <= len(header['sections']), 'ambiguous symbol')
                    symbols[name] = header['sections'][section - 1]['rva'] + offset
            cursor += length + 2
        need(symbols == expected['symbols'], 'frozen PDB symbols disagree')
        self.path, self.header, self.cv_rva, self.cv = normalized(path), header, cv_rva, cv
        self.pe_sha, self.pdb_sha = pe_sha, pdb_sha
        self.header_bytes = span(pe, 0, header['headerSize'])
        self.debug_bytes = debug
        self.image_base_rva = header['pe'] + 24 + 24
        self.preferred_image_base = unpack('<Q', pe, self.image_base_rva)[0]
        need(0 < self.preferred_image_base <= (1 << 64) - header['size']
             and self.preferred_image_base % 65536 == 0, 'invalid pinned preferred ImageBase')
        self.tid_rva = expected['symbols']['tidPaletteWatcherThread']
        self.start_symbols = {r: n for n, r in symbols.items() if n != 'tidPaletteWatcherThread'}
        need(any(s['name'] == '.data' and s['rva'] <= self.tid_rva
                 and self.tid_rva + 4 <= s['rva'] + s['virtual'] for s in header['sections']),
             'watcher TID is not inside pinned data section')
        self.guards = ((0, span(pe, 0, 64)), (header['pe'], span(pe, header['pe'], 136)), (cv_rva, cv[:24]))

    def nt_guard_for_image_base(self, value):
        # Compare complete templates, not independently allowed bytes. Only the
        # observer's exact bound module base may supply the alternative value.
        expected = self.guards[1][1]
        offset = self.image_base_rva - self.header['pe']
        return expected[:offset] + struct.pack('<Q', value) + expected[offset + 8:]

    @classmethod
    def load(cls):
        expected = dict(timestamp=868189093, checksum=1026881, size=1097728, guid=GUID, age=1,
                        symbols={'tidPaletteWatcherThread': TID_RVA,
                                 **{name: rva for rva, name in START_SYMBOLS.items()}})
        need(PE_PATH.stat().st_size == 983040 and PDB_PATH.stat().st_size == 692224, 'frozen input size mismatch')
        return cls(PE_PATH.read_bytes(), PDB_PATH.read_bytes(), pe_sha=PE_SHA, pdb_sha=PDB_SHA,
                   path=PE_PATH, expected=expected)

    def receipt(self):
        return dict(pePath=self.path, peSha256=self.pe_sha, pdbSha256=self.pdb_sha, tidRva=hex(self.tid_rva),
                    sizeOfImage=self.header['size'], codeView=rsds(self.cv), startSymbols=self.start_symbols,
                    imageBaseRva=hex(self.image_base_rva), preferredImageBase=hex(self.preferred_image_base))


def validate_process(actual, bound):
    need(actual['pid'] == bound['pid'] and actual['creation100ns'] == bound['creation100ns']
         and normalized(actual['image']) == normalized(bound['image']) and actual['active'] is True,
         'exact owned process changed or exited')


def module_identity(row, frozen):
    need(normalized(row['path']) == frozen.path and row['bytes'] == frozen.header['size']
         and type(row['base']) is int and 0 < row['base'] <= (1 << 64) - row['bytes'],
         'loaded OpenGL identity/range mismatch')
    return (normalized(row['path']), row['base'], row['bytes'])


class PaletteWatcherObserver:
    def __init__(self, api, bound, frozen):
        need(type(bound.get('pid')) is int and bound['pid'] > 0
             and type(bound.get('creation100ns')) is int and bound['creation100ns'] > 0,
             'bound process identity missing')
        self.api, self.bound, self.frozen = api, dict(bound), frozen
        self.handle = None
        self.cached = None
        self.locked_image_base = None
        self.discontinued = False
        self.labels, self.threads = set(), []
        self.result = dict(schema='warvk-palette-watcher-observation/v1', acceptancePassed=False,
                           processAccess=PROCESS_ACCESS, threadAccess=THREAD_ACCESS, samples=[], errors=[],
                           metadataBytesRequested=0, dataBytesRequested=0, maxMetadataBytes=MAX_METADATA,
                           maxDataBytes=MAX_DATA, maxSamples=7, atomicSnapshot=False,
                           debuggerAttached=False, targetWritten=False, windowQueries=False,
                           threadSuspended=False, timingMayBePerturbed=True,
                           frozen=frozen.receipt(), threadOwners=[], skippedPreSamples=[])
        try:
            self.handle = api.open_process(bound['pid'])
            validate_process(api.process(self.handle), self.bound)
        except Exception:
            self.close()
            raise

    def read(self, address, size, data=False):
        field, maximum = ('dataBytesRequested', MAX_DATA) if data else ('metadataBytesRequested', MAX_METADATA)
        need(type(address) is int and 0 < address <= (1 << 64) - size, 'remote address overflow')
        need(self.result[field] + size <= maximum, 'remote read budget exhausted')
        self.result[field] += size  # Requested bytes, including failed/short reads.
        region = self.api.region(self.handle, address)
        need(region['state'] == 0x1000 and region['type'] == 0x1000000
             and region['allocationBase'] == self.cached[1]
             and region['base'] <= address and address + size <= region['base'] + region['bytes']
             and not region['protect'] & (0x100 | 1)
             and region['protect'] & 0xff in (2, 4, 8, 0x20, 0x40, 0x80), 'remote page not readable pinned MEM_IMAGE')
        raw = self.api.read(self.handle, address, size)
        need(type(raw) is bytes and len(raw) == size, 'remote read incomplete')
        return raw

    def modules(self):
        rows = self.api.modules(self.bound['pid'])
        need(len(rows) <= 2048, 'module count bound exceeded')
        wanted = [r for r in rows if normalized(r['path']).rsplit('/', 1)[-1] == 'opengl32.dll']
        presence = [dict(name=normalized(r['path']).rsplit('/', 1)[-1], path=r['path'],
                         base=hex(r['base']), bytes=r['bytes']) for r in rows
                    if normalized(r['path']).rsplit('/', 1)[-1] in ('opengl32.dll', 'vulkan-1.dll', 'nvoglv64.dll')]
        return wanted, presence

    def bind_module(self, rows):
        if len(rows) != 1:
            if self.cached is not None:
                self.discontinued = True
            raise ValueError('OpenGL module absent or ambiguous')
        actual = module_identity(rows[0], self.frozen)
        if self.cached is not None and actual != self.cached:
            self.discontinued = True
        need(not self.discontinued, 'previously bound module unloaded or changed')
        if self.cached is None:
            self.cached = actual
            try:
                header = self.read(actual[1], self.frozen.header['headerSize'])
                need(pe_headers(header) == self.frozen.header, 'loaded PE header/section identity mismatch')
                need(self.read(actual[1] + self.frozen.header['debugRva'], len(self.frozen.debug_bytes))
                     == self.frozen.debug_bytes, 'loaded debug directory mismatch')
                need(self.read(actual[1] + self.frozen.cv_rva, len(self.frozen.cv)) == self.frozen.cv,
                     'loaded RSDS mismatch')
                # Reuse the already-read full header after the PE/RSDS identity
                # checks. Accept exactly one whole ImageBase representation:
                # the pinned preferred address or this independently bound
                # module base. Nothing here proves who wrote the memory field.
                observed_base = unpack('<Q', header, self.frozen.image_base_rva)[0]
                nt_rva, disk_nt = self.frozen.guards[1]
                observed_nt = span(header, nt_rva, len(disk_nt))
                if observed_base not in (self.frozen.preferred_image_base, actual[1]):
                    raise GuardMismatch(self.frozen, 1, nt_rva, disk_nt, observed_nt)
                expected_nt = self.frozen.nt_guard_for_image_base(observed_base)
                if observed_nt != expected_nt:
                    raise GuardMismatch(self.frozen, 1, nt_rva, expected_nt, observed_nt)
                # Lock the first fully verified representation. Even switching
                # between the two otherwise valid values later is unknown.
                self.locked_image_base = observed_base
                self.result['imageBaseBinding'] = dict(
                    policy='pinned-preferred-or-bound-base-first-representation-locked',
                    representation=('preferred' if observed_base == self.frozen.preferred_image_base
                                    else 'exact-bound-loaded-base'),
                    preferredImageBase=hex(self.frozen.preferred_image_base),
                    boundModuleBase=hex(actual[1]), lockedImageBase=hex(observed_base),
                    addressSource='exact-process-module-binding-not-ImageBase-field',
                    systemMutationCauseProven=False, additionalRemoteReads=0)
            except Exception:
                self.discontinued = True
                raise
        self.guard()

    def guard(self):
        for index, (rva, expected) in enumerate(self.frozen.guards):
            try:
                if index == 1:
                    need(self.locked_image_base is not None, 'ImageBase representation not bound')
                    expected = self.frozen.nt_guard_for_image_base(self.locked_image_base)
                actual = self.read(self.cached[1] + rva, len(expected))
                if actual != expected:
                    raise GuardMismatch(self.frozen, index, rva, expected, actual)
            except Exception:
                self.discontinued = True
                raise

    def thread(self, tid):
        if tid == 0:
            return dict(status='zero-tid-no-thread-query')
        handle = None
        try:
            held = next((row for row in self.threads if row['tid'] == tid), None)
            if held:
                current = self.api.thread(held['handle'])
                need((current['pid'], current['tid'], current['creation100ns']) ==
                     (self.bound['pid'], tid, held['creation100ns']), 'retained thread identity changed')
                # A retained HANDLE identifies its historical thread, not a later
                # reuse of the same numeric TID. Never re-open that TID to guess.
                status = ('retained-live-instance' if current['active'] is True else
                          'retained-exited-instance-current-tid-owner-unknown')
                return dict(status=status, **current, start=held['start'],
                            liveOwnerObserved=current['active'] is True, atomicOwnerSnapshot=False)
            if len(self.threads) >= MAX_THREAD_OWNERS:
                return dict(status='thread-owner-budget-exhausted', liveOwnerObserved=False)
            handle = self.api.open_thread(tid)
            row = self.api.thread(handle)
            need(row['pid'] == self.bound['pid'] and row['tid'] == tid
                 and row['creation100ns'] >= self.bound['creation100ns'] and row['active'] is True,
                 'new thread is not a live exact-owned instance')
            start = dict(status='unavailable')
            try:
                address = self.api.thread_start(handle)
                need(type(address) is int and 0 < address < (1 << 64), 'invalid thread start address')
                inside = self.cached[1] <= address < self.cached[1] + self.cached[2]
                start = dict(status='observed', address=hex(address), insideBoundOpenGL=inside)
                if inside:
                    rva = address - self.cached[1]
                    start['rva'] = hex(rva)
                    if rva in self.frozen.start_symbols:
                        start['exactPublicSymbol'] = self.frozen.start_symbols[rva]
            except Exception as exc:
                start['error'] = f'{type(exc).__name__}: {exc}'
            after = self.api.thread(handle)
            need((after['pid'], after['tid'], after['creation100ns']) ==
                 (row['pid'], row['tid'], row['creation100ns']), 'thread identity changed during start query')
            held = dict(handle=handle, tid=tid, creation100ns=row['creation100ns'], start=start)
            self.threads.append(held)
            handle = None
            self.result['threadOwners'].append(dict(pid=row['pid'], tid=tid, creation100ns=row['creation100ns'], start=start))
            status = ('exact-owned-instance' if after['active'] is True else
                      'exact-owned-exited-instance-current-tid-owner-unknown')
            return dict(status=status, **after, start=start,
                        liveOwnerObserved=after['active'] is True, atomicOwnerSnapshot=False)
        except Exception as exc:
            return dict(status='unavailable-not-owner-proof', liveOwnerObserved=False,
                        error=f'{type(exc).__name__}: {exc}')
        finally:
            if handle is not None:
                self.api.close(handle)

    def sample(self, label, requested_ms=None, actual_ms=None):
        need(label in LABELS and label not in self.labels and len(self.labels) < 7, 'sample label/budget invalid')
        self.labels.add(label)
        started = time.monotonic()
        row = dict(label=label, requestedMs=requested_ms, observedMs=actual_ms, status='unknown',
                   values=[], atomicSnapshot=False)
        try:
            validate_process(self.api.process(self.handle), self.bound)
            need(not self.discontinued, 'module lifetime previously became unknown')
            modules, row['modulePresenceBefore'] = self.modules()
            self.bind_module(modules)
            row['moduleBase'] = hex(self.cached[1])
            first = struct.unpack('<I', self.read(self.cached[1] + self.frozen.tid_rva, 4, True))[0]
            row['values'].append(first)
            row['thread'] = self.thread(first)
            second = struct.unpack('<I', self.read(self.cached[1] + self.frozen.tid_rva, 4, True))[0]
            row['values'].append(second)
            self.guard()
            modules, row['modulePresenceAfter'] = self.modules()
            need(len(modules) == 1 and module_identity(modules[0], self.frozen) == self.cached,
                 'module changed across observation')
            validate_process(self.api.process(self.handle), self.bound)
            row['status'] = 'observed-equal-pair' if first == second else 'observed-changing-pair'
            row['valueCertified'] = True
        except Exception as exc:
            row.update(error=f'{type(exc).__name__}: {exc}', valueCertified=False)
            if isinstance(exc, GuardMismatch):
                row['guardMismatch'] = exc.detail
            # No rebinding after a failure involving an already validated image.
            if self.cached is not None:
                self.discontinued = True
        row['collectionMs'] = round((time.monotonic() - started) * 1000, 3)
        self.result['samples'].append(row)
        return row

    def close(self):
        for row in self.threads:
            try:
                self.api.close(row['handle'])
            except Exception as exc:
                self.result['errors'].append(f'thread-close: {exc}')
        self.threads.clear()
        if self.handle is not None:
            try:
                self.api.close(self.handle)
            except Exception as exc:
                self.result['errors'].append(f'process-close: {exc}')
            self.handle = None
        self.result['handlesClosed'] = not self.result['errors']
        # A completed monitor is not complete value coverage. Early exit,
        # unavailable metadata and skipped windows must remain separately visible.
        observed = sum(row.get('valueCertified') is True for row in self.result['samples'])
        self.result['coverage'] = dict(requestedSamples=len(self.result['samples']), observedPairs=observed,
            unknownSamples=len(self.result['samples']) - observed,
            unrequestedLabels=[label for label in LABELS if label not in self.labels],
            allSevenPairsObserved=observed == len(LABELS))


class PreExitSchedule:
    def __init__(self):
        self.done, self.skipped = set(), []

    def due(self, elapsed_ms, init_complete=False, exiting=False):
        if exiting:
            for label in LABELS[:4]:
                if label not in self.done:
                    self.done.add(label)
                    self.skipped.append(dict(label=label, reason='exit-already-observed'))
            return None
        candidates = [('pre-bound', 0), ('pre-init-complete', None)]
        if elapsed_ms >= 1000 and 'pre-250' not in self.done:
            self.done.add('pre-250')
            self.skipped.append(dict(label='pre-250', reason='missed-before-next-scheduled-window'))
        candidates += [('pre-250', 250), ('pre-1000', 1000)]
        for label, requested in candidates:
            if label not in self.done and ((label == 'pre-init-complete' and init_complete)
                    or (requested is not None and elapsed_ms >= requested)):
                self.done.add(label)
                return label, requested
        return None


class NativePaletteApi:
    """Only instantiated after opt-in and prove_host; calls are query/read only."""
    def __init__(self, query_api, module_entry_type):
        need(os.name == 'nt' and ct.sizeof(ct.c_void_p) == 8, 'x64 Windows observer required')
        self.query, self.module_entry_type, self.k = query_api, module_entry_type, query_api.k

        class Region(ct.Structure):
            _fields_ = [('base', ct.c_void_p), ('allocationBase', ct.c_void_p), ('allocationProtect', wt.DWORD),
                        ('partition', wt.WORD), ('bytes', ct.c_size_t), ('state', wt.DWORD),
                        ('protect', wt.DWORD), ('type', wt.DWORD)]
        need(ct.sizeof(Region) == 48 and Region.bytes.offset == 24, 'MEMORY_BASIC_INFORMATION ABI mismatch')
        self.Region = Region
        self.k.ReadProcessMemory.restype = wt.BOOL
        self.k.ReadProcessMemory.argtypes = [wt.HANDLE, ct.c_void_p, ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t)]
        self.k.VirtualQueryEx.restype = ct.c_size_t
        self.k.VirtualQueryEx.argtypes = [wt.HANDLE, ct.c_void_p, ct.POINTER(Region), ct.c_size_t]
        self.nt = ct.WinDLL('ntdll.dll', use_last_error=True, winmode=0x800)
        self.start_query = getattr(self.nt, 'NtQueryInformationThread', None)
        if self.start_query:
            self.start_query.restype = ct.c_long
            self.start_query.argtypes = [wt.HANDLE, ct.c_uint, ct.c_void_p, wt.ULONG, ct.POINTER(wt.ULONG)]

    def open_process(self, pid):
        return self.query.check(self.k.OpenProcess(PROCESS_ACCESS, False, pid), 'OpenProcess(owned palette read)')

    def close(self, handle):
        self.query.close(handle)

    def process(self, handle):
        return self.query.process(handle)

    def modules(self, pid):
        result = []
        for row in self.query.entries(8 | 16, pid, self.module_entry_type, 'Module32'):
            need(row.th32ProcessID == pid and len(result) < 2048, 'module scope/count mismatch')
            result.append(dict(path=row.szExePath, base=row.modBaseAddr or 0, bytes=row.modBaseSize))
        return result

    def region(self, handle, address):
        row = self.Region()
        need(self.k.VirtualQueryEx(handle, address, ct.byref(row), ct.sizeof(row)) == ct.sizeof(row),
             f'VirtualQueryEx failed win32={ct.get_last_error()}')
        return {name: getattr(row, name) for name in ('base', 'allocationBase', 'bytes', 'state', 'protect', 'type')}

    def read(self, handle, address, size):
        buffer, actual = ct.create_string_buffer(size), ct.c_size_t()
        ok = self.k.ReadProcessMemory(handle, address, buffer, size, ct.byref(actual))
        need(ok and actual.value == size, f'ReadProcessMemory failed/short win32={ct.get_last_error()} bytes={actual.value}')
        return buffer.raw

    def open_thread(self, tid):
        return self.query.check(self.k.OpenThread(THREAD_ACCESS, False, tid), 'OpenThread(palette query only)')

    def thread(self, handle):
        stamps = [wt.FILETIME() for _ in range(4)]
        self.query.check(self.k.GetThreadTimes(handle, *(ct.byref(s) for s in stamps)), 'GetThreadTimes(palette)')
        state = self.k.WaitForSingleObject(handle, 0)
        need(state in (0, 258), 'thread wait status invalid')
        return dict(pid=self.k.GetProcessIdOfThread(handle), tid=self.k.GetThreadId(handle),
                    **{key: (stamp.dwHighDateTime << 32) | stamp.dwLowDateTime for key, stamp in
                       zip(('creation100ns', 'exit100ns', 'kernel100ns', 'user100ns'), stamps)}, active=state == 258)

    def thread_start(self, handle):
        need(self.start_query is not None, 'NtQueryInformationThread unavailable')
        address, length = ct.c_void_p(), wt.ULONG()
        status = self.start_query(handle, 9, ct.byref(address), ct.sizeof(address), ct.byref(length))
        need(status >= 0 and length.value == ct.sizeof(address),
             f'thread start query unavailable status=0x{status & 0xffffffff:08X} bytes={length.value}')
        return address.value
