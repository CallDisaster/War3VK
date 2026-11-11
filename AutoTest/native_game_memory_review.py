"""Metadata-only full x86 VA map, through the retained owned process handle.

MEM_PRIVATE does not identify its allocator. Mapped filenames identify backing
files, not the creator of a private heap. No category inferred from adjacency,
size, or a GPU budget is promoted to allocator ownership or migration savings.
The sequential scan is bounded and non-atomic; no suspension/payload reads.
"""
from __future__ import annotations
import copy
import ctypes as ct
from pathlib import Path
import struct
import time

from native_game_va_sample import (_WindowsVaApi, _verify, _require, STATE_NAMES,
    TYPE_NAMES, MEM_FREE, MEM_COMMIT, MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE)

BEGIN = 0x10000
MAX_QUERIES = 32768


def address_limit(executable):
    with Path(executable).open('rb') as stream:
        dos = stream.read(64)
        _require(len(dos) == 64 and dos[:2] == b'MZ', 'invalid executable DOS header')
        offset = struct.unpack_from('<I', dos, 60)[0]
        _require(64 <= offset <= 1024*1024, 'unbounded executable PE offset')
        stream.seek(offset)
        pe = stream.read(26)
    _require(len(pe) == 26 and pe[:4] == b'PE\0\0' and
             struct.unpack_from('<H', pe, 4)[0] == 0x14c and
             struct.unpack_from('<H', pe, 24)[0] == 0x10b, 'exact x86 PE32 required')
    laa = bool(struct.unpack_from('<H', pe, 22)[0] & 0x20)
    return dict(largeAddressAware=laa, end=0x100000000 if laa else 0x80000000,
                source='exact retained-identity executable PE, WOW64 on 64-bit Windows')


class WindowsMapApi(_WindowsVaApi):
    def __init__(self):
        super().__init__()
        self.p = ct.WinDLL('psapi', use_last_error=True)
        self.p.GetMappedFileNameW.restype = ct.c_uint32
        self.p.GetMappedFileNameW.argtypes = (ct.c_void_p, ct.c_void_p, ct.c_wchar_p, ct.c_uint32)
        self.k.IsWow64Process.restype = ct.c_int
        self.k.IsWow64Process.argtypes = (ct.c_void_p, ct.POINTER(ct.c_int))

    def wow64(self, process):
        value = ct.c_int()
        _require(self.k.IsWow64Process(process, ct.byref(value)), 'IsWow64Process failed')
        return bool(value.value)

    def query(self, process, address):
        row = super().query(process, address)
        row.update(allocationBase=int(self._info.AllocationBase or 0),
                   protect=int(self._info.Protect), allocationProtect=int(self._info.AllocationProtect))
        return row

    def filename(self, process, address):
        value = ct.create_unicode_buffer(32768)
        size = self.p.GetMappedFileNameW(process, address, value, len(value))
        if not size or size >= len(value)-1:
            return None
        return value.value


def summarize(regions):
    states = {name: 0 for name in STATE_NAMES.values()}
    types = {name: dict(committed=0, reserved=0) for name in TYPE_NAMES.values()}
    files, unknown, holes, allocations = {}, 0, [], {}
    for row in regions:
        size = row['end'] - row['begin']
        state = STATE_NAMES[row['state']]
        states[state] += size
        if row['state'] == MEM_FREE:
            if holes and holes[-1][1] == row['begin']:
                holes[-1][1] = row['end']
            else:
                holes.append([row['begin'], row['end']])
            continue
        types[TYPE_NAMES[row['type']]][state] += size
        allocations.setdefault(row['allocationBase'], 0)
        allocations[row['allocationBase']] += size
        name = row.get('mappedFile')
        if row['type'] == MEM_PRIVATE or not name:
            unknown += size
        else:
            key = (TYPE_NAMES[row['type']], name.casefold())
            total = files.setdefault(key, dict(type=key[0], mappedFile=name, committed=0, reserved=0))
            total[state] += size
    usable = [max(0, end - ((begin+65535) & ~65535)) for begin, end in holes]
    largest = max(usable, default=0)
    _require(sum(sum(row.values()) for row in types.values()) == states['committed'] + states['reserved'],
             'type and state VA accounting differs')
    _require(sum(row['committed']+row['reserved'] for row in files.values())+unknown ==
             states['committed']+states['reserved'], 'mapped file partition differs')
    return dict(stateBytes=states, typeBytes=types, mappedFiles=list(files.values()),
                allocationBaseCount=len(allocations), unclassifiedAllocatedVaBytes=unknown,
                freeRegionCount=len(holes), largestAlignedFreeHoleBytes=largest,
                freeOutsideLargestHoleBytes=states['free']-largest,
                usableHoleCounts={str(n): sum(value >= n for value in usable)
                                  for n in (65536, 262144, 1048576, 16777216, 67108864)},
                allocationOwnersProven=False, migrationSavingsProven=False)


def sample(native, *, budget_seconds=2.0, _api=None, _clock=time.monotonic, _limit=None):
    result = dict(schema='warvk-native-full-va-review/v1', complete=False,
                  snapshotAtomic=False, payloadRead=False, targetSuspended=False,
                  processReopened=False, queries=0, regions=[], identityVerifiedBefore=False,
                  identityVerifiedAfter=False, stopReason='qualification')
    process, expected, api = native.process, copy.deepcopy(native.expected), None
    started, cursor = _clock(), BEGIN
    try:
        _require(type(budget_seconds) in (int, float) and 0 < budget_seconds <= 5,
                 'bounded query deadline required')
        _require(not getattr(native, '_full_va_review_attempted', False), 'full VA review already attempted')
        native._full_va_review_attempted = True
        api = _api or WindowsMapApi()
        _verify(native, api, process, expected)
        _require(api.wow64(process), 'WOW64 target required')
        limit = _limit or address_limit(expected['actualExecutable'])
        _require(limit['end'] in (0x80000000, 0x100000000), 'x86 VA limit')
        result.update(identityVerifiedBefore=True, processIdentity=expected, addressLimit=limit,
                      rangeBegin=BEGIN, rangeEnd=limit['end'])
        names = {}
        while cursor < limit['end']:
            if _clock()-started >= budget_seconds:
                result['stopReason'] = 'deadline'
                break
            if result['queries'] >= MAX_QUERIES:
                result['stopReason'] = 'query-limit'
                break
            row = api.query(process, cursor)
            result['queries'] += 1
            _require(all(type(row.get(key)) is int for key in
                         ('base', 'size', 'state', 'type', 'allocationBase', 'protect', 'allocationProtect')),
                     'invalid region metadata')
            _require(0 <= row['base'] <= cursor < row['base']+row['size'] <= 1 << 64 and
                     row['size'] > 0 and row['state'] in STATE_NAMES, 'region fails progress/extent')
            free = row['state'] == MEM_FREE
            _require((row['type'] == 0 and row['allocationBase'] == 0) if free else
                     (row['type'] in TYPE_NAMES and 0 < row['allocationBase'] <= row['base']),
                     'invalid allocation identity/type')
            end = min(limit['end'], row['base']+row['size'])
            row.update(begin=cursor, end=end)
            if row['type'] in (MEM_IMAGE, MEM_MAPPED):
                key = row['allocationBase']
                if key not in names:
                    names[key] = api.filename(process, cursor)
                row['mappedFile'] = names[key]
            result['regions'].append(row)
            cursor = end
        else:
            result['stopReason'] = 'range-complete'
    except Exception as error:
        result.update(stopReason='error', error=f'{type(error).__name__}: {error}')
    finally:
        if result['identityVerifiedBefore']:
            try:
                _verify(native, api, process, expected)
                result['identityVerifiedAfter'] = True
            except Exception as error:
                result.update(stopReason='identity-changed', identityError=str(error))
        result.update(nextAddress=cursor, coveredBytes=cursor-BEGIN, elapsedSeconds=_clock()-started)
        if result['elapsedSeconds'] >= budget_seconds and result['stopReason'] == 'range-complete':
            result['stopReason'] = 'deadline'
    result['summary'] = summarize(result['regions'])
    result['complete'] = (result['stopReason'] == 'range-complete' and
                          result['identityVerifiedBefore'] and result['identityVerifiedAfter'])
    result['totalsArePartial'] = not result['complete']
    return result
