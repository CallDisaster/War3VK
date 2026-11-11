"""Exit diagnostics for explicitly whitelisted preserved presenter binaries.

This is NOT an acceptance gate. A debugger is deliberately not attached: a
timed-out LLDB command cannot prove that its target was safely detached. Only
query handles, Toolhelp snapshots and asynchronous Wait Chain Traversal (WCT)
are used by the monitor. No running-target suspension by that monitor, debug
privilege adjustment, registry or driver changes. The separately described
opt-in dump helper may briefly suspend the target. --restricted-medium creates
only the new child suspended to verify its derived token before first execution.
The existing isolated Desktop/Job launcher remains the sole launch/watchdog
authority; the selected case's original launcher deadline (r8:45s, r9:90s) and
the native 5-second exit deadline stay unchanged. Its watchdog/Job termination
is a FAILURE, never diagnostic success. The default is r8-clear; r9-readback
reuses the frozen eight-frame fixture, not current sources or build outputs.

Thread/process Toolhelp enumeration is system-wide by API design; only the
exact owned host's rows are retained. WCT flags=0 does not follow other
processes, and out-of-scope node identities/names are not retained. WCT waits
are asynchronous, with a 200ms collection window and documented cancellation;
Windows API return latency is not a hard real-time guarantee. An absent or
single-node chain does NOT prove that a thread is not stalled.

Optional --implicit-layers-disabled changes only the child environment. It is
an experimental comparison, not a repair or evidence that every loader honors
the filter. No loader/registry/system setting is modified.

Optional --observe-palette-watcher adds only bounded VM_READ of one hash/PDB-
bound OpenGL TID global and query-only thread identity/start observations for
the already-owned host. At most four pre-exit plus three exit samples, two 4B
reads each; no window enumeration/message pumping. It is non-atomic diagnostic
evidence, not an exit fix. Disabled means no additional process VM_READ handle.

Opt-in --capture-minidump starts an external 64-bit Python helper at about
1000ms after exit-intent. It uses MiniDumpNormal | MiniDumpWithThreadInfo for
the witnessed main thread only, with no full memory/handle collection. DbgHelp
may internally suspend that thread while reading context: this is NOT a
zero-interference observation. Cancellation is requested after 1.5s; after
3s the supervisor terminates ONLY its own helper through its original Popen
HANDLE. If that happens, target resumption is UNPROVEN, all timing is tainted,
and the original fixture/isolated Job remains the only target cleanup owner.
The diagnostic code never explicitly suspends, resumes or kills the target.

References:
https://learn.microsoft.com/en-us/windows/win32/api/wct/nf-wct-getthreadwaitchain
https://learn.microsoft.com/en-us/windows/win32/api/wct/nf-wct-closethreadwaitchainsession
https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDebugging.md
https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/ne-minidumpapiset-minidump_callback_type
"""

from __future__ import annotations

import argparse
import copy
import ctypes as ct
from ctypes import wintypes as wt
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json
from render_lab_layer_source import create_empty_source, empty_directory_witness, check_empty_source_trace


PRESETS = {
    "r8-clear": dict(
        caseDirectory="clear-soak", priorDirectory="original-r8", launcherTimeoutSeconds=45,
        fixtureSchema="warvk-d3d9-presenter-clear-soak/v1",
        arguments=("{parent}", "--dll", "{dll}", "--scenario", "clear-soak", "--diagnostic-readback", "1",
                   "--expect-desktop", "{desktop}", "--input-desktop", "{input}"),
        desktopArgumentPositions={8: "desktop", 10: "input"},
        binaries={
            "parent": ("d3d9-presenter-fault32.exe", 1470976, "11E586882DECAC83C80DBB843A06258BF31EA53B966CFC714CAB3AC8EA2E7635"),
            "dll": ("proxy.dll", 962048, "57301263CCC0DFEBB30F200AB0758D7661A5393E00110469F66C65594C945BD9"),
            "host": ("warvk-d3d9-host-dev.exe", 5484544, "7ED552F1A9E6F7AA998E51E26014286A9B4891EACC37198ECD21B6E291707A3C"),
        }),
    "r9-readback": dict(
        caseDirectory="presenter-readback-eight-frames", priorDirectory="original-r9", launcherTimeoutSeconds=90,
        fixtureSchema="warvk-d3d9-wsi-fixture/v1",
        arguments=("{parent}", "--dll", "{dll}", "--scenario", "normal", "--wsi-readback"),
        desktopArgumentPositions={},
        binaries={
            "parent": ("d3d9-api-parent32.exe", 815104, "F30635FF70F163AE5F1D86B3C7E1272692354530E37ECC82836BFB9AAFB02889"),
            "dll": ("proxy.dll", 962048, "16A6B4B870175B37AD47F8F0DF1FB5CD413E122290517353172D8A62827FABA6"),
            "host": ("warvk-d3d9-host-dev.exe", 5484544, "AE0180484E2D953EA4E0C5EA128FA7150D0826A4432CC6B76DA410DABB7A9E20"),
        }),
}
EXIT_INTENT = re.compile(
    r"^D3D9 host exit-intent pid=([1-9][0-9]*) strategy=normal-return code=0 processElapsedMs=([0-9]+)$")
QUERY_PROCESS = 0x1000 | 0x100000  # QUERY_LIMITED_INFORMATION | SYNCHRONIZE
QUERY_THREAD = 0x0800 | 0x100000
INVALID_HANDLE = ct.c_void_p(-1).value
SNAPSHOT_AT_MS = (300, 1000, 3000)
DBGHELP = Path("C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/dbghelp.dll")
DUMP_MAX_BYTES = 64 * 1024 * 1024
DUMP_MAX_WRITE_BYTES = 8 * 1024 * 1024
DUMP_FLAGS = 0x1000  # Normal(0) | WithThreadInfo, no full/indirect memory.
DUMP_OPTIONAL_AVX_CONTEXT = 0x00200000  # SDK MiniDumpWithAvxXStateContext, NOT FilterTriage.
DUMP_TARGET_ACCESS = 0x0400 | 0x0010 | 0x100000  # QUERY_INFORMATION | VM_READ | SYNCHRONIZE


def same_path(left, right):
    return os.path.normcase(str(Path(left).resolve())) == os.path.normcase(str(Path(right).resolve()))


def positive_int(value):
    return type(value) is int and 0 < value <= 0xffffffff


def payload_equal(left, right):
    return left["bytes"] == right["bytes"] and left["sha256"] == right["sha256"]


def selected_preset(name):
    require(type(name) is str and name in PRESETS, "case is not a frozen diagnostic preset")
    return PRESETS[name]


def preset_argv(preset, directory, desktop, input_desktop):
    values = {role: str(directory / row[0]) for role, row in preset["binaries"].items()}
    values.update(desktop=desktop, input=input_desktop)
    return [argument.format_map(values) for argument in preset["arguments"]]


def preserved_case_launch(original, original_directory, preset):
    """Pure receipt gate: only the saved case, arguments and deadline are allowed."""
    require(original.get("scope") == "D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP", "wrong original receipt scope")
    require(original.get("ok") is False, "original failure must remain visible")
    cases = [row for row in original["cases"] if row.get("label") == preset["caseDirectory"]]
    require(len(cases) == 1, "frozen case receipt missing/duplicate")
    case = cases[0]
    launch = case["process"]
    require(type(launch.get("timeoutSeconds")) in (int, float)
            and launch["timeoutSeconds"] == preset["launcherTimeoutSeconds"], "original launcher deadline differs")
    require(isinstance(launch.get("desktop"), str) and launch["desktop"]
            and isinstance(launch.get("inputBefore"), str) and launch["inputBefore"]
            and launch["desktop"].casefold() != launch["inputBefore"].casefold(), "original desktop scope")
    expected = preset_argv(preset, original_directory / preset["caseDirectory"],
                           launch["desktop"], launch["inputBefore"])
    argv = launch["argv"]
    require(type(argv) is list and len(argv) == len(expected) and all(type(item) is str for item in argv),
            "original argument shape differs")
    require(all(same_path(actual, wanted) if index in (0, 2) else actual == wanted
                for index, (actual, wanted) in enumerate(zip(argv, expected))), "original arguments differ")
    require(len([row for row in case["reports"] if row.get("schema") == preset["fixtureSchema"]]) == 1,
            "original fixture schema missing/duplicate")
    return launch


def remap_preserved_argv(launch, preset, directory, desktop, input_desktop):
    """Preserve all original argv entries except exact binary paths/Desktop values."""
    argv = list(launch["argv"])
    argv[0] = str(directory / preset["binaries"]["parent"][0])
    argv[2] = str(directory / preset["binaries"]["dll"][0])
    values = dict(desktop=desktop, input=input_desktop)
    for index, key in preset["desktopArgumentPositions"].items():
        argv[index] = values[key]
    require(argv == preset_argv(preset, directory, desktop, input_desktop), "remapped argument contract differs")
    return argv


def validate_frozen_binary(preset, role, row):
    name, size, digest = preset["binaries"][role]
    require(Path(row["path"]).name == name and (row["bytes"], row["sha256"]) == (size, digest),
            "not preserved preset binary: " + role)


def validate_dump_preset(request, directory):
    preset = selected_preset(request.get("casePreset"))
    require(directory.name == preset["caseDirectory"]
            and same_path(request["outputDirectory"], directory), "dump request case scope")
    for role in ("host", "parent"):
        require(same_path(request["binding"][role]["image"], directory / preset["binaries"][role][0]),
                "dump target image outside exact preset copies")
    return preset


def write_json(path, value):
    # Generated evidence, never source editing or replacement of old evidence.
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def complete_records(path):
    if not path.is_file():
        return []
    raw = path.read_bytes()
    require(len(raw) <= 8 * 1024 * 1024, "diagnostic log exceeded read bound")
    return [strict_json(line.decode("utf-8")) for line in raw.splitlines(keepends=True)
            if line.endswith(b"\n") and line.startswith(b"{")]


def prove_host(launch, witness, parent, host, expected_parent, expected_host,
               desktop_name, input_desktop, observed_parent_pid):
    """Pure identity gate, also used by the no-native self-test."""
    require(launch.get("jobAtomic") is True and launch.get("handleListApplied") is True,
            "parent not launched in the original atomic Job")
    require(positive_int(launch.get("pid")) and positive_int(witness.get("pid")), "PID absent")
    require(witness.get("schema") == "warvk-d3d9-desktop/v1" and witness.get("beforeGpu") is True,
            "native pre-GPU desktop witness absent")
    require(witness["pid"] == host["pid"] and witness.get("peerPid") == parent["pid"] == launch["pid"],
            "host/peer/launch PID disagreement")
    require(observed_parent_pid == parent["pid"], "actual host parent is not owned fixture")
    require(type(launch.get("creation100ns")) is int and launch["creation100ns"] > 0
            and parent["creation100ns"] == launch["creation100ns"]
            and host["creation100ns"] >= parent["creation100ns"], "creation identity disagreement")
    require(parent["active"] is True and host["active"] is True, "process exited before binding")
    require(same_path(parent["image"], expected_parent)
            and same_path(launch["actualExecutable"], expected_parent)
            and same_path(launch["argv"][0], expected_parent)
            and same_path(host["image"], expected_host), "process image disagreement")
    require(witness.get("desktop") == launch.get("desktop") == desktop_name
            and witness.get("inputDesktop") == launch.get("inputBefore") == input_desktop
            and desktop_name.casefold() != input_desktop.casefold()
            and witness.get("windowStation") == "WinSta0", "non-input desktop identity disagreement")
    require(positive_int(witness.get("threadId")) and positive_int(witness.get("windowThreadId")),
            "native thread identities absent")


class ProcessEntry(ct.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ct.c_size_t), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD), ("szExeFile", wt.WCHAR * 260)]


class ThreadEntry(ct.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class ModuleEntry(ct.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ct.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                ("szModule", wt.WCHAR * 256), ("szExePath", wt.WCHAR * 260)]


class WctLock(ct.Structure):
    _fields_ = [("name", wt.WCHAR * 128), ("timeout", ct.c_int64), ("alertable", wt.BOOL)]


class WctThread(ct.Structure):
    _fields_ = [("pid", wt.DWORD), ("tid", wt.DWORD), ("waitTime", wt.DWORD),
                ("contextSwitches", wt.DWORD)]


class WctObject(ct.Union):
    _fields_ = [("lock", WctLock), ("thread", WctThread)]


class WctNode(ct.Structure):
    _fields_ = [("kind", ct.c_int), ("status", ct.c_int), ("object", WctObject)]


def filetime(value):
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


class ReadOnlyWindows:
    def __init__(self):
        require(os.name == "nt" and ct.sizeof(ct.c_void_p) == 8, "64-bit Windows Python required")
        require(ct.sizeof(WctNode) == 280 and WctNode.object.offset == 8, "WCT ABI mismatch")
        self.k = ct.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "OpenProcess": (wt.HANDLE, [wt.DWORD, wt.BOOL, wt.DWORD]),
            "OpenThread": (wt.HANDLE, [wt.DWORD, wt.BOOL, wt.DWORD]),
            "CloseHandle": (wt.BOOL, [wt.HANDLE]),
            "GetProcessId": (wt.DWORD, [wt.HANDLE]),
            "GetProcessIdOfThread": (wt.DWORD, [wt.HANDLE]),
            "GetThreadId": (wt.DWORD, [wt.HANDLE]),
            "WaitForSingleObject": (wt.DWORD, [wt.HANDLE, wt.DWORD]),
            "QueryFullProcessImageNameW": (wt.BOOL, [wt.HANDLE, wt.DWORD, wt.LPWSTR, ct.POINTER(wt.DWORD)]),
            "GetProcessTimes": (wt.BOOL, [wt.HANDLE] + [ct.POINTER(wt.FILETIME)] * 4),
            "GetThreadTimes": (wt.BOOL, [wt.HANDLE] + [ct.POINTER(wt.FILETIME)] * 4),
            "CreateToolhelp32Snapshot": (wt.HANDLE, [wt.DWORD, wt.DWORD]),
        }
        for prefix, entry in (("Process32", ProcessEntry), ("Thread32", ThreadEntry), ("Module32", ModuleEntry)):
            for suffix in ("First", "Next"):
                name = prefix + suffix + ("W" if prefix != "Thread32" else "")
                signatures[name] = (wt.BOOL, [wt.HANDLE, ct.POINTER(entry)])
        for name, (result, args) in signatures.items():
            fn = getattr(self.k, name)
            fn.restype, fn.argtypes = result, args

    def check(self, value, operation):
        if not value:
            raise OSError(ct.get_last_error(), operation)
        return value

    def close(self, handle):
        if handle:
            self.check(self.k.CloseHandle(handle), "CloseHandle(query/snapshot)")

    def process(self, handle):
        stamps = [wt.FILETIME() for _ in range(4)]
        self.check(self.k.GetProcessTimes(handle, *(ct.byref(item) for item in stamps)), "GetProcessTimes")
        size = wt.DWORD(32768)
        name = ct.create_unicode_buffer(size.value)
        self.check(self.k.QueryFullProcessImageNameW(handle, 0, name, ct.byref(size)), "QueryFullProcessImageNameW")
        status = self.k.WaitForSingleObject(handle, 0)
        require(status in (0, 258), "unexpected process HANDLE wait status")
        return dict(pid=self.k.GetProcessId(handle), creation100ns=filetime(stamps[0]),
                    exit100ns=filetime(stamps[1]), kernel100ns=filetime(stamps[2]),
                    user100ns=filetime(stamps[3]), image=name.value, active=status == 258)

    def entries(self, flag, pid, entry_type, prefix):
        handle = None
        # ERROR_BAD_LENGTH can be transient during DLL load/unload; do not spin.
        for _ in range(3):
            handle = self.k.CreateToolhelp32Snapshot(flag, pid)
            if handle != INVALID_HANDLE:
                break
            error = ct.get_last_error()
            if error != 24:
                raise OSError(error, "CreateToolhelp32Snapshot")
        if handle == INVALID_HANDLE:
            raise OSError(24, "module list changed across three bounded attempts")
        try:
            row = entry_type()
            row.dwSize = ct.sizeof(row)
            suffix = "" if prefix == "Thread32" else "W"
            first = getattr(self.k, prefix + "First" + suffix)
            following = getattr(self.k, prefix + "Next" + suffix)
            ok = first(handle, ct.byref(row))
            count = 0
            while ok:
                count += 1
                require(count <= 131072, "Toolhelp traversal bound exceeded")
                yield row
                row = entry_type()
                row.dwSize = ct.sizeof(row)
                ok = following(handle, ct.byref(row))
            require(ct.get_last_error() == 18, "Toolhelp enumeration incomplete")
        finally:
            self.close(handle)

    def parent_pid(self, pid):
        matches = [row.th32ParentProcessID for row in self.entries(2, 0, ProcessEntry, "Process32")
                   if row.th32ProcessID == pid]
        require(len(matches) == 1, "owned host absent/ambiguous in process snapshot")
        return matches[0]

    def wait_chains(self, pid, tids):
        """No cross-process traversal, privileges, synchronous WCT or callbacks into COM."""
        result = dict(mode="asynchronous-flags-zero", collectionBudgetMs=200, maxThreads=16,
                      noPrivilegeAdjustment=True, rows=[], limitation="unsupported waits are not exonerated")
        if not tids:
            return result
        try:
            library = ct.WinDLL("advapi32", use_last_error=True)
            callback_type = ct.WINFUNCTYPE(None, wt.HANDLE, ct.c_size_t, wt.DWORD,
                                          ct.POINTER(wt.DWORD), ct.POINTER(WctNode), ct.POINTER(wt.BOOL))
            pending = {}

            def callback(_session, context, status, count, nodes, cycle):
                request = pending.get(context)
                if request is None:
                    return
                row = request["row"]
                try:
                    row["status"] = int(status)
                    if status in (0, 234, 565) and count and nodes:
                        row["reportedNodeCount"] = count[0]
                        row["isCycle"] = bool(cycle and cycle[0])
                        row["nodes"] = []
                        for index in range(min(count[0], 16)):
                            node = nodes[index]
                            item = dict(kind=node.kind, status=node.status)
                            if node.kind == 8:
                                thread = node.object.thread
                                if thread.pid == pid:
                                    item.update(pid=thread.pid, tid=thread.tid, waitTime=thread.waitTime,
                                                contextSwitches=thread.contextSwitches)
                                else:
                                    item["outsideOwnedProcessRedacted"] = True
                            else:
                                # Named synchronization objects can carry unrelated app data.
                                item["objectNameOmitted"] = True
                            row["nodes"].append(item)
                except BaseException as exc:
                    row["callbackError"] = str(exc)
                finally:
                    request["done"].set()

            callback_fn = callback_type(callback)
            library.OpenThreadWaitChainSession.restype = wt.HANDLE
            library.OpenThreadWaitChainSession.argtypes = [wt.DWORD, callback_type]
            library.GetThreadWaitChain.restype = wt.BOOL
            library.GetThreadWaitChain.argtypes = [wt.HANDLE, ct.c_size_t, wt.DWORD, wt.DWORD,
                ct.POINTER(wt.DWORD), ct.POINTER(WctNode), ct.POINTER(wt.BOOL)]
            library.CloseThreadWaitChainSession.restype = None
            library.CloseThreadWaitChainSession.argtypes = [wt.HANDLE]
            session = self.check(library.OpenThreadWaitChainSession(1, callback_fn), "OpenThreadWaitChainSession")
            try:
                for context, tid in enumerate(tids[:16], 1):
                    row = dict(tid=tid, completedBeforeCancellation=False)
                    request = dict(row=row, count=wt.DWORD(16), nodes=(WctNode * 16)(),
                                   cycle=wt.BOOL(), done=threading.Event())
                    pending[context] = request
                    result["rows"].append(row)
                    ok = library.GetThreadWaitChain(session, context, 0, tid, ct.byref(request["count"]),
                                                    request["nodes"], ct.byref(request["cycle"]))
                    error = ct.get_last_error() if not ok else 0
                    row["requestError"] = error
                    if error not in (0, 997):
                        request["done"].set()
                deadline = time.monotonic() + .2
                for request in pending.values():
                    request["done"].wait(max(0, deadline - time.monotonic()))
                    request["row"]["completedBeforeCancellation"] = request["done"].is_set()
            finally:
                # All output buffers/callbacks stay alive through documented cancellation.
                began = time.monotonic()
                library.CloseThreadWaitChainSession(session)
                result["cancelCloseMs"] = round((time.monotonic() - began) * 1000, 3)
                result["sessionClosed"] = True
        except Exception as exc:
            result["unavailable"] = f"{type(exc).__name__}: {exc}"
        return result

    def snapshot(self, handle, bound, main_tid):
        current = self.process(handle)
        require(current["pid"] == bound["pid"] and current["creation100ns"] == bound["creation100ns"]
                and same_path(current["image"], bound["image"]), "owned handle identity changed")
        result = dict(process=current, threadRows=[], modules=[], debuggerAttached=False, threadSuspended=False)
        if not current["active"]:
            result["skipped"] = "exact-process-handle-already-signaled"
            return result
        handles = []
        try:
            try:
                rows = [dict(tid=row.th32ThreadID, basePriority=row.tpBasePri)
                        for row in self.entries(4, 0, ThreadEntry, "Thread32")
                        if row.th32OwnerProcessID == bound["pid"]]
                rows.sort(key=lambda row: (row["tid"] != main_tid, row["tid"]))
                result["ownedThreadsObserved"] = len(rows)
                result["threadsTruncated"] = len(rows) > 512
                for row in rows[:512]:
                    thread = self.k.OpenThread(QUERY_THREAD, False, row["tid"])
                    if not thread:
                        row["unavailableWin32"] = ct.get_last_error()
                        result["threadRows"].append(row)
                        continue
                    handles.append(thread)
                    owner = self.k.GetProcessIdOfThread(thread)
                    actual_tid = self.k.GetThreadId(thread)
                    if owner != bound["pid"] or actual_tid != row["tid"]:
                        row["identityMismatchSkipped"] = True
                    else:
                        row["exactOwnedThread"] = True
                        stamps = [wt.FILETIME() for _ in range(4)]
                        if self.k.GetThreadTimes(thread, *(ct.byref(item) for item in stamps)):
                            row.update(zip(("creation100ns", "exit100ns", "kernel100ns", "user100ns"),
                                           (filetime(value) for value in stamps)))
                        else:
                            row["timesUnavailableWin32"] = ct.get_last_error()
                    result["threadRows"].append(row)
                eligible = [row["tid"] for row in result["threadRows"]
                            if row.get("exactOwnedThread") and row.get("exit100ns") == 0]
                result["waitChains"] = self.wait_chains(bound["pid"], eligible)
            except Exception as exc:
                result["threadSnapshotUnavailable"] = f"{type(exc).__name__}: {exc}"
            try:
                for row in self.entries(8 | 16, bound["pid"], ModuleEntry, "Module32"):
                    require(row.th32ProcessID == bound["pid"], "unexpected module owner")
                    require(len(result["modules"]) < 2048, "module output bound exceeded")
                    result["modules"].append(dict(name=row.szModule, path=row.szExePath,
                        base=hex(row.modBaseAddr or 0), bytes=row.modBaseSize))
            except Exception as exc:
                result["moduleSnapshotUnavailable"] = f"{type(exc).__name__}: {exc}"
        finally:
            for thread in handles:
                self.close(thread)
        result["processAfterSnapshot"] = self.process(handle)
        return result


class DumpIo(ct.Structure):
    # Windows SDK minidumpapiset.h uses pshpack4, including on x64.
    _pack_ = 4
    _fields_ = [("handle", ct.c_void_p), ("offset", ct.c_uint64),
                ("buffer", ct.c_void_p), ("bytes", ct.c_uint32)]


class DumpInputPrefix(ct.Structure):
    # Only the common prefix and Io/include-thread union prefix are accessed.
    # Never allocate this shortened view as the native callback input object.
    _pack_ = 4
    _fields_ = [("pid", ct.c_uint32), ("handle", ct.c_void_p),
                ("kind", ct.c_uint32), ("io", DumpIo)]


def bounded_dump_write(offset, count, total):
    require(type(offset) is int and type(count) is int and type(total) is int
            and 0 <= offset <= DUMP_MAX_BYTES and 0 <= count <= DUMP_MAX_WRITE_BYTES
            and offset + count <= DUMP_MAX_BYTES and 0 <= total <= 2 * DUMP_MAX_BYTES
            and total + count <= 2 * DUMP_MAX_BYTES, "minidump output budget exceeded")


def validate_dump_identity(request, parent, host, parent_pid):
    """Revalidate the saved binding immediately before MiniDumpWriteDump."""
    binding = request["binding"]
    prove_host(binding["originalLaunch"], binding["nativeDesktopWitness"], parent, host,
               binding["parent"]["image"], binding["host"]["image"],
               binding["nativeDesktopWitness"]["desktop"],
               binding["nativeDesktopWitness"]["inputDesktop"], parent_pid)
    for actual, saved in ((parent, binding["parent"]), (host, binding["host"])):
        require(actual["pid"] == saved["pid"] and actual["creation100ns"] == saved["creation100ns"]
                and same_path(actual["image"], saved["image"]), "minidump process identity changed")
    require(request["mainThreadId"] == binding["nativeDesktopWitness"]["threadId"],
            "minidump main thread is not the native witnessed thread")


def supervise_dump_helper(process):
    """Only a freshly created Popen object is passed, never a target PID.

    Windows Popen.kill uses its retained original process HANDLE. A timeout is
    not a safe cancellation claim: the helper may have been inside DbgHelp.
    """
    result = dict(hardTimeoutSeconds=3, helperKilled=False, helperSettled=False,
                  targetResumptionUnproven=False)
    try:
        result["exitCode"] = process.wait(timeout=3)
        result["helperSettled"] = True
    except subprocess.TimeoutExpired:
        result.update(timedOut=True, targetResumptionUnproven=True)
        try:
            process.kill()  # ONLY this helper's original Popen HANDLE.
            result["helperKilled"] = True
            result["exitCode"] = process.wait(timeout=.5)
            result["helperSettled"] = True
        except Exception as exc:
            result["cleanupError"] = f"{type(exc).__name__}: {exc}"
    except Exception as exc:
        result.update(waitError=f"{type(exc).__name__}: {exc}", targetResumptionUnproven=True)
        try:
            process.kill()  # Still ONLY the helper's retained original HANDLE.
            result["helperKilled"] = True
            result["exitCode"] = process.wait(timeout=.5)
            result["helperSettled"] = True
        except Exception as cleanup:
            result["cleanupError"] = f"{type(cleanup).__name__}: {cleanup}"
    return result


def validate_dump_flags(flags):
    # DbgHelp can write AVX context metadata although only ThreadInfo was requested.
    # This exact observed register-context addition does not authorize extra memory,
    # handles, IPT, triage filtering, or any other flags. Never change DUMP_FLAGS.
    # SDK minidumpapiset.h / Microsoft MINIDUMP_TYPE: AVX=0x00200000, triage=0x00100000.
    require(type(flags) is int and flags in (DUMP_FLAGS, DUMP_FLAGS | DUMP_OPTIONAL_AVX_CONTEXT),
            "minidump flags outside requested ThreadInfo and optional AVX context")
    return dict(requestedFlags=DUMP_FLAGS, headerFlags=flags,
                additionalContextFlags=flags & ~DUMP_FLAGS,
                avxXStateContextPresent=bool(flags & DUMP_OPTIONAL_AVX_CONTEXT))


def inspect_dump(path, expected_tid):
    """Bounded format sanity only; a valid container is not a valid backtrace."""
    size = path.stat().st_size
    require(32 <= size <= DUMP_MAX_BYTES, "minidump size outside bound")
    with path.open("rb") as stream:
        data = stream.read(DUMP_MAX_BYTES + 1)
    signature, _, count, rva, _, _, flags = struct.unpack_from("<IIIIIIQ", data)
    require(signature == 0x504d444d and 0 < count <= 128,
            "minidump header/type mismatch")
    flag_report = validate_dump_flags(flags)
    require(32 <= rva and rva + count * 12 <= size, "minidump directory outside file")
    entries = {}
    for index in range(count):
        kind, length, offset = struct.unpack_from("<III", data, rva + index * 12)
        if kind == 0:  # Unused directory slots are not duplicate data streams.
            continue
        require(kind not in entries and offset + length <= size, "invalid/duplicate minidump stream")
        entries[kind] = (length, offset)
    require(all(kind in entries for kind in (3, 4, 17)), "thread/module/thread-info streams missing")
    length, offset = entries[3]
    require(length >= 52 and struct.unpack_from("<I", data, offset)[0] == 1,
            "minidump must contain only the witnessed main thread")
    require(struct.unpack_from("<I", data, offset + 4)[0] == expected_tid, "wrong minidump thread")
    stack_bytes, stack_rva = struct.unpack_from("<II", data, offset + 4 + 32)
    context_bytes, context_rva = struct.unpack_from("<II", data, offset + 4 + 40)
    require(stack_bytes > 0 and stack_rva + stack_bytes <= size
            and context_bytes > 0 and context_rva + context_bytes <= size,
            "minidump stack/context absent or outside file")
    return dict(streamCount=count, mainThreadId=expected_tid, stackBytes=stack_bytes,
                contextBytes=context_bytes, backtraceValidated=False, **flag_report)


def dump_helper(request_path):
    """Internal, one-shot external process; no native/GPU workload is started."""
    require(os.name == "nt" and ct.sizeof(ct.c_void_p) == 8, "dump helper requires x64 Windows Python")
    request_path = request_path.resolve(strict=True)
    directory = request_path.parent
    require(request_path.name == "minidump-request.json"
            and directory.is_relative_to((ROOT / "AutoTest/artifacts").resolve()),
            "dump helper output outside authorized artifact directory")
    require(request_path.stat().st_size <= 65536, "dump request too large")
    request = strict_json(request_path.read_text("utf-8"))
    require(request["schema"] == "warvk-exit-minidump-request/v1"
            and same_path(request["outputDirectory"], directory), "dump request scope")
    preset = validate_dump_preset(request, directory)
    result = dict(schema="warvk-exit-minidump-helper/v1", ok=False, acceptancePassed=False,
                  casePreset=request["casePreset"],
                  diagnosticInterferencePossible=True, targetExplicitSuspendResumeKill=False,
                  dbghelpMaySuspendSelectedThread=True, targetProcessAccess=DUMP_TARGET_ACCESS,
                  flags=DUMP_FLAGS, maxBytes=DUMP_MAX_BYTES, softCancelSeconds=1.5, errors=[])
    api = None
    host_handle = parent_handle = thread_handle = None
    try:
        require(identity(DBGHELP) == request["dbghelp"] and pe_identity(DBGHELP) == (0x8664, 0x20b),
                "DbgHelp installation changed or wrong width")
        for role in ("host", "parent"):
            image = directory / preset["binaries"][role][0]
            validate_frozen_binary(preset, role, identity(image))
        api = ReadOnlyWindows()
        host_pid = request["binding"]["host"]["pid"]
        parent_pid = request["binding"]["parent"]["pid"]
        main_tid = request["mainThreadId"]
        require(positive_int(host_pid) and positive_int(parent_pid) and positive_int(main_tid), "dump identity range")
        host_handle = api.check(api.k.OpenProcess(DUMP_TARGET_ACCESS, False, host_pid), "OpenProcess(dump target query/read)")
        parent_handle = api.check(api.k.OpenProcess(QUERY_PROCESS, False, parent_pid), "OpenProcess(dump parent query)")
        thread_handle = api.check(api.k.OpenThread(QUERY_THREAD, False, main_tid), "OpenThread(dump main query)")
        require(api.k.GetProcessIdOfThread(thread_handle) == host_pid and api.k.GetThreadId(thread_handle) == main_tid,
                "dump main thread owner changed")
        require(api.k.WaitForSingleObject(thread_handle, 0) == 258, "dump main thread already exited")
        require(DumpInputPrefix.kind.offset == 12 and DumpInputPrefix.io.offset == 16
                and ct.sizeof(DumpIo) == 28, "minidump SDK pack4 ABI mismatch")
        # Explicit SDK path plus DLL-load-directory/System32 lookup, not cwd.
        library = ct.WinDLL(str(DBGHELP), use_last_error=True, winmode=0x100 | 0x800)
        callback_type = ct.WINFUNCTYPE(wt.BOOL, ct.c_void_p, ct.c_void_p, ct.c_void_p)

        class CallbackInfo(ct.Structure):
            _pack_ = 4
            _fields_ = [("callback", callback_type), ("parameter", ct.c_void_p)]

        native = library.MiniDumpWriteDump
        native.restype = wt.BOOL
        native.argtypes = [wt.HANDLE, wt.DWORD, wt.HANDLE, ct.c_uint32,
                          ct.c_void_p, ct.c_void_p, ct.POINTER(CallbackInfo)]
        state = dict(ioStarted=False, ioFinished=False, totalWritten=0, maxExtent=0,
                     includedThread=False, canceled=False, callbackErrors=[])
        dump_path = directory / "host-main-exit.dmp"
        with dump_path.open("xb", buffering=0) as stream:
            import msvcrt
            file_handle = msvcrt.get_osfhandle(stream.fileno())
            started = time.monotonic()

            def callback(_parameter, input_ptr, output_ptr):
                try:
                    require(input_ptr and output_ptr, "empty dump callback")
                    data = ct.cast(input_ptr, ct.POINTER(DumpInputPrefix)).contents
                    out = ct.cast(output_ptr, ct.POINTER(ct.c_uint32))
                    expired = time.monotonic() - started >= 1.5
                    if data.kind == 6:  # CancelCallback
                        state["canceled"] = state["canceled"] or expired
                        out[0], out[1] = 1, int(state["canceled"])
                    elif data.kind == 3:  # IncludeThreadCallback union begins with ULONG tid.
                        tid = ct.c_uint32.from_address(input_ptr + DumpInputPrefix.io.offset).value
                        if tid != main_tid:
                            return False
                        state["includedThread"] = True
                    elif data.kind == 11:  # IoStartCallback: S_FALSE delegates ALL writes.
                        state["ioStarted"] = True
                        out[0] = 1
                    elif data.kind == 12:  # IoWriteAllCallback
                        require(state["ioStarted"] and not expired, "dump canceled before write")
                        io = data.io
                        require(io.handle == file_handle and (io.bytes == 0 or io.buffer), "dump I/O identity")
                        bounded_dump_write(io.offset, io.bytes, state["totalWritten"])
                        stream.seek(io.offset)
                        payload = ct.string_at(io.buffer, io.bytes) if io.bytes else b""
                        require(stream.write(payload) == io.bytes, "partial dump write")
                        state["totalWritten"] += io.bytes
                        state["maxExtent"] = max(state["maxExtent"], io.offset + io.bytes)
                        out[0] = 0
                    elif data.kind == 13:  # IoFinishCallback
                        require(not expired, "dump canceled at completion")
                        state["ioFinished"] = True
                        out[0] = 0
                    elif data.kind in (5, 7, 9, 14):
                        # No extra memory/kernel dump, no masking read failures.
                        return False
                    elif data.kind in (16, 17):
                        out[0] = 0  # Actual process, no VM/snapshot callbacks.
                    return True
                except Exception as exc:
                    state["canceled"] = True
                    if len(state["callbackErrors"]) < 16:
                        state["callbackErrors"].append(str(exc))
                    return False

            callback_fn = callback_type(callback)
            info = CallbackInfo(callback_fn, None)
            parent, host = api.process(parent_handle), api.process(host_handle)
            validate_dump_identity(request, parent, host, api.parent_pid(host_pid))
            result["identityImmediatelyBeforeWrite"] = dict(parent=parent, host=host, mainThreadId=main_tid)
            result["writeStarted"] = True
            written = native(host_handle, host_pid, file_handle, DUMP_FLAGS, None, None, ct.byref(info))
            result["miniDumpHresult"] = ct.get_last_error() if not written else 0
            result["writeMs"] = round((time.monotonic() - started) * 1000, 3)
            result["callback"] = state
            require(bool(written) and state["ioStarted"] and state["ioFinished"]
                    and state["includedThread"] and not state["canceled"] and not state["callbackErrors"],
                    "MiniDumpWriteDump incomplete/canceled")
        result["format"] = inspect_dump(dump_path, main_tid)
        result["dump"] = identity(dump_path)
        result["targetAfterWrite"] = api.process(host_handle)
        result["ok"] = True
    except Exception as exc:
        result["errors"].append(f"{type(exc).__name__}: {exc}")
    finally:
        if api:
            for handle in (thread_handle, host_handle, parent_handle):
                try:
                    api.close(handle)
                except Exception as exc:
                    result["errors"].append(str(exc))
        result["ok"] = result["ok"] and not result["errors"]
        write_json(directory / "minidump-helper-result.json", result)
    return 0 if result["ok"] else 1


def capture_minidump(api, host_handle, binding, directory, dbghelp_identity, case_name):
    result = dict(requested=True, acceptancePassed=False, diagnosticInterferencePossible=True,
                  dbghelpMaySuspendSelectedThread=True, targetResumptionUnproven=False)
    current = api.process(host_handle)
    require(current["pid"] == binding["host"]["pid"]
            and current["creation100ns"] == binding["host"]["creation100ns"]
            and same_path(current["image"], binding["host"]["image"]), "dump supervisor binding changed")
    if not current["active"]:
        result["skipped"] = "original-host-handle-signaled-before-dump"
        return result
    request_path = directory / "minidump-request.json"
    write_json(request_path, dict(schema="warvk-exit-minidump-request/v1", casePreset=case_name,
               outputDirectory=str(directory),
               binding=binding, mainThreadId=binding["nativeDesktopWitness"]["threadId"], dbghelp=dbghelp_identity))
    command = [sys.executable, "-B", str(Path(__file__).resolve()), "--dump-helper", str(request_path)]
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() in {"SYSTEMROOT", "WINDIR", "PATH", "TEMP", "TMP", "USERPROFILE"}}
    result["python"] = identity(Path(sys.executable))
    started = time.monotonic()
    with (directory / "minidump-helper.stdout").open("xb") as stdout, \
            (directory / "minidump-helper.stderr").open("xb") as stderr:
        process = subprocess.Popen(command, cwd=directory, env=environment, stdin=subprocess.DEVNULL,
            stdout=stdout, stderr=stderr, close_fds=True,
            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        # Retain and query the original CreateProcess handle, never reopen/kill a PID.
        try:
            result["helperIdentity"] = api.process(int(process._handle))
        except Exception as exc:
            result["helperIdentityUnavailable"] = str(exc)
        result.update(supervise_dump_helper(process))
    result["elapsedMs"] = round((time.monotonic() - started) * 1000, 3)
    report_path = directory / "minidump-helper-result.json"
    if result["helperSettled"] and report_path.is_file():
        require(report_path.stat().st_size <= 65536, "helper result too large")
        result["helperResult"] = strict_json(report_path.read_text("utf-8"))
    result["ok"] = bool(result["helperSettled"] and not result.get("timedOut") and not result.get("waitError")
                        and result.get("exitCode") == 0 and result.get("helperResult", {}).get("ok") is True)
    result["targetCleanupOwner"] = "existing-native-fixture-and-isolated-Job-only"
    return result


class ExitMonitor(threading.Thread):
    def __init__(self, desktop, parent, host, directory, dbghelp_identity=None, case_name="r8-clear",
                 palette_image=None):
        super().__init__(name="owned-host-read-only-exit-monitor", daemon=True)
        self.desktop, self.parent, self.host, self.directory = desktop, parent, host, directory
        self.stop_event = threading.Event()
        self.dbghelp_identity = dbghelp_identity
        self.case_name = case_name
        self.palette_image = palette_image
        self.launcher_timeout = selected_preset(case_name)["launcherTimeoutSeconds"]
        self.result = dict(mode="query-plus-owned-main-thread-minidump" if dbghelp_identity else "non-debug-query-only",
                           samples=[], errors=[], identityCertified=False,
                           debuggerSkipped="bounded LLDB detach not proven", exitIntentObserved=False)
        if palette_image is not None:
            self.result["mode"] += "+bounded-read-only-palette-vmread"

    def run(self):
        api = None
        palette = palette_schedule = None
        palette_bound_at = None
        init_complete = False
        parent_handle = host_handle = None
        witness = bound = None
        exit_at = None
        cursor, remainder, samples = 0, b"", 0
        dump_attempted = False
        try:
            api = ReadOnlyWindows()
            deadline = time.monotonic() + self.launcher_timeout + 2
            while not self.stop_event.is_set() and time.monotonic() < deadline:
                log = self.directory / "host.log"
                if log.is_file():
                    with log.open("rb") as stream:
                        stream.seek(cursor)
                        part = stream.read(65536)
                    cursor += len(part)
                    require(cursor <= 8 * 1024 * 1024, "host log observation budget exceeded")
                    chunks = (remainder + part).split(b"\n")
                    remainder = chunks.pop()
                    require(len(remainder) <= 131072, "host log incomplete-line bound exceeded")
                    for raw in chunks:
                        line = raw.rstrip(b"\r").decode("utf-8")
                        if line.startswith("{"):
                            row = strict_json(line)
                            if row.get("schema") == "warvk-d3d9-desktop/v1":
                                require(witness is None, "duplicate native desktop witness")
                                witness = row
                        match = EXIT_INTENT.fullmatch(line)
                        if match:
                            require(exit_at is None, "duplicate exit-intent")
                            require(witness and int(match[1]) == witness["pid"], "exit-intent PID not witnessed")
                            exit_at = time.monotonic()
                            self.result.update(exitIntentObserved=True, exitIntentLine=line,
                                               hostElapsedAtExitIntentMs=int(match[2]))
                        if self.palette_image is not None:
                            init = re.fullmatch(r"D3D9 host init-apply complete result=([0-9]+) elapsedMs=([0-9]+)", line)
                            if init:
                                require(not init_complete, "duplicate init-complete observation")
                                init_complete = True
                                self.result["paletteInitComplete"] = dict(result=int(init[1]), elapsedMs=int(init[2]),
                                    observedMonotonicMs=round(time.monotonic() * 1000, 3))
                            milestone = re.fullmatch(r"D3D9 host exit-milestone pid=([1-9][0-9]*) tid=([1-9][0-9]*) "
                                r"phase=([a-z-]+) tickMs=([0-9]+) processElapsedMs=([0-9]+)", line)
                            if milestone:
                                require(witness and int(milestone[1]) == witness["pid"]
                                    and int(milestone[2]) == witness["threadId"], "milestone owner mismatch")
                                timeline = self.result.setdefault("paletteNativeMilestones", [])
                                require(len(timeline) < 8, "native milestone observation bound exceeded")
                                timeline.append(dict(phase=milestone[3], tickMs=int(milestone[4]),
                                    wmainElapsedMs=int(milestone[5]), observedMonotonicMs=round(time.monotonic()*1000, 3)))
                if witness and bound is None:
                    launches = [row for row in self.desktop.receipt["launches"]
                                if row.get("pid") == witness.get("peerPid") and row.get("creation100ns")
                                and row.get("actualExecutable")]
                    require(len(launches) <= 1, "ambiguous owned parent launch")
                    if launches:
                        launch = copy.deepcopy(launches[0])
                        parent_handle = api.check(api.k.OpenProcess(QUERY_PROCESS, False, launch["pid"]), "OpenProcess(parent)")
                        host_handle = api.check(api.k.OpenProcess(QUERY_PROCESS, False, witness["pid"]), "OpenProcess(host)")
                        parent, host = api.process(parent_handle), api.process(host_handle)
                        actual_parent = api.parent_pid(host["pid"])
                        prove_host(launch, witness, parent, host, self.parent, self.host,
                                   self.desktop.desktop_name, self.desktop.input_before, actual_parent)
                        bound = host
                        self.result["binding"] = dict(parent=parent, host=host, nativeDesktopWitness=witness,
                                                       actualParentPid=actual_parent, originalLaunch=launch)
                        if self.desktop.restricted_medium:
                            from render_lab_token import _NativeTokenApi, security_profile
                            token_api = _NativeTokenApi()
                            profiles = {}
                            for role, handle in (("parent", parent_handle), ("host", host_handle)):
                                token = token_api.open_process(handle)
                                try:
                                    profiles[role] = token_api.describe(token)
                                finally:
                                    token_api.close(token)
                            expected = launch["restrictedToken"]["childBeforeResume"]
                            require(profiles["host"]["integrityRid"] == 8192
                                and security_profile(profiles["host"]) == security_profile(profiles["parent"])
                                and security_profile(profiles["parent"]) == security_profile(expected),
                                "exact bound host/parent token does not match the restricted launch")
                            self.result["boundProcessTokens"] = dict(**profiles,
                                observedBeforeGpu=False, exactProcessHandlesAndCreationBound=True)
                        if self.palette_image is not None:
                            from render_lab_palette_watcher import NativePaletteApi, PaletteWatcherObserver, PreExitSchedule
                            palette = PaletteWatcherObserver(NativePaletteApi(api, ModuleEntry), bound, self.palette_image)
                            palette_schedule = PreExitSchedule()
                            palette_bound_at = time.monotonic()
                            self.result["paletteWatcher"] = palette.result
                            self.result["paletteClockBasis"] = dict(hostCreation100ns=bound["creation100ns"],
                                bindObservedWall100ns=time.time_ns()//100 + 116444736000000000,
                                bindObservedMonotonicMs=round(palette_bound_at*1000, 3),
                                hostProcessElapsedMeans="wmain-entry-not-process-creation",
                                logObservationIsExecutionTime=False)
                if palette is not None:
                    pre_elapsed = (time.monotonic() - palette_bound_at) * 1000
                    due = palette_schedule.due(pre_elapsed, init_complete, exit_at is not None)
                    if due:
                        palette.sample(due[0], due[1], round(pre_elapsed, 3))
                    palette.result["skippedPreSamples"] = list(palette_schedule.skipped)
                if exit_at is not None and bound is not None:
                    elapsed = (time.monotonic() - exit_at) * 1000
                    if self.dbghelp_identity and not dump_attempted and elapsed >= 1000:
                        dump_attempted = True
                        self.result["minidump"] = capture_minidump(api, host_handle, self.result["binding"],
                                                                 self.directory, self.dbghelp_identity, self.case_name)
                        self.result["minidump"]["requestedAfterExitIntentMs"] = 1000
                        self.result["minidump"]["observedAfterExitIntentMs"] = round(elapsed, 3)
                        if not self.result["minidump"].get("ok") and not self.result["minidump"].get("skipped"):
                            self.result["errors"].append("minidump failed; inspect helper result/timeout, never acceptance")
                        elapsed = (time.monotonic() - exit_at) * 1000
                    if samples < len(SNAPSHOT_AT_MS) and elapsed >= SNAPSHOT_AT_MS[samples]:
                        if palette is not None:
                            palette.sample(f"exit-{SNAPSHOT_AT_MS[samples]}", SNAPSHOT_AT_MS[samples],
                                round((time.monotonic()-exit_at)*1000, 3))
                        started = time.monotonic()
                        sample = api.snapshot(host_handle, bound, witness["threadId"])
                        sample.update(requestedAfterExitIntentMs=SNAPSHOT_AT_MS[samples],
                                      observedAfterExitIntentMs=round(elapsed, 3),
                                      collectionMs=round((time.monotonic() - started) * 1000, 3))
                        path = self.directory / f"exit-snapshot-{samples + 1}.json"
                        write_json(path, sample)
                        self.result["samples"].append(identity(path))
                        samples += 1
                        if sample.get("skipped") or samples == len(SNAPSHOT_AT_MS):
                            break
                self.stop_event.wait(.02)
            if not bound:
                self.result["samplingSkipped"] = "no-complete-live-owned-process-identity"
            elif not self.result["samples"]:
                self.result["samplingSkipped"] = "host-ended-before-post-exit-intent-sampling-window"
        except Exception as exc:
            self.result["errors"].append(f"{type(exc).__name__}: {exc}")
        finally:
            if palette is not None:
                palette.close()
                self.result["errors"].extend(palette.result["errors"])
            if api:
                for handle in (host_handle, parent_handle):
                    try:
                        api.close(handle)
                    except Exception as exc:
                        self.result["errors"].append(str(exc))


def frozen_inputs(receipt_path, case_name="r8-clear"):
    preset = selected_preset(case_name)
    original = strict_json(receipt_path.read_text("utf-8"))
    preserved_case_launch(original, receipt_path.parent, preset)
    case = receipt_path.parent / preset["caseDirectory"]
    files = []
    for role, (name, _, _) in preset["binaries"].items():
        path = case / name
        actual = identity(path)
        validate_frozen_binary(preset, role, actual)
        rows = [row for row in original["caseCopies"] if same_path(row["copy"]["path"], path)]
        require(len(rows) == 1 and payload_equal(rows[0]["source"], actual)
                and payload_equal(rows[0]["copy"], actual), "original copy receipt mismatch")
        files.append(dict(role=name, identity=actual, originalReceiptHashRecorded=True))
    probe_launch = original["desktopProbe"]
    require(probe_launch["ok"] is True and probe_launch["exitCode"] == 0, "original desktop probe failed")
    for bits in (32, 64):
        path = receipt_path.parent / f"desktop-probe-{bits}.exe"
        require(any(same_path(item, path) for item in probe_launch["argv"] if isinstance(item, str)
                    and item.lower().endswith(".exe")), "probe was not an original launched executable")
        require(pe_identity(path) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), "probe width")
        files.append(dict(role=path.name, identity=identity(path), originalReceiptHashRecorded=False,
                          provenance="preserved-original-probe-path; hash pinned now, not retrospectively proven"))
    require(pe_identity(case / preset["binaries"]["parent"][0])
            == pe_identity(case / preset["binaries"]["dll"][0]) == (0x14c, 0x10b),
            "frozen x86 input widths")
    require(pe_identity(case / preset["binaries"]["host"][0]) == (0x8664, 0x20b), "frozen x64 host width")
    return original, files


def self_test():
    """Pure rejection tests; no Windows API, files, Desktop, process or GPU."""
    here = str(Path(__file__).resolve())
    launch = dict(pid=12, creation100ns=100, argv=[here], actualExecutable=here,
                  jobAtomic=True, handleListApplied=True, desktop="lab", inputBefore="Default")
    witness = dict(schema="warvk-d3d9-desktop/v1", beforeGpu=True, pid=13, peerPid=12,
                   desktop="lab", inputDesktop="Default", windowStation="WinSta0", threadId=14, windowThreadId=15)
    parent = dict(pid=12, creation100ns=100, image=here, active=True)
    host = dict(pid=13, creation100ns=101, image=here, active=True)
    def check(values):
        prove_host(*values, here, here, "lab", "Default", 12)
    values = [launch, witness, parent, host]
    check(values)
    mutations = [(0, "creation100ns", 99), (0, "jobAtomic", False), (0, "handleListApplied", False),
                 (0, "actualExecutable", str(Path(here).with_name("other.exe"))),
                 (1, "pid", 99), (1, "peerPid", 99), (1, "beforeGpu", False),
                 (1, "desktop", "Default"), (1, "threadId", True), (1, "windowStation", "Other"),
                 (2, "creation100ns", 99), (2, "active", False), (3, "creation100ns", 1),
                 (3, "active", False), (3, "image", str(Path(here).with_name("other.exe")))]
    for index, field, value in mutations:
        changed = copy.deepcopy(values)
        changed[index][field] = value
        try:
            check(changed)
        except (ValueError, KeyError, TypeError):
            continue
        raise AssertionError("identity mutation survived: " + field)
    require(EXIT_INTENT.fullmatch("D3D9 host exit-intent pid=13 strategy=normal-return code=0 processElapsedMs=3"), "intent")
    for line in ("D3D9 host exit-intent pid=13 strategy=exit-process code=0 processElapsedMs=3",
                 "D3D9 host exit-intent pid=13 strategy=normal-return code=1 processElapsedMs=3"):
        require(EXIT_INTENT.fullmatch(line) is None, "unexpected intent accepted")
    request = dict(binding=dict(originalLaunch=launch, nativeDesktopWitness=witness,
                                parent=parent, host=host), mainThreadId=14)
    validate_dump_identity(request, parent, host, 12)
    dump_mutations = 0
    for field, value in (("pid", 99), ("creation100ns", 102), ("active", False),
                         ("image", str(Path(here).with_name("unrelated.exe")))):
        changed = host | {field: value}
        try:
            validate_dump_identity(request, parent, changed, 12)
        except (ValueError, KeyError, TypeError):
            dump_mutations += 1
        else:
            raise AssertionError("dump identity mutation survived: " + field)
    for observed_parent in (13, 99):
        try:
            validate_dump_identity(request, parent, host, observed_parent)
        except ValueError:
            dump_mutations += 1
        else:
            raise AssertionError("dump parent mismatch survived")
    bounded_dump_write(DUMP_MAX_BYTES - 32, 32, 0)
    budget_cases = ((-1, 1, 0), (DUMP_MAX_BYTES, 1, 0), (0, DUMP_MAX_WRITE_BYTES + 1, 0),
                    (0, 1, 2 * DUMP_MAX_BYTES), (True, 1, 0))
    for values in budget_cases:
        try:
            bounded_dump_write(*values)
        except ValueError:
            continue
        raise AssertionError("minidump bound mutation survived")
    require(validate_dump_flags(DUMP_FLAGS)["additionalContextFlags"] == 0, "requested dump flags refused")
    require(validate_dump_flags(DUMP_FLAGS | DUMP_OPTIONAL_AVX_CONTEXT)["additionalContextFlags"]
            == DUMP_OPTIONAL_AVX_CONTEXT, "observed AVX context flags refused")
    invalid_flags = [0, DUMP_OPTIONAL_AVX_CONTEXT, -1, True, None, 1 << 64]
    invalid_flags.extend(DUMP_FLAGS | (1 << bit) for bit in range(64) if bit not in (12, 21))
    for flags in invalid_flags:
        try:
            validate_dump_flags(flags)
        except ValueError:
            continue
        raise AssertionError("dump flag mutation survived: " + str(flags))

    class FakeHelper:
        def __init__(self, behavior):
            self.behavior, self.waits, self.kills = behavior, [], 0
        def wait(self, timeout):
            self.waits.append(timeout)
            if self.behavior == "normal":
                return 0
            if self.behavior == "waiterror" and len(self.waits) == 1:
                raise OSError("simulated wait failure")
            if len(self.waits) == 1 or self.behavior == "unsettled":
                raise subprocess.TimeoutExpired("owned-helper", timeout)
            return 1
        def kill(self):
            self.kills += 1

    normal = FakeHelper("normal")
    report = supervise_dump_helper(normal)
    require(report["helperSettled"] and not report["helperKilled"] and normal.kills == 0, "normal helper killed")
    timed = FakeHelper("timeout")
    report = supervise_dump_helper(timed)
    require(report["timedOut"] and report["helperKilled"] and report["helperSettled"]
            and report["targetResumptionUnproven"] and timed.kills == 1 and timed.waits == [3, .5],
            "helper timeout not bounded/fail-visible")
    unsettled = FakeHelper("unsettled")
    report = supervise_dump_helper(unsettled)
    require(not report["helperSettled"] and report["cleanupError"] and report["targetResumptionUnproven"],
            "unsettled helper wrongly accepted")
    failed_wait = FakeHelper("waiterror")
    report = supervise_dump_helper(failed_wait)
    require(report["waitError"] and report["helperSettled"] and failed_wait.kills == 1
            and report["targetResumptionUnproven"], "helper wait exception lost ownership cleanup")
    preset_mutations = 0

    def must_reject(action):
        nonlocal preset_mutations
        try:
            action()
        except (ValueError, KeyError, TypeError):
            preset_mutations += 1
            return
        raise AssertionError("frozen preset mutation survived")

    for name in ("unknown", "r10", None, True):
        must_reject(lambda name=name: selected_preset(name))
    original_directory = Path(here).parent / "pure-original"
    output_directory = Path(here).parent / "pure-diagnostic"
    for case_name, preset in PRESETS.items():
        original_launch = dict(timeoutSeconds=preset["launcherTimeoutSeconds"],
                               desktop="old-lab", inputBefore="Default",
                               argv=preset_argv(preset, original_directory / preset["caseDirectory"],
                                                "old-lab", "Default"))
        original_case = dict(label=preset["caseDirectory"], process=original_launch,
                             reports=[dict(schema=preset["fixtureSchema"], ok=True)])
        original = dict(scope="D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP", ok=False, cases=[original_case])
        require(preserved_case_launch(original, original_directory, preset) == original_launch,
                "valid frozen case refused")
        directory = output_directory / preset["caseDirectory"]
        mapped = remap_preserved_argv(original_launch, preset, directory, "new-lab", "Default")
        require(mapped == preset_argv(preset, directory, "new-lab", "Default"), "frozen argv remap")
        changes = {0, 2} | set(preset["desktopArgumentPositions"])
        require(all(mapped[index] == value for index, value in enumerate(original_launch["argv"])
                    if index not in changes), "original non-path arguments changed")
        changed_receipts = []
        for field, value in (("timeoutSeconds", 600), ("desktop", "Default")):
            changed = copy.deepcopy(original)
            changed["cases"][0]["process"][field] = value
            changed_receipts.append(changed)
        for index, value in ((0, str(directory / "wrong-parent.exe")),
                             (2, str(directory / "wrong.dll")), (4, "fault-injection")):
            changed = copy.deepcopy(original)
            changed["cases"][0]["process"]["argv"][index] = value
            changed_receipts.append(changed)
        changed = copy.deepcopy(original)
        changed["cases"][0]["process"]["argv"].append("--extra")
        changed_receipts.append(changed)
        changed = copy.deepcopy(original)
        changed["cases"][0]["reports"][0]["schema"] = "wrong-fixture/v1"
        changed_receipts.append(changed)
        changed_receipts.extend([original | {"ok": True}, original | {"cases": []},
                                 original | {"cases": [original_case, original_case]}])
        for changed in changed_receipts:
            must_reject(lambda changed=changed: preserved_case_launch(changed, original_directory, preset))
        for role, (name, size, digest) in preset["binaries"].items():
            binary = dict(path=str(directory / name), bytes=size, sha256=digest)
            validate_frozen_binary(preset, role, binary)
            for field, value in (("path", str(directory / "wrong.exe")), ("bytes", size + 1),
                                 ("sha256", "0" * 64)):
                must_reject(lambda field=field, value=value, role=role, binary=binary:
                            validate_frozen_binary(preset, role, binary | {field: value}))
        dump_request = dict(casePreset=case_name, outputDirectory=str(directory), binding={
            role: dict(image=str(directory / preset["binaries"][role][0])) for role in ("parent", "host")})
        require(validate_dump_preset(dump_request, directory) is preset, "dump preset refused")
        other_name = next(name for name in PRESETS if name != case_name)
        for field, value in (("casePreset", other_name), ("casePreset", "unlisted"),
                             ("outputDirectory", str(output_directory / "wrong-case"))):
            must_reject(lambda field=field, value=value:
                        validate_dump_preset(dump_request | {field: value}, directory))
        for role in ("parent", "host"):
            changed = copy.deepcopy(dump_request)
            changed["binding"][role]["image"] = str(output_directory / preset["binaries"][role][0])
            must_reject(lambda changed=changed: validate_dump_preset(changed, directory))
        # Both presets deliberately share DLL/host filenames, never their hashes.
        for role in ("dll", "host"):
            name, size, digest = selected_preset(other_name)["binaries"][role]
            must_reject(lambda role=role, name=name, size=size, digest=digest:
                        validate_frozen_binary(preset, role, dict(path=str(directory / name), bytes=size, sha256=digest)))
    print(json.dumps(dict(scope="PURE_DIAGNOSTIC_IDENTITY_READER", positive=1,
                          identityMutationsKilled=len(mutations), exitIntentCases=3,
                          dumpIdentityMutationsKilled=dump_mutations, dumpBudgetCases=len(budget_cases) + 1,
                          dumpFlagCases=2, dumpFlagMutationsKilled=len(invalid_flags),
                          fakeHelperSettlementCases=4, frozenPresetCases=len(PRESETS),
                          frozenPresetMutationsKilled=preset_mutations, nativeRun=False)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=tuple(PRESETS), default="r8-clear",
                        help="explicit frozen case (default: r8-clear)")
    parser.add_argument("--receipt", type=Path, help="preserved receipt.json for the selected frozen case")
    parser.add_argument("--output", type=Path, help="new directory; never replaces evidence")
    parser.add_argument("--implicit-layers-disabled", action="store_true")
    parser.add_argument("--empty-implicit-layer-path", action="store_true",
                        help="diagnostic only: owned empty manifest source, requires medium and disabled layers")
    parser.add_argument("--empty-explicit-layer-path", action="store_true",
                        help="also isolate unused explicit discovery; requires empty implicit source")
    parser.add_argument("--restricted-medium", action="store_true",
                        help="derived medium-integrity child only; no global security changes")
    parser.add_argument("--capture-minidump", action="store_true",
                        help="opt-in external main-thread dump; DbgHelp may transiently suspend target, never acceptance")
    parser.add_argument("--observe-palette-watcher", action="store_true",
                        help="opt-in bounded read-only frozen OpenGL TID/thread observations; no exit repair")
    parser.add_argument("--dump-helper", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.dump_helper:
        require(not any((args.receipt, args.output, args.capture_minidump, args.self_test,
                         args.implicit_layers_disabled, args.restricted_medium,
                         args.empty_implicit_layer_path, args.empty_explicit_layer_path,
                         args.observe_palette_watcher)) and args.case == "r8-clear",
                "internal dump-helper options cannot be combined")
        return dump_helper(args.dump_helper)
    if args.self_test:
        self_test()
        return 0
    require(args.receipt and args.output, "--receipt and --output required")
    require(not args.empty_implicit_layer_path or (args.restricted_medium and args.implicit_layers_disabled),
            "empty manifest source requires verified Medium token and explicit disabled-layers comparison")
    require(not args.empty_explicit_layer_path or args.empty_implicit_layer_path,
            "explicit discovery isolation requires implicit discovery isolation")
    require(os.name == "nt" and ct.sizeof(ct.c_void_p) == 8, "64-bit Windows Python required")
    receipt_path = args.receipt.resolve(strict=True)
    preset = selected_preset(args.case)
    original, inputs = frozen_inputs(receipt_path, args.case)
    original_launch = preserved_case_launch(original, receipt_path.parent, preset)
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    record = dict(schema="warvk-presenter-exit-diagnostic/v1", diagnosticCompleted=False,
                  casePreset=args.case, originalCaseLaunch=original_launch,
                  acceptancePassed=False, originalFailureOverridden=False, rebuilt=False,
                  gameTouched=False, playerDllChanged=False, productIntegrated=False,
                  debuggerAttached=False, nativeDeadlineMs=5000,
                  launcherTimeoutSeconds=preset["launcherTimeoutSeconds"],
                  implicitLayersDisableRequested=args.implicit_layers_disabled,
                  emptyImplicitLayerPathRequested=args.empty_implicit_layer_path,
                  emptyExplicitLayerPathRequested=args.empty_explicit_layer_path,
                  restrictedMediumRequested=args.restricted_medium,
                  minidumpRequested=args.capture_minidump, paletteWatcherRequested=args.observe_palette_watcher,
                  diagnosticInterferencePossible=args.capture_minidump or args.observe_palette_watcher,
                  originalReceipt=identity(receipt_path), originalError=original.get("error"),
                  originalOk=original.get("ok"), inputs=inputs, errors=[])
    protected = []
    monitor = None
    native_clean = False
    dbghelp_identity = None
    palette_image = None
    try:
        protected.extend(identity(path) for path in (Path(__file__),
            ROOT / "AutoTest/isolated_render_desktop.py", ROOT / "AutoTest/run_render_host_protocol_gate.py",
            ROOT / "AutoTest/run_render_host_transport_gate.py", ROOT / "AutoTest/render_lab_token.py",
            ROOT / "AutoTest/render_lab_default_dacl.py", ROOT / "AutoTest/render_lab_layer_source.py",
            ROOT / "AutoTest/test_render_lab_layer_source.py"))
        for key in ("referenceBefore", "playerBefore"):
            path = Path(original[key]["path"])
            require(path.is_file(), "preservation witness missing: " + key)
            protected.append(identity(path))
        record["protectedBefore"] = protected
        if args.observe_palette_watcher:
            from render_lab_palette_watcher import FrozenImage, PE_PATH, PDB_PATH
            palette_image = FrozenImage.load()  # Pure pinned-file preflight BEFORE any native launch.
            protected.extend(identity(path) for path in (PE_PATH, PDB_PATH,
                ROOT / "AutoTest/render_lab_palette_watcher.py", ROOT / "AutoTest/test_render_lab_palette_watcher.py"))
            record["paletteFrozenImage"] = palette_image.receipt()
        if args.capture_minidump:
            require(output.is_relative_to((ROOT / "AutoTest/artifacts").resolve()),
                    "minidump output must be within this repository's AutoTest/artifacts")
            dbghelp_identity = identity(DBGHELP)
            require(pe_identity(DBGHELP) == (0x8664, 0x20b), "installed DbgHelp is not x64")
            protected.append(dbghelp_identity)
            record["dbghelp"] = dbghelp_identity
        # Preserve the exact prior receipt and raw failure, not a cleaned summary.
        prior = output / preset["priorDirectory"]
        prior.mkdir()
        original_case = receipt_path.parent / preset["caseDirectory"]
        for path in (receipt_path, original_case / "host.log",
                     original_case / "case.stdout", original_case / "case.stderr"):
            shutil.copy2(path, prior / path.name)
        case = output / preset["caseDirectory"]
        case.mkdir()
        copied = []
        for row in inputs:
            source = Path(row["identity"]["path"])
            target = (output if row["role"].startswith("desktop-probe-") else case) / row["role"]
            shutil.copy2(source, target)
            after = identity(target)
            require(payload_equal(row["identity"], after), "copy changed binary")
            copied.append(dict(source=row["identity"], copy=after))
        record["copies"] = copied
        env = {key: value for key, value in os.environ.items() if key.upper() in {
            "SYSTEMROOT", "WINDIR", "COMSPEC", "PATH", "TEMP", "TMP", "USERPROFILE", "APPDATA", "LOCALAPPDATA",
            "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMDATA", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE"}}
        env.update(DXVK_LOG_LEVEL="info", DXVK_SHADER_CACHE="0")
        if args.implicit_layers_disabled:
            env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
            env["VK_LOADER_DEBUG"] = "error,warn,info,layer"
        if args.empty_implicit_layer_path:
            # The installed 1.4.341 loader supports process-local discovery
            # override. Disabling known layers alone still parses stale system
            # manifests. Keep all old errors; do not edit/repair registry entries.
            record["emptySourceBefore"] = create_empty_source(output)
            empty_layer_path = Path(record["emptySourceBefore"]["path"])
            env["VK_IMPLICIT_LAYER_PATH"] = str(empty_layer_path)
            if args.empty_explicit_layer_path:
                env["VK_LAYER_PATH"] = str(empty_layer_path)
            record["implicitLayerSource"] = dict(path=str(empty_layer_path), emptyBefore=True,
                globalRegistryChanged=False, overrideIsDiagnosticOnly=True)
        # Only nonsecret experimental keys are persisted, never the environment.
        record["comparisonEnvironment"] = {key: env[key] for key in (
            "VK_LOADER_LAYERS_DISABLE", "VK_LOADER_DEBUG", "VK_IMPLICIT_LAYER_PATH", "VK_LAYER_PATH") if key in env}
        with IsolatedRenderDesktop(restricted_medium=args.restricted_medium) as desktop:
            record["desktopSafety"] = desktop.receipt
            probe = desktop.run([output / "desktop-probe-32.exe", "--expect-desktop", desktop.desktop_name,
                "--expect-station", "WinSta0", "--input-desktop", desktop.input_before,
                "--child", output / "desktop-probe-64.exe", "--child-log", output / "desktop-child.log"],
                cwd=output, stdout_path=output / "probe.stdout", stderr_path=output / "probe.stderr", timeout_s=30, env=env)
            record["probe"] = probe
            require(probe["ok"] is True,
                    f"CPU desktop probe failed: exit={probe.get('exitCode')} timeout={probe.get('timedOut')}")
            probe_rows = complete_records(output / "probe.stdout") + complete_records(output / "desktop-child.log")
            record["probeReports"] = probe_rows
            require(len(probe_rows) == 2 and probe["ok"] is True, "CPU desktop probe incomplete")
            validate_probe_receipt(probe_rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station="WinSta0", bits=32, child=probe_rows[1], launch=probe)
            parent, dll, host = (case / preset["binaries"][role][0] for role in ("parent", "dll", "host"))
            local = env | dict(DXVK_LOG_PATH=str(case), DXVK_CONFIG_FILE=str(case / "absent.conf"),
                              WARVK_D3D9_PROFILE="presenter-readback-isolated-v1",
                              WARVK_D3D9_HOST_EXE=str(host), WARVK_D3D9_HOST_LOG=str(case / "host.log"))
            monitor = ExitMonitor(desktop, parent, host, case, dbghelp_identity, args.case, palette_image)
            monitor.start()
            try:
                argv = remap_preserved_argv(original_launch, preset, case, desktop.desktop_name, desktop.input_before)
                launch = desktop.run(argv, cwd=case, stdout_path=case / "case.stdout",
                                     stderr_path=case / "case.stderr", timeout_s=preset["launcherTimeoutSeconds"], env=local)
                record["launch"] = launch
            finally:
                monitor.stop_event.set()
                monitor.join(4 if args.capture_minidump else 2)
                require(not monitor.is_alive(), "read-only monitor has not settled; diagnostic incomplete")
            record["monitor"] = monitor.result
            rows = complete_records(case / "case.stdout")
            record["nativeReports"] = rows
            record["hostReports"] = complete_records(case / "host.log")
            fixtures = [row for row in rows if row.get("schema") == preset["fixtureSchema"]]
            require(len(fixtures) == 1, "selected native fixture receipt missing/duplicate")
            record["nativeFixtureOk"] = fixtures[0].get("ok") is True
            proxies = [row for row in rows if row.get("schema") == "warvk-d3d9-proxy/v1"]
            require(len(proxies) == 1, "final native child receipt missing/duplicate")
            proxy = proxies[0]
            binding = monitor.result.get("binding")
            if binding:
                require(proxy.get("childPid") == binding["host"]["pid"]
                        and proxy.get("childCreation") == binding["host"]["creation100ns"],
                        "final native original child HANDLE identity disagrees with query binding")
                monitor.result["identityCertified"] = True
            # This native field is observed elapsed time, NOT configured timeout.
            # The frozen proxy binary supplies the unchanged 5-second deadline.
            elapsed = proxy.get("primaryExitWaitMs")
            require(type(elapsed) is int and 0 <= elapsed <= 0xffffffffffffffff,
                    "native primary exit wait elapsed time is not an unsigned integer")
            record["nativePrimaryExitWaitObservedMs"] = elapsed
            native_clean = bool(launch["ok"] is True and proxy.get("transportFailed") is False
                                and proxy.get("childNaturalExit") is True and proxy.get("childProcessSettled") is True
                                and proxy.get("childExit") == 0 and elapsed <= 5000)
            record["nativeExitDeadlinePassedThisDiagnosticOnly"] = native_clean
            record["nativeFailurePreserved"] = not native_clean
            if args.implicit_layers_disabled:
                trace = (case / "host.log").read_text("utf-8", errors="replace")
                disabled = re.findall(r'Implicit layer "[^"\r\n]+" forced disabled because name matches filter', trace)
                inserted = re.findall(r'Insert(?:ed)? (?:instance|device) layer "([^"\r\n]+)"', trace)
                record["implicitLayerFilterEvidence"] = dict(disabledLines=len(disabled), insertedLayers=inserted,
                    applied=bool(disabled) and not inserted)
                if args.empty_implicit_layer_path:
                    empty_after = not list(empty_layer_path.iterdir())
                    path_witness = str(empty_layer_path).replace('\\', '/').casefold() in trace.replace('\\', '/').casefold()
                    registry_lookup = bool(re.search(r'(?:Checking for Layer Manifest files in Registry|Located json file).*ImplicitLayers', trace))
                    record["implicitLayerSource"].update(emptyAfter=empty_after,
                        actualLoaderPathWitness=path_witness, implicitRegistryLookupObserved=registry_lookup,
                        sourceOverrideObserved=empty_after and path_witness and not registry_lookup and not inserted)
                    require(record["implicitLayerSource"]["sourceOverrideObserved"],
                            "actual loader did not prove owned empty implicit discovery source")
                    if args.empty_explicit_layer_path:
                        record["strictLayerSourceEvidence"] = check_empty_source_trace(trace, empty_layer_path)
                else:
                    require(record["implicitLayerFilterEvidence"]["applied"],
                            "implicit-layer filtering was requested but actual loader trace does not prove it")
            record["diagnosticCompleted"] = not monitor.result["errors"]
    except Exception as exc:
        record["errors"].append(f"{type(exc).__name__}: {exc}")
    finally:
        if monitor and monitor.is_alive():
            monitor.stop_event.set()
            record["monitorStillActive"] = True
        elif monitor:
            record["monitor"] = monitor.result
        try:
            if "emptySourceBefore" in record:
                record["emptySourceAfter"] = empty_directory_witness(Path(record["emptySourceBefore"]["path"]))
                require(record["emptySourceBefore"] == record["emptySourceAfter"], "owned empty source changed")
            record["protectedAfter"] = [identity(Path(row["path"])) for row in protected]
            record["protectedPreserved"] = protected == record["protectedAfter"]
            record["originalReceiptPreserved"] = identity(receipt_path) == record["originalReceipt"]
            record["inputFilesPreserved"] = all(identity(Path(row["identity"]["path"])) == row["identity"] for row in inputs)
            record["copiesPreserved"] = all(identity(Path(row["copy"]["path"])) == row["copy"] for row in record.get("copies", []))
            record["rawEvidence"] = [identity(path) for path in sorted(output.rglob("*")) if path.is_file()
                                     and path.suffix.lower() not in (".exe", ".dll")]
        except Exception as exc:
            record["errors"].append("preservation: " + str(exc))
        record["diagnosticCompleted"] = bool(record["diagnosticCompleted"] and not record["errors"]
            and record.get("protectedPreserved") and record.get("originalReceiptPreserved")
            and record.get("inputFilesPreserved") and record.get("copiesPreserved")
            and record.get("desktopSafety", {}).get("desktopClosed") is True
            and not record.get("desktopSafety", {}).get("cleanupErrors"))
        write_json(output / "receipt.json", record)
    print(json.dumps(dict(receipt=str(output / "receipt.json"), diagnosticCompleted=record["diagnosticCompleted"],
                          nativeDeadlinePassed=native_clean, acceptancePassed=False), ensure_ascii=False))
    return 0 if record["diagnosticCompleted"] and native_clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
