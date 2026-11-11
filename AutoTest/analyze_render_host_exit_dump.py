#!/usr/bin/env python3
"""Read a bounded AMD64 host minidump without attaching to a live process.

The stack-word scan is explicitly NOT an unwind. LLDB's LoadCore unwinder is
reported separately, along with PC/SP/CFA, and its nearest-export labels are
not accepted as private function names. Exact OpenGL names come only from the
hash-pinned, previously audited PDB/PE address map. No target execution,
attachment, symbol download, DLL copying or system configuration is performed.

Use the Python version expected by the installed LLVM Python extension (the
local LLVM 22 package expects Python 3.11). Output is create-new only.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import uuid


def identity(path: Path) -> dict:
    raw = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest().upper()}


def need(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def take(raw: bytes, offset: int, size: int) -> bytes:
    need(offset >= 0 and size >= 0 and offset + size <= len(raw),
         "truncated or out-of-range file span")
    return raw[offset:offset + size]


def unpack(fmt: str, raw: bytes, offset: int):
    return struct.unpack(fmt, take(raw, offset, struct.calcsize(fmt)))


def cv_record(raw: bytes) -> dict:
    need(len(raw) >= 24 and raw[:4] == b"RSDS", "expected bounded RSDS CodeView")
    return {"guid": str(uuid.UUID(bytes_le=raw[4:20])).upper(),
            "age": unpack("<I", raw, 20)[0]}


def read_dump(path: Path) -> dict:
    raw = path.read_bytes()
    signature, version, count, directory, checksum, timestamp, flags = unpack("<6IQ", raw, 0)
    need(signature == 0x504D444D and count <= 128, "invalid minidump header")
    streams = {}
    for i in range(count):
        kind, size, rva = unpack("<III", raw, directory + 12 * i)
        if kind == 0:  # UnusedStream directory padding is allowed to repeat.
            need(size == 0, "unused minidump stream is not empty")
            continue
        need(kind not in streams, "duplicate minidump stream")
        streams[kind] = take(raw, rva, size)
    need({3, 4, 7, 15, 17}.issubset(streams), "required identity/context streams missing")
    need(unpack("<H", streams[7], 0)[0] == 9, "only AMD64 minidumps are supported")
    misc = streams[15]
    need(unpack("<I", misc, 4)[0] & 1, "dump process-id flag missing")
    pid = unpack("<I", misc, 8)[0]
    modules = []
    module_count = unpack("<I", streams[4], 0)[0]
    need(module_count <= 4096, "unbounded module count")
    for i in range(module_count):
        row = take(streams[4], 4 + i * 108, 108)
        base, size, module_checksum, stamp, name_rva = unpack("<QIIII", row, 0)
        name_size = unpack("<I", raw, name_rva)[0]
        need(name_size <= 32768 and name_size % 2 == 0, "invalid module path length")
        name = take(raw, name_rva + 4, name_size).decode("utf-16-le")
        cv_size, cv_rva = unpack("<II", row, 76)
        modules.append({"path": name, "base": base, "size": size,
                        "checksum": module_checksum, "timestamp": stamp,
                        "cv": cv_record(take(raw, cv_rva, cv_size)) if cv_size else None})
    thread_count = unpack("<I", streams[3], 0)[0]
    need(thread_count == 1, "expected exactly one selected main thread")
    row = take(streams[3], 4, 48)
    tid = unpack("<I", row, 0)[0]
    stack_start, stack_size, stack_rva, context_size, context_rva = unpack("<QIIII", row, 24)
    context = take(raw, context_rva, context_size)
    need(len(context) >= 256, "AMD64 context truncated")
    rsp, rip = unpack("<Q", context, 152)[0], unpack("<Q", context, 248)[0]
    need(stack_start <= rsp < stack_start + stack_size, "RSP is outside captured stack")
    info_header, info_entry, info_count = unpack("<III", streams[17], 0)
    need(info_header >= 12 and info_entry >= 64 and info_count <= 4096, "invalid thread-info header")
    info = []
    for i in range(info_count):
        entry = take(streams[17], info_header + info_entry * i, info_entry)
        if unpack("<I", entry, 0)[0] == tid:
            info.append({"creation100ns": unpack("<Q", entry, 16)[0],
                         "exit100ns": unpack("<Q", entry, 24)[0],
                         "startAddress": unpack("<Q", entry, 48)[0]})
    need(len(info) == 1, "selected thread identity is missing or ambiguous")
    return {"pid": pid, "tid": tid, "threadInfo": info[0], "rip": rip,
            "rsp": rsp, "stackStart": stack_start, "stackBytes": stack_size,
            "contextBytes": context_size, "flags": flags, "streamCount": count,
            "modules": modules, "stack": take(raw, stack_rva, stack_size)}


def read_pe(path: Path) -> dict:
    raw = path.read_bytes()
    pe = unpack("<I", raw, 60)[0]
    need(take(raw, pe, 4) == b"PE\0\0", "invalid PE signature")
    machine, sections, stamp = unpack("<HHI", raw, pe + 4)
    optional_size = unpack("<H", raw, pe + 20)[0]
    optional = pe + 24
    need(machine == 0x8664 and unpack("<H", raw, optional)[0] == 0x20B,
         "expected AMD64 PE32+ image")
    section_rows = []
    for i in range(sections):
        row = take(raw, optional + optional_size + 40 * i, 40)
        virtual_size, rva, raw_size, raw_offset = unpack("<IIII", row, 8)
        section_rows.append((rva, virtual_size, raw_offset, raw_size))

    def rva_span(rva: int, size: int) -> bytes:
        for start, virtual_size, offset, raw_size in section_rows:
            if start <= rva and rva + size <= start + raw_size:
                return take(raw, offset + rva - start, size)
        raise ValueError("PE RVA is not wholly file-backed")

    debug_rva, debug_size = unpack("<II", raw, optional + 112 + 6 * 8)
    need(debug_size % 28 == 0, "invalid PE debug directory")
    cvs = []
    for offset in range(0, debug_size, 28):
        row = rva_span(debug_rva + offset, 28)
        if unpack("<I", row, 12)[0] == 2:
            size, data_rva, data_offset = unpack("<III", row, 16)
            cvs.append(cv_record(take(raw, data_offset, size)))
    need(len(cvs) == 1, "expected exactly one PE RSDS record")
    return {"timestamp": stamp, "size": unpack("<I", raw, optional + 56)[0],
            "checksum": unpack("<I", raw, optional + 64)[0], "cv": cvs[0]}


def module_for(dump: dict, address: int):
    return next((m for m in dump["modules"]
                 if m["base"] <= address < m["base"] + m["size"]), None)


def describe_address(dump: dict, address: int, audit: dict) -> dict:
    result = {"address": f"0x{address:016X}"}
    module = module_for(dump, address)
    if module is None:
        return result
    rva = address - module["base"]
    result.update(module=Path(module["path"]).name, rva=f"0x{rva:X}")
    if Path(module["path"]).name.lower() == "opengl32.dll":
        for item in audit["addressMap"]:
            start, end = int(item["functionStartRva"], 16), int(item["functionEndExclusiveRva"], 16)
            if start <= rva < end:
                result["auditedFunction"] = item["function"]
                result["functionOffset"] = f"0x{rva - start:X}"
                result["exactAuditedReturnAddress"] = rva == int(item["returnAddressRva"], 16)
                break
    return result


def lldb_unwind(path: Path, llvm: Path, pdb: Path, dump: dict, audit: dict) -> dict:
    # This LLVM distribution names the extension _lldb.abi3None. Load that
    # existing binary explicitly; do not rename/copy it or modify an install.
    dll_scope = os.add_dll_directory(str(llvm / "bin"))
    extension = llvm / "lib/site-packages/lldb/native/_lldb.abi3None"
    loader = importlib.machinery.ExtensionFileLoader("_lldb", str(extension))
    spec = importlib.util.spec_from_loader("_lldb", loader)
    need(spec is not None, "LLVM Python extension has no module spec")
    native = importlib.util.module_from_spec(spec)
    sys.modules["_lldb"] = native
    loader.exec_module(native)
    sys.path.insert(0, str(llvm / "lib/site-packages"))
    import lldb
    debugger = lldb.SBDebugger.Create(False)
    try:
        error = lldb.SBDebugger.SetInternalVariable(
            "symbols.enable-external-lookup", "false", debugger.GetInstanceName())
        need(error.Success(), "cannot disable external symbol lookup")
        target = debugger.CreateTarget("")
        error = lldb.SBError()
        process = target.LoadCore(str(path.resolve()), error)
        need(error.Success() and process.IsValid(), "LLDB failed to load offline core")
        # Commands cannot launch/attach: only a fixed symbol-add command with
        # a validated local path is admitted; no arbitrary command argument.
        need('"' not in str(pdb) and "\n" not in str(pdb), "invalid symbol path")
        result = lldb.SBCommandReturnObject()
        debugger.GetCommandInterpreter().HandleCommand(
            'target symbols add "' + str(pdb.resolve()).replace("\\", "/") + '"', result)
        need(result.Succeeded(), "LLDB rejected pinned PDB")
        need(process.GetProcessID() == dump["pid"], "LLDB/dump process-id disagreement")
        need(process.GetNumThreads() == 1, "LLDB thread count disagreement")
        thread = process.GetThreadAtIndex(0)
        need(thread.GetThreadID() == dump["tid"], "LLDB/dump thread-id disagreement")
        count = thread.GetNumFrames()
        need(0 < count <= 64, "unbounded or empty LLDB unwind")
        frames = []
        for i in range(count):
            frame = thread.GetFrameAtIndex(i)
            row = describe_address(dump, frame.GetPC(), audit)
            row.update(index=i, stackPointer=f"0x{frame.GetSP():016X}",
                       canonicalFrameAddress=f"0x{frame.GetCFA():016X}",
                       lldbLabelNotPrivateSymbolProof=frame.GetFunctionName())
            frames.append(row)
        need(thread.GetFrameAtIndex(0).GetPC() == dump["rip"] and
             thread.GetFrameAtIndex(0).GetSP() == dump["rsp"], "LLDB top context disagreement")
        return {"method": "LLDB SBTarget.LoadCore offline unwind; not raw stack scan",
                "version": lldb.SBDebugger.GetVersionString(),
                "externalSymbolLookupEnabled": False, "targetAttachedOrRun": False,
                "frames": frames, "symbolAddMessage": result.GetOutput()}
    finally:
        lldb.SBDebugger.Destroy(debugger)
        dll_scope.close()


def analyze(args) -> dict:
    paths = {name: Path(getattr(args, name)).resolve()
             for name in ("dump", "helper_result", "symbol_audit", "pdb", "pe")}
    before = {name: identity(path) for name, path in paths.items()}
    helper = json.loads(paths["helper_result"].read_text(encoding="utf-8-sig"))
    audit = json.loads(paths["symbol_audit"].read_text(encoding="utf-8-sig"))
    need(helper["ok"] is True, "capture helper did not establish success")
    need(before["dump"]["sha256"] == helper["dump"]["sha256"].upper(), "dump hash disagrees with helper")
    need(before["dump"]["bytes"] == helper["dump"]["bytes"], "dump size disagrees with helper")
    for name in ("pdb", "pe"):
        need(before[name]["sha256"] == audit["artifacts"][name]["sha256"].upper(),
             "symbol audit's pinned file changed: " + name)
        need(before[name]["bytes"] == audit["artifacts"][name]["bytes"], "pinned symbol file size changed")
    dump = read_dump(paths["dump"])
    capture_identity = helper["identityImmediatelyBeforeWrite"]
    need(dump["pid"] == capture_identity["host"]["pid"], "captured PID mismatch")
    need(dump["tid"] == capture_identity["mainThreadId"], "captured main TID mismatch")
    opengl = [m for m in dump["modules"] if Path(m["path"]).name.lower() == "opengl32.dll"]
    need(len(opengl) == 1, "missing or ambiguous OpenGL module")
    opengl = opengl[0]
    pe = read_pe(paths["pe"])
    for name in ("timestamp", "size", "checksum", "cv"):
        need(opengl[name] == pe[name], "dump/PE OpenGL identity mismatch: " + name)
    binding = audit["pdbBinding"]
    need(pe["cv"] == {"guid": binding["guid"], "age": binding["peRsdsAge"]},
         "current OpenGL RSDS differs from frozen symbol audit")
    scan = []
    for offset in range(0, len(dump["stack"]) - 7, 8):
        address = unpack("<Q", dump["stack"], offset)[0]
        if opengl["base"] <= address < opengl["base"] + opengl["size"]:
            row = describe_address(dump, address, audit)
            row["stackWordAddress"] = f"0x{dump['stackStart'] + offset:016X}"
            scan.append(row)
    unwind = lldb_unwind(paths["dump"], Path(args.llvm_root).resolve(), paths["pdb"], dump, audit)
    observed = [f["rva"] for f in unwind["frames"] if f.get("module", "").lower() == "opengl32.dll"]
    expected = [entry["returnAddressRva"].upper().replace("0X", "0x") for entry in audit["addressMap"]]
    after = {name: identity(path) for name, path in paths.items()}
    need(before == after, "input evidence changed during offline analysis")
    return {"schema": "warvk-offline-exit-dump-analysis/v1", "scope": "OFFLINE_ONLY",
            "acceptancePassed": False, "originalFailureOverridden": False,
            "targetAttachedOrRun": False, "systemModified": False,
            "inputs": before, "inputFilesPreserved": True,
            "identity": {"pid": dump["pid"], "mainThreadId": dump["tid"],
                         "hostCreation100nsFromCaptureReceipt": capture_identity["host"]["creation100ns"],
                         "mainThreadInfoFromDump": dump["threadInfo"],
                         "rip": f"0x{dump['rip']:016X}", "rsp": f"0x{dump['rsp']:016X}",
                         "stackBytes": dump["stackBytes"], "contextBytes": dump["contextBytes"],
                         "headerFlags": dump["flags"], "moduleCount": len(dump["modules"])},
            "openglDumpModule": opengl, "pinnedPePdbAuditBindingPassed": True,
            "rawStackWords": {"isUnwind": False, "scope": "OpenGL-address-like words only",
                              "candidates": scan}, "unwind": unwind,
            "comparison": {"exactPriorOpenGlUnwoundRvas": expected,
                           "currentOpenGlUnwoundRvas": observed,
                           "sameOpenGlUnwoundCallChain": observed == expected},
            "limitations": ["LLDB unwind is not an independent second unwinder implementation.",
                "Only the four OpenGL functions covered by the pinned symbol audit receive exact private names.",
                "Nearest-export LLDB labels for other un-symbolized frames are not precise internal function proof.",
                "Raw stack candidates may be data or stale return addresses; they do not certify frames.",
                "This does not identify who created the palette watcher or certify a fix.",
                "The bounded dump does not establish the current watcher-global value."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("dump", "helper-result", "symbol-audit", "pdb", "pe", "llvm-root", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    need(not output.exists() and output.parent.is_dir(), "output must be a new file in an existing directory")
    report = analyze(args)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    print(json.dumps({"report": str(output), "identity": report["identity"],
                      "comparison": report["comparison"],
                      "frames": report["unwind"]["frames"]}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
