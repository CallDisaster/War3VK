#!/usr/bin/env python3
"""Independent Python struct golden and codec acceptance test for WVE1 wire.

Part of E1 CPU forensic history offload (2026-09-16).
Scope: pure Python standard library (struct, unittest, hashlib, json, pathlib, argparse).
Does NOT call production Record, does NOT spawn processes, does NOT touch game/GPU.
Zero disk I/O on import. Optional --output creates a fresh directory only;
running without --output performs in-memory verification only.
"""

import argparse
import binascii
import hashlib
import json
import struct
import sys
import unittest
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]

# Wire Constants from WVE1 specification
HEADER_BYTES = 80
EVENT_BYTES = 392
MAX_EVENTS = 160
MAX_PACKET_BYTES = HEADER_BYTES + MAX_EVENTS * EVENT_BYTES
MAX_HISTORY_EVENTS = 262144
NO_TRIGGER = 0xFFFFFFFFFFFFFFFF
MAGIC = b"WVE1"
MAJOR = 1
MINOR = 0

OP_BEGIN = 1
OP_DATA = 2
OP_SEAL = 3

GOLDEN_HEX = (
    "575645310100000002000000500000008801000001000000efcdab8967452301"
    "2a00000000000000640000000000000000000000000000000000000000000000"
    "002000000000000000000000000000007b00000000000000efcdab8967452301"
    "1032547698badcfe07060504030201008877665544332211f401000000000000"
    "030000000000000002000000000000005713000012000000776172766b2e676f"
    "6c64656e2e6576656e742e7631000000000000000000000004a003a002a001a0"
    "04b003b002b001b004c003c002c001c004d003d002d001d004e003e002e001e0"
    "04f003f002f001f0efcdab896745230167452301efcdab89bebafecaefbeadde"
    "ffffffff0000000000000000fffffffff0debc9a785634120000803f00000040"
    "000000c00000803e000080bf0000000000000080ffff7f7f0000800000004000"
    "0000c8420000c8c20000003fabaaaa3edb0f494054f82d40efbeaddebebafeca"
    "0df0adbacefaedfe0403020100ff00ffff00ff0055555555aaaaaaaa78563412"
    "f0debc9a0100000000000080ffffffffffffff7fffff00002000001021000010"
    "2200001023000010240000102500001026000010270000102800001029000010"
    "2a0000102b0000102c0000102d0000102e0000102f000010"
)

GOLDEN_BYTES_SHA256 = "2401882cff8cb5e50d23911ce278168375f5e33cccfe76f36d9a808c2ffa11ee"


@dataclass
class Header:
    op: int = OP_BEGIN
    count: int = 0
    session: int = 0
    ordinal: int = 0
    trigger: int = NO_TRIGGER
    attempted: int = 0
    lost: int = 0
    capacity: int = 0
    reason: int = 0
    accepted: int = 0


@dataclass
class Event:
    sequence: int = 0
    session: int = 0
    parent: int = 0
    qpc: int = 0
    owner: int = 0
    frame: int = 0
    map_epoch: int = 0
    device_epoch: int = 0
    thread: int = 0
    kind: int = 7  # Camera
    label: bytes = b"\x00" * 32
    data: List[int] = field(default_factory=lambda: [0] * 12)
    bits: List[int] = field(default_factory=lambda: [0] * 48)


HEADER_FMT = "<4sHHHHIIIQQQQQIIQ"
EVENT_FMT = "<QQQQQQQQII32s12Q48I"


class WireError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


def validate_header(h: Header) -> None:
    if h.op not in (OP_BEGIN, OP_DATA, OP_SEAL):
        raise WireError("Opcode", f"invalid op {h.op}")
    if h.capacity < 4 or h.capacity > MAX_HISTORY_EVENTS:
        raise WireError("Capacity", f"invalid capacity {h.capacity}")
    if h.session == 0:
        raise WireError("Session", "session must be non-zero")
    if h.ordinal == 0:
        raise WireError("Ordinal", "ordinal must be non-zero")
    if h.op == OP_BEGIN and h.ordinal != 1:
        raise WireError("Ordinal", "Begin packet ordinal must be 1")
    if h.op == OP_BEGIN and h.trigger != NO_TRIGGER:
        raise WireError("Trigger", "Begin packet trigger must be NoTrigger")
    if h.op == OP_SEAL and h.trigger != NO_TRIGGER and h.trigger > h.accepted:
        raise WireError("Trigger", "Seal trigger must be <= accepted")

    if h.count > MAX_EVENTS:
        raise WireError("Shape", f"count {h.count} exceeds max {MAX_EVENTS}")
    if h.op == OP_BEGIN and (h.count != 0 or h.reason != 0):
        raise WireError("Shape", "Begin packet count and reason must be 0")
    if h.op == OP_DATA and (h.count == 0 or h.reason != 0):
        raise WireError("Shape", "Data packet count must be 1..160 and reason must be 0")
    if h.op == OP_SEAL and (h.count != 0 or h.reason < 1 or h.reason > 4):
        raise WireError("Shape", "Seal packet count must be 0 and reason must be 1..4")

    if h.op == OP_BEGIN and (h.attempted != 0 or h.lost != 0 or h.accepted != 0):
        raise WireError("Totals", "Begin packet attempted/lost/accepted must be 0")
    if h.op == OP_DATA and (h.attempted != 0 or h.lost != 0 or h.accepted != 0):
        raise WireError("Totals", "Data packet attempted/lost/accepted must be 0")
    if h.op == OP_SEAL and (h.accepted > h.attempted or h.lost != h.attempted - h.accepted):
        raise WireError("Totals", "Seal packet totals mismatch")


def validate_event(ev: Event, header_session: Optional[int] = None) -> None:
    if ev.sequence == 0 or ev.sequence == 0xFFFFFFFFFFFFFFFF:
        raise WireError("EventValue", f"invalid sequence {ev.sequence}")
    if ev.session == 0:
        raise WireError("Session", "event session must be non-zero")
    if header_session is not None and ev.session != header_session:
        raise WireError("Session", f"event session {ev.session} != header session {header_session}")
    if ev.kind < 1 or ev.kind > 18:
        raise WireError("EventValue", f"kind {ev.kind} out of range 1..18")
    if b"\x00" not in ev.label:
        raise WireError("EventValue", "label must contain NUL terminator")
    if len(ev.label) != 32:
        raise WireError("EventValue", "label bytes length must be 32")
    if len(ev.data) != 12:
        raise WireError("EventValue", "data length must be 12")
    if len(ev.bits) != 48:
        raise WireError("EventValue", "bits length must be 48")


def pack_packet(h: Header, events: List[Event]) -> bytes:
    validate_header(h)
    if h.count != len(events):
        raise WireError("Shape", f"header count {h.count} != events len {len(events)}")
    for ev in events:
        validate_event(ev, h.session)

    payload_bytes = h.count * EVENT_BYTES
    header_raw = struct.pack(
        HEADER_FMT,
        MAGIC,
        MAJOR,
        MINOR,
        h.op,
        0,  # flags
        HEADER_BYTES,
        payload_bytes,
        h.count,
        h.session,
        h.ordinal,
        h.trigger,
        h.attempted,
        h.lost,
        h.capacity,
        h.reason,
        h.accepted,
    )
    events_raw = bytearray()
    for ev in events:
        events_raw.extend(
            struct.pack(
                EVENT_FMT,
                ev.sequence,
                ev.session,
                ev.parent,
                ev.qpc,
                ev.owner,
                ev.frame,
                ev.map_epoch,
                ev.device_epoch,
                ev.thread,
                ev.kind,
                ev.label,
                *ev.data,
                *ev.bits,
            )
        )
    return header_raw + bytes(events_raw)


def unpack_event(data: bytes) -> Event:
    if len(data) != EVENT_BYTES:
        raise WireError("Size", f"event size {len(data)} != {EVENT_BYTES}")
    fields = struct.unpack(EVENT_FMT, data)
    ev = Event(
        sequence=fields[0],
        session=fields[1],
        parent=fields[2],
        qpc=fields[3],
        owner=fields[4],
        frame=fields[5],
        map_epoch=fields[6],
        device_epoch=fields[7],
        thread=fields[8],
        kind=fields[9],
        label=fields[10],
        data=list(fields[11:23]),
        bits=list(fields[23:71]),
    )
    validate_event(ev)
    return ev


def unpack_packet(data: bytes) -> Tuple[Header, List[Event]]:
    if len(data) < HEADER_BYTES:
        raise WireError("Size", f"data length {len(data)} < {HEADER_BYTES}")

    fields = struct.unpack(HEADER_FMT, data[:HEADER_BYTES])
    magic, major, minor, op, flags, header_bytes, payload_bytes, count = fields[:8]
    session, ordinal, trigger, attempted, lost, capacity, reason, accepted = fields[8:]

    if magic != MAGIC:
        raise WireError("Magic", f"bad magic {magic}")
    if major != MAJOR or minor != MINOR:
        raise WireError("Version", f"bad version {major}.{minor}")
    if header_bytes != HEADER_BYTES:
        raise WireError("Header", f"bad header bytes {header_bytes}")
    if flags != 0:
        raise WireError("Flags", f"bad flags {flags}")
    if op not in (OP_BEGIN, OP_DATA, OP_SEAL):
        raise WireError("Opcode", f"bad op {op}")

    if count > MAX_EVENTS:
        raise WireError("Shape", f"count {count} > {MAX_EVENTS}")
    if payload_bytes != count * EVENT_BYTES:
        raise WireError("Shape", f"payload_bytes {payload_bytes} != count * {EVENT_BYTES}")

    expected_len = HEADER_BYTES + payload_bytes
    if len(data) != expected_len:
        raise WireError("Size", f"data len {len(data)} != expected {expected_len}")

    h = Header(
        op=op,
        count=count,
        session=session,
        ordinal=ordinal,
        trigger=trigger,
        attempted=attempted,
        lost=lost,
        capacity=capacity,
        reason=reason,
        accepted=accepted,
    )
    validate_header(h)

    events: List[Event] = []
    p = HEADER_BYTES
    for _ in range(count):
        ev = unpack_event(data[p : p + EVENT_BYTES])
        if ev.session != h.session:
            raise WireError("Session", f"event session {ev.session} != header session {h.session}")
        events.append(ev)
        p += EVENT_BYTES

    return h, events


def make_golden() -> Tuple[Header, Event]:
    h = Header(
        op=OP_DATA,
        count=1,
        session=0x0123456789ABCDEF,
        ordinal=42,
        trigger=100,
        attempted=0,
        lost=0,
        capacity=8192,
        reason=0,
        accepted=0,
    )
    label_bytes = b"warvk.golden.event.v1\x00" + b"\x00" * 10
    data_items = [
        0xA001A002A003A004,
        0xB001B002B003B004,
        0xC001C002C003C004,
        0xD001D002D003D004,
        0xE001E002E003E004,
        0xF001F002F003F004,
        0x0123456789ABCDEF,
        0x89ABCDEF01234567,
        0xDEADBEEFCAFEBABE,
        0x00000000FFFFFFFF,
        0xFFFFFFFF00000000,
        0x123456789ABCDEF0,
    ]
    bits_items = [
        0x3F800000,
        0x40000000,
        0xC0000000,
        0x3E800000,
        0xBF800000,
        0x00000000,
        0x80000000,
        0x7F7FFFFF,
        0x00800000,
        0x00400000,
        0x42C80000,
        0xC2C80000,
        0x3F000000,
        0x3EAAAAAB,
        0x40490FDB,
        0x402DF854,
        0xDEADBEEF,
        0xCAFEBABE,
        0xBAADF00D,
        0xFEEDFACE,
        0x01020304,
        0xFF00FF00,
        0x00FF00FF,
        0x55555555,
        0xAAAAAAAA,
        0x12345678,
        0x9ABCDEF0,
        0x00000001,
        0x80000000,
        0xFFFFFFFF,
        0x7FFFFFFF,
        0x0000FFFF,
        0x10000020,
        0x10000021,
        0x10000022,
        0x10000023,
        0x10000024,
        0x10000025,
        0x10000026,
        0x10000027,
        0x10000028,
        0x10000029,
        0x1000002A,
        0x1000002B,
        0x1000002C,
        0x1000002D,
        0x1000002E,
        0x1000002F,
    ]
    ev = Event(
        sequence=123,
        session=0x0123456789ABCDEF,
        parent=0xFEDCBA9876543210,
        qpc=0x0001020304050607,
        owner=0x1122334455667788,
        frame=500,
        map_epoch=3,
        device_epoch=2,
        thread=0x00001357,
        kind=18,
        label=label_bytes,
        data=data_items,
        bits=bits_items,
    )
    return h, ev


class TestGoldenCodec(unittest.TestCase):
    def test_golden_hex_generation_and_sha256(self):
        h, ev = make_golden()
        packet_bytes = pack_packet(h, [ev])
        self.assertEqual(len(packet_bytes), 472)
        hex_str = packet_bytes.hex()
        self.assertEqual(hex_str, GOLDEN_HEX)
        self.assertEqual(hashlib.sha256(packet_bytes).hexdigest(), GOLDEN_BYTES_SHA256)

    def test_golden_packet_roundtrip(self):
        raw = binascii.unhexlify(GOLDEN_HEX)
        h, events = unpack_packet(raw)
        self.assertEqual(h.op, OP_DATA)
        self.assertEqual(h.count, 1)
        self.assertEqual(h.session, 0x0123456789ABCDEF)
        self.assertEqual(h.ordinal, 42)
        self.assertEqual(h.trigger, 100)
        self.assertEqual(h.attempted, 0)
        self.assertEqual(h.lost, 0)
        self.assertEqual(h.capacity, 8192)
        self.assertEqual(h.reason, 0)
        self.assertEqual(h.accepted, 0)

        self.assertEqual(len(events), 1)
        ev = events[0]
        self.assertEqual(ev.sequence, 123)
        self.assertEqual(ev.session, 0x0123456789ABCDEF)
        self.assertEqual(ev.parent, 0xFEDCBA9876543210)
        self.assertEqual(ev.qpc, 0x0001020304050607)
        self.assertEqual(ev.owner, 0x1122334455667788)
        self.assertEqual(ev.frame, 500)
        self.assertEqual(ev.map_epoch, 3)
        self.assertEqual(ev.device_epoch, 2)
        self.assertEqual(ev.thread, 0x1357)
        self.assertEqual(ev.kind, 18)
        self.assertEqual(ev.label[:21], b"warvk.golden.event.v1")
        self.assertEqual(ev.data[0], 0xA001A002A003A004)
        self.assertEqual(ev.data[11], 0x123456789ABCDEF0)

        # Check float conversions
        f_val0 = struct.unpack("<f", struct.pack("<I", ev.bits[0]))[0]
        self.assertAlmostEqual(f_val0, 1.0)
        f_val1 = struct.unpack("<f", struct.pack("<I", ev.bits[1]))[0]
        self.assertAlmostEqual(f_val1, 2.0)
        f_val2 = struct.unpack("<f", struct.pack("<I", ev.bits[2]))[0]
        self.assertAlmostEqual(f_val2, -2.0)
        f_val10 = struct.unpack("<f", struct.pack("<I", ev.bits[10]))[0]
        self.assertAlmostEqual(f_val10, 100.0)
        f_val11 = struct.unpack("<f", struct.pack("<I", ev.bits[11]))[0]
        self.assertAlmostEqual(f_val11, -100.0)
        f_val12 = struct.unpack("<f", struct.pack("<I", ev.bits[12]))[0]
        self.assertAlmostEqual(f_val12, 0.5)

    def test_begin_packet_roundtrip(self):
        h = Header(
            op=OP_BEGIN,
            count=0,
            session=0x55AA55AA,
            ordinal=1,
            trigger=NO_TRIGGER,
            capacity=16384,
        )
        packet = pack_packet(h, [])
        self.assertEqual(len(packet), 80)
        h_out, ev_out = unpack_packet(packet)
        self.assertEqual(h_out.op, OP_BEGIN)
        self.assertEqual(h_out.count, 0)
        self.assertEqual(h_out.session, 0x55AA55AA)
        self.assertEqual(h_out.ordinal, 1)
        self.assertEqual(h_out.trigger, NO_TRIGGER)
        self.assertEqual(h_out.capacity, 16384)
        self.assertEqual(len(ev_out), 0)

    def test_seal_packet_roundtrip_reasons(self):
        for reason in (1, 2, 3, 4):
            h = Header(
                op=OP_SEAL,
                count=0,
                session=0x77778888,
                ordinal=15,
                trigger=50,
                attempted=100,
                lost=10,
                capacity=8192,
                reason=reason,
                accepted=90,
            )
            packet = pack_packet(h, [])
            self.assertEqual(len(packet), 80)
            h_out, ev_out = unpack_packet(packet)
            self.assertEqual(h_out.op, OP_SEAL)
            self.assertEqual(h_out.reason, reason)
            self.assertEqual(h_out.accepted, 90)
            self.assertEqual(h_out.lost, 10)
            self.assertEqual(len(ev_out), 0)

    def test_data_160_events_roundtrip(self):
        h = Header(
            op=OP_DATA,
            count=MAX_EVENTS,
            session=0x9999,
            ordinal=3,
            trigger=NO_TRIGGER,
            capacity=MAX_HISTORY_EVENTS,
        )
        events = []
        for i in range(MAX_EVENTS):
            ev = Event(
                sequence=i + 1,
                session=0x9999,
                parent=i,
                qpc=1000 + i,
                owner=i,
                frame=i,
                map_epoch=1,
                device_epoch=1,
                thread=i,
                kind=1 + (i % 18),
                label=f"ev_{i}\x00".encode("ascii").ljust(32, b"\x00"),
                data=[i * 10 + j for j in range(12)],
                bits=[(i * 48 + j) ^ 0xA5A5A5A5 for j in range(48)],
            )
            events.append(ev)

        packet = pack_packet(h, events)
        self.assertEqual(len(packet), 80 + 160 * 392)
        h_out, ev_out = unpack_packet(packet)
        self.assertEqual(h_out.count, 160)
        self.assertEqual(len(ev_out), 160)
        for i in range(160):
            self.assertEqual(ev_out[i].sequence, i + 1)
            self.assertEqual(ev_out[i].session, 0x9999)
            self.assertEqual(ev_out[i].kind, 1 + (i % 18))


class TestSyntheticNegatives(unittest.TestCase):
    def test_bad_magic(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        raw[0:4] = b"XXXX"
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Magic")

    def test_bad_version(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        struct.pack_into("<HH", raw, 4, 2, 0)
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Version")

    def test_bad_header_bytes(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        struct.pack_into("<I", raw, 12, 84)
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Header")

    def test_bad_flags(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        struct.pack_into("<H", raw, 10, 1)
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Flags")

    def test_bad_opcode(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        struct.pack_into("<H", raw, 8, 4)
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Opcode")

    def test_size_truncated(self):
        raw = binascii.unhexlify(GOLDEN_HEX)
        with self.assertRaises(WireError) as ctx:
            unpack_packet(raw[:471])
        self.assertEqual(ctx.exception.code, "Size")

    def test_size_trailing(self):
        raw = binascii.unhexlify(GOLDEN_HEX) + b"\x00"
        with self.assertRaises(WireError) as ctx:
            unpack_packet(raw)
        self.assertEqual(ctx.exception.code, "Size")

    def test_shape_count_over_max(self):
        h = Header(op=OP_DATA, count=161, session=1, ordinal=1, capacity=8192)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [Event()] * 161)
        self.assertEqual(ctx.exception.code, "Shape")

    def test_shape_payload_bytes_mismatch(self):
        raw = bytearray(binascii.unhexlify(GOLDEN_HEX))
        struct.pack_into("<I", raw, 16, 100)  # payloadBytes = 100 != 392
        with self.assertRaises(WireError) as ctx:
            unpack_packet(bytes(raw))
        self.assertEqual(ctx.exception.code, "Shape")

    def test_capacity_out_of_bounds(self):
        h = Header(op=OP_BEGIN, count=0, session=1, ordinal=1, capacity=3)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Capacity")

        h.capacity = MAX_HISTORY_EVENTS + 1
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Capacity")

    def test_session_zero(self):
        h = Header(op=OP_BEGIN, count=0, session=0, ordinal=1, capacity=8192)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Session")

    def test_event_session_mismatch(self):
        h = Header(op=OP_DATA, count=1, session=0x1111, ordinal=1, capacity=8192)
        ev = Event(sequence=1, session=0x2222, kind=1, label=b"ok\x00" + b"\x00" * 29)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [ev])
        self.assertEqual(ctx.exception.code, "Session")

    def test_begin_ordinal_not_one(self):
        h = Header(op=OP_BEGIN, count=0, session=1, ordinal=2, capacity=8192)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Ordinal")

    def test_begin_trigger_not_no_trigger(self):
        h = Header(op=OP_BEGIN, count=0, session=1, ordinal=1, trigger=10, capacity=8192)
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Trigger")

    def test_seal_trigger_greater_accepted(self):
        h = Header(
            op=OP_SEAL,
            count=0,
            session=1,
            ordinal=2,
            capacity=8192,
            reason=1,
            attempted=50,
            accepted=50,
            lost=0,
            trigger=51,
        )
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Trigger")

    def test_totals_seal_mismatch(self):
        h = Header(
            op=OP_SEAL,
            count=0,
            session=1,
            ordinal=2,
            capacity=8192,
            reason=1,
            attempted=100,
            accepted=80,
            lost=15,  # 100 - 80 != 15
        )
        with self.assertRaises(WireError) as ctx:
            pack_packet(h, [])
        self.assertEqual(ctx.exception.code, "Totals")

    def test_event_sequence_invalid(self):
        ev = Event(sequence=0, session=1, kind=1, label=b"x\x00" + b"\x00" * 30)
        with self.assertRaises(WireError) as ctx:
            validate_event(ev, 1)
        self.assertEqual(ctx.exception.code, "EventValue")

        ev.sequence = 0xFFFFFFFFFFFFFFFF
        with self.assertRaises(WireError) as ctx:
            validate_event(ev, 1)
        self.assertEqual(ctx.exception.code, "EventValue")

    def test_event_kind_out_of_range(self):
        ev = Event(sequence=1, session=1, kind=0, label=b"x\x00" + b"\x00" * 30)
        with self.assertRaises(WireError) as ctx:
            validate_event(ev, 1)
        self.assertEqual(ctx.exception.code, "EventValue")

        ev.kind = 19
        with self.assertRaises(WireError) as ctx:
            validate_event(ev, 1)
        self.assertEqual(ctx.exception.code, "EventValue")

    def test_event_label_missing_nul(self):
        ev = Event(sequence=1, session=1, kind=1, label=b"A" * 32)
        with self.assertRaises(WireError) as ctx:
            validate_event(ev, 1)
        self.assertEqual(ctx.exception.code, "EventValue")


def main(argv=None):
    parser = argparse.ArgumentParser(description="WVE1 wire golden test")
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Optional fresh directory to write artifacts (must not exist)",
    )
    args = parser.parse_args(argv)

    if args.output is not None:
        if args.output.exists():
            print(f"refusing: output directory already exists: {args.output}", file=sys.stderr)
            sys.exit(1)

    suite = unittest.TestLoader().loadTestsFromTestCase(TestGoldenCodec)
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestSyntheticNegatives))
    runner = unittest.TextTestRunner(verbosity=2)
    res = runner.run(suite)

    if not res.wasSuccessful():
        print(f"FAILED: {len(res.failures)} failures, {len(res.errors)} errors", file=sys.stderr)
        sys.exit(1)

    golden_bin = binascii.unhexlify(GOLDEN_HEX)
    golden_sha = hashlib.sha256(golden_bin).hexdigest()

    if args.output is not None:
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "golden.hex").write_text(f"GOLDEN={GOLDEN_HEX}\n", encoding="utf-8")
        (args.output / "golden.bin").write_bytes(golden_bin)

        summary = {
            "suite": "test_recorder_event_wire_golden",
            "tests_run": res.testsRun,
            "failures": len(res.failures),
            "errors": len(res.errors),
            "golden_packet_bytes": len(golden_bin),
            "golden_sha256": golden_sha,
            "scope": "TARGETED_PYTHON_STATIC_AND_SYNTHETIC_ONLY",
        }
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print(f"GOLDEN={GOLDEN_HEX}")
    print(f"checks={res.testsRun} PASS")


if __name__ == "__main__":
    main()