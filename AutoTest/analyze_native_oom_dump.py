"""Read a native Storm OOM dump without an ExceptionStream, never edit it.

Blizzard's private 0x1000 stream is not known by the minidump dependency. Only
the in-memory directory copy maps unknown types to Unused; retained original
bytes/hash and the list of omitted streams are explicit in the result. Stacks
are bounded EBP chains, not a complete unwind of optimized code. No VA census
or causal TLS/OOM claim is inferred from missing dump pages.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import logging
import struct
from pathlib import Path

from minidump.constants import MINIDUMP_STREAM_TYPE
from minidump.minidumpfile import MinidumpFile
from analyze_war3_crash_dump import (module_rows, parse_exception_context,
                                    symbolize, walk_ebp_chain, read_bytes)


def parse_copy(raw):
    if len(raw) < 32 or raw[:4] != b"MDMP":
        raise ValueError("not a complete minidump header")
    count, offset = struct.unpack_from("<II", raw, 8)
    if count > 256 or offset < 32 or offset + count * 12 > len(raw):
        raise ValueError("invalid stream directory")
    known = {x.value for x in MINIDUMP_STREAM_TYPE}
    copy = bytearray(raw)
    streams, ignored = [], []
    for i in range(count):
        at = offset + i * 12
        kind, size, rva = struct.unpack_from("<III", raw, at)
        if rva + size > len(raw):
            raise ValueError("truncated stream")
        row = dict(type=kind, size=size, rva=rva)
        streams.append(row)
        if kind not in known:
            ignored.append(row)
            struct.pack_into("<I", copy, at, 0)
    logging.disable(logging.CRITICAL)
    return MinidumpFile.parse_bytes(copy), streams, ignored


def analyze(path):
    raw = path.read_bytes()
    dump, streams, ignored = parse_copy(raw)
    modules = module_rows(dump)
    reader = dump.get_reader()
    threads = []
    for thread in dump.threads.threads:
        ctx = parse_exception_context(path, thread.ThreadContext)
        frames = walk_ebp_chain(reader, ctx.Ebp, modules, limit=48)
        for frame in frames:
            data = read_bytes(reader, frame['framePointer'] + 8, 32)
            frame['followingDwordsNotTypedArguments'] = (
                [f"0x{x:08X}" for x in struct.unpack('<8I', data)] if data else None)
        threads.append(dict(tid=thread.ThreadId, teb=thread.Teb,
            instruction=symbolize(ctx.Eip, modules),
            registers={n: int(getattr(ctx, n)) for n in
                ('Eax','Ebx','Ecx','Edx','Esi','Edi','Esp','Ebp','Eip')}, frames=frames))
    vm = None
    for row in streams:
        if row['type'] != 22:
            continue
        data = raw[row['rva']:row['rva'] + row['size']]
        if len(data) != 152:
            continue
        revision, flags, faults = struct.unpack_from('<HHI', data)
        if revision != 2:
            continue
        # WinSDK minidumpapiset.h MINIDUMP_PROCESS_VM_COUNTERS_2; optional
        # values are exposed only when their source flag is actually present.
        names = ('peakWorkingSet','workingSet','quotaPeakPaged','quotaPaged',
                 'quotaPeakNonpaged','quotaNonpaged','pagefile','peakPagefile',
                 'peakVirtual','virtual','private','privateWorkingSet','sharedCommit',
                 'jobSharedCommit','jobPrivateCommit','jobPeakPrivateCommit',
                 'jobPrivateCommitLimit','jobTotalCommitLimit')
        values = struct.unpack_from('<18Q', data, 8)
        masks = (1,) * 8 + (2, 2, 4, 8, 8) + (16,) * 5
        vm = dict(revision=revision, flags=flags, pageFaultCount=faults,
                  bytes={name: value for name, value, mask in zip(names, values, masks) if flags & mask})
    return dict(schema='warvk-native-oom-dump/v1',
        source=dict(path=str(path.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest().upper()),
        streams=streams, ignoredPrivateStreams=ignored, modules=modules,
        hasExceptionStream=dump.exception is not None,
        hasMemoryInfo=dump.memory_info is not None, threadCount=len(threads), threads=threads,
        processVmCounters=vm,
        sourceModified=False, fullUnwind=False, lowVaExhaustionCertified=False,
        tlsCausalityCertified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.dump)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: result[k] for k in ('threadCount','hasExceptionStream','hasMemoryInfo','ignoredPrivateStreams')}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
