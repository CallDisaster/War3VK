#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract an actionable x86 exception stack from a WarVK minidump.

This deliberately does not require a GUI debugger.  WarVK installs its
vectored/unhandled exception handler before the rendering pipeline is created,
and writes a dump that contains the exception context, stacks, data segments
and indirectly referenced memory.  This reader turns that retained evidence
into stable JSON suitable for comparing repeated map-load failures.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import logging
import struct
import sys
from pathlib import Path
from typing import Any

from minidump.minidumpfile import MinidumpFile
from minidump.streams.ContextStream import WOW64_CONTEXT


REGISTER_NAMES = (
    "Eax", "Ebx", "Ecx", "Edx", "Esi", "Edi", "Ebp", "Esp", "Eip",
)

STORM_EXPORTS = (
    ("SMemAlloc", 0x2B830),
    ("SMemFree", 0x2BE40),
    ("SMemGetSize", 0x2C000),
    ("SMemReAlloc", 0x2C8B0),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def hex32(value: int) -> str:
    return f"0x{int(value) & 0xFFFFFFFF:08X}"


def read_bytes(reader: Any, address: int, size: int) -> bytes | None:
    if address <= 0 or size <= 0:
        return None
    try:
        data = bytes(reader.read(int(address), int(size)))
    except Exception:
        return None
    return data if len(data) == size else None


def read_u32(reader: Any, address: int) -> int | None:
    data = read_bytes(reader, address, 4)
    return struct.unpack("<I", data)[0] if data is not None else None


def parse_exception_context(dump_path: Path, location: Any) -> Any:
    with dump_path.open("rb") as stream:
        stream.seek(int(location.Rva))
        raw = stream.read(int(location.DataSize))
    if len(raw) < 4:
        raise ValueError("exception context is truncated")
    # Warcraft III is a 32-bit process.  A 64-bit Python host still receives
    # the WOW64/x86 CONTEXT layout in this dump.
    return WOW64_CONTEXT.parse(io.BytesIO(raw))


def module_rows(minidump: Any) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    modules = getattr(getattr(minidump, "modules", None), "modules", []) or []
    for module in modules:
        rows.append({
            "name": str(module.name),
            "base": int(module.baseaddress),
            "end": int(module.endaddress),
            "size": int(module.size),
            "timestamp": int(module.timestamp),
        })
    rows.sort(key=lambda row: row["base"])
    return rows


def symbolize(address: int, modules: list[dict[str, Any]]) -> dict[str, Any]:
    for module in modules:
        if module["base"] <= address < module["end"]:
            return {
                "address": address,
                "addressHex": hex32(address),
                "module": Path(module["name"]).name,
                "modulePath": module["name"],
                "moduleBase": module["base"],
                "moduleBaseHex": hex32(module["base"]),
                "moduleOffset": address - module["base"],
                "moduleOffsetHex": hex32(address - module["base"]),
                "symbol": (
                    f"{Path(module['name']).name}"
                    f"+0x{address - module['base']:X}"
                ),
            }
    return {
        "address": address,
        "addressHex": hex32(address),
        "module": None,
        "moduleOffset": None,
        "symbol": hex32(address),
    }


def walk_ebp_chain(
    reader: Any,
    start_ebp: int,
    modules: list[dict[str, Any]],
    limit: int = 64,
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    seen: set[int] = set()
    ebp = int(start_ebp)
    for index in range(limit):
        if ebp == 0 or ebp in seen:
            break
        seen.add(ebp)
        next_ebp = read_u32(reader, ebp)
        return_address = read_u32(reader, ebp + 4)
        if next_ebp is None or return_address is None:
            break
        row = {
            "index": index,
            "framePointer": ebp,
            "framePointerHex": hex32(ebp),
            "nextFramePointer": next_ebp,
            "nextFramePointerHex": hex32(next_ebp),
            "return": symbolize(return_address, modules),
        }
        result.append(row)
        # x86 EBP chains grow toward higher addresses.  Reject wild/corrupt
        # links rather than turning arbitrary heap bytes into a fake stack.
        if next_ebp <= ebp or next_ebp - ebp > 4 * 1024 * 1024:
            row["terminated"] = "non-monotonic-or-implausible-frame"
            break
        ebp = next_ebp
    return result


def dword_window(reader: Any, address: int, before: int, after: int) -> dict[str, Any]:
    start = max(0, int(address) - before)
    size = before + after
    data = read_bytes(reader, start, size)
    if data is None:
        return {
            "address": int(address),
            "addressHex": hex32(address),
            "readable": False,
        }
    dwords = []
    for offset in range(0, len(data) - 3, 4):
        value = struct.unpack_from("<I", data, offset)[0]
        dwords.append({
            "address": start + offset,
            "addressHex": hex32(start + offset),
            "value": value,
            "valueHex": hex32(value),
        })
    return {
        "address": int(address),
        "addressHex": hex32(address),
        "start": start,
        "startHex": hex32(start),
        "readable": True,
        "dwords": dwords,
    }


def storm_hook_probes(
    reader: Any, modules: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    storm = next(
        (
            module
            for module in modules
            if Path(module["name"]).name.casefold() == "storm.dll"
        ),
        None,
    )
    if storm is None:
        return []

    probes: list[dict[str, Any]] = []
    for name, rva in STORM_EXPORTS:
        address = int(storm["base"]) + rva
        code = read_bytes(reader, address, 16)
        row: dict[str, Any] = {
            "name": name,
            "rva": rva,
            "rvaHex": hex32(rva),
            "address": address,
            "addressHex": hex32(address),
            "bytes": code.hex().upper() if code is not None else None,
            "readable": code is not None,
            "minHookJumpPresent": bool(code and code[0] == 0xE9),
        }
        if code and code[0] == 0xE9:
            relative = struct.unpack_from("<i", code, 1)[0]
            detour = (address + 5 + relative) & 0xFFFFFFFF
            row["detour"] = symbolize(detour, modules)
        probes.append(row)
    return probes


def analyze(dump_path: Path) -> dict[str, Any]:
    # The third-party parser attempts an optional PEB decode and logs an error
    # when MiniDumpScanMemory did not retain that exact page.  It does not
    # affect exception/stack parsing, so keep command output deterministic.
    logging.disable(logging.CRITICAL)
    minidump = MinidumpFile.parse(str(dump_path))
    exception_rows = (
        getattr(getattr(minidump, "exception", None), "exception_records", [])
        or []
    )
    if not exception_rows:
        raise ValueError("dump has no exception stream")
    exception = exception_rows[0]
    record = exception.ExceptionRecord
    context = parse_exception_context(dump_path, exception.ThreadContext)
    reader = minidump.get_reader()
    modules = module_rows(minidump)

    registers = {
        name.lower(): {
            "value": int(getattr(context, name)),
            "hex": hex32(getattr(context, name)),
        }
        for name in REGISTER_NAMES
    }
    information = [int(value) for value in record.ExceptionInformation]
    access_kind = None
    if int(record.ExceptionCode_raw) == 0xC0000005 and information:
        access_kind = {0: "read", 1: "write", 8: "execute"}.get(
            information[0], f"unknown-{information[0]}"
        )

    instruction = read_bytes(reader, int(context.Eip), 32)
    result = {
        "schema": "warvk-x86-minidump-analysis-v1",
        "dump": str(dump_path.resolve()),
        "dumpSize": dump_path.stat().st_size,
        "dumpSha256": sha256(dump_path),
        "exception": {
            "threadId": int(exception.ThreadId),
            "code": int(record.ExceptionCode_raw),
            "codeHex": hex32(record.ExceptionCode_raw),
            "flags": int(record.ExceptionFlags),
            "recordAddress": int(record.ExceptionAddress),
            "recordAddressHex": hex32(record.ExceptionAddress),
            "contextInstruction": symbolize(int(context.Eip), modules),
            "information": information,
            "informationHex": [hex32(value) for value in information],
            "accessKind": access_kind,
            "accessAddress": (
                information[1]
                if int(record.ExceptionCode_raw) == 0xC0000005
                and len(information) >= 2
                else None
            ),
            "accessAddressHex": (
                hex32(information[1])
                if int(record.ExceptionCode_raw) == 0xC0000005
                and len(information) >= 2
                else None
            ),
        },
        "registers": registers,
        "instructionBytes": instruction.hex().upper() if instruction else None,
        "ebpFrames": walk_ebp_chain(
            reader, int(context.Ebp), modules
        ),
        "stackWindow": dword_window(
            reader, int(context.Esp), 0, 256
        ),
        "registerMemory": {
            name.lower(): dword_window(
                reader, int(getattr(context, name)), 0, 96
            )
            for name in ("Eax", "Ebx", "Ecx", "Edx", "Esi", "Edi")
        },
        "stormHookTargets": storm_hook_probes(reader, modules),
        "modules": modules,
    }
    return result


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="解析 WarVK 写出的 32 位 Warcraft III 原生崩溃转储。"
    )
    parser.add_argument("dump", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    dump_path = args.dump.resolve()
    if not dump_path.is_file():
        raise SystemExit(f"dump not found: {dump_path}")
    payload = analyze(dump_path)
    text = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    if args.output is not None:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
