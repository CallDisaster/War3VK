"""Phase 5.1 first wiring: run the real game on the x64 render host, in the sandbox.

This is a development probe, NOT a gate, an equivalence test or a release test.
A fresh sandbox instance gets the development D3D9 proxy as its d3d9.dll; the
proxy starts the x64 host and forwards every D3D9 call to it. The run happens on
an owned non-input desktop under a restricted Medium token and a Job; nothing is
written outside the fresh instance and the fresh output folder. No global input,
no registry writes, no foreground change, no player deployment.

What it records: the proxy's first-wiring trace (game.stderr: device creation,
unsupported D3D9 entries, frame dumps), the host log, x86 memory samples of the
game process, one low-address-space sample, and back-buffer dumps converted to
PNG. After --seconds the game's own window gets WM_CLOSE on its own desktop; in
a running map the game answers with its own quit confirmation, which the probe
accepts with the dialog's hotkey, posted to the same window. The game must then
exit by itself; a timeout ends in the launcher's Job cleanup and fails.

--actions adds timed acts on that window before the close (resize, minimize and
restore, WM_ACTIVATEAPP, a posted key). They go to the game's own HWND from a
worker bound to the game's desktop: never global input, never the input desktop.

The proxy admits a game process only with WARVK_D3D9_GAME=sandbox-isolated-v1
on a non-input desktop (tools/render_host/d3d9_window_identity.h).

--product-dll with --in-process runs the product alone, the device in the game
process: the reference arm for comparisons with proxy mode.

--product-dll runs the product x86 d3d9.dll in proxy mode instead (phase 5.1-b):
it becomes the instance's d3d9.dll, the development proxy is its front end
(WARVK_D3D9_FRONT), and the probe also talks to the data layer's control plane:
ping, runtime status, wait_until, one capture_final_frame.

--vanilla runs the game as it ships: no d3d9.dll in the instance, no host, no
WarVK environment, so the game uses Windows' own Direct3D 9. It is the
reference arm of the comparison with the original game
(run_v130_vanilla_compare.py), which counts frames from outside with Direct3D
9's own Present events. The probe records the wall-clock start of its run
(observer.beganUnixSeconds) so those events can be cut to the same window.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import threading
import time
import zlib

from autotest_sessions import (DEFAULT_SANDBOX_ROOT, build_instance_layout, deploy_content_addressed_map,
    materialize_instance_root, normalize_identifier, preflight_instance_pool)
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from run_native_d3d9_survey import (NativeObserver, bmp_dimensions, checked_path, identity, launch_identity,
    loadfile_argument, pe_bits, require, strict_json, within)
import native_game_exit_acceptance as exit_acceptance
import native_game_va_sample

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = ROOT / "AutoTest/artifacts"
PROBES = ARTIFACT_ROOT / "host-child-surface-cpu-20261023-r2"
PLAYER_DLL = Path("E:/Work/Warcraft III/d3d9.dll")
ENV_ALLOW = frozenset(("SYSTEMROOT", "WINDIR", "COMSPEC", "PATH", "PATHEXT", "USERNAME", "USERPROFILE", "APPDATA",
    "LOCALAPPDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMDATA", "ALLUSERSPROFILE",
    "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE"))
STREAM = dict(WARVK_D3D9_BATCH_DRAWS="1", WARVK_D3D9_ASYNC_BATCH="1", WARVK_D3D9_ASYNC_EVENTS="1", WARVK_D3D9_STREAM="1")
# Transport profiles, by the names of the GPU reference gate's arms.
PROFILES = {
    "sync": {},
    "stream-fixed": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1"),
    "stream-fixed-cmdring": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1"),
    # The product x86 memory profile on top (d3d9_upload_window.h).
    "product": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1",
                    WARVK_D3D9_X86_BACKING_KIB="32768", WARVK_D3D9_UPLOAD_WINDOW_KIB="1024"),
    # The same with the four ordered asynchronous switches (2026-11-18 P2): texture uploads, EndScene's
    # flush, scene records and Present leave the x86 thread without waiting for the host. Kept as its
    # own profile until a freeze round has passed with it; then it becomes "product".
    "product-async": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1",
                          WARVK_D3D9_X86_BACKING_KIB="32768", WARVK_D3D9_UPLOAD_WINDOW_KIB="1024",
                          WARVK_D3D9_ASYNC_UPLOADS="1", WARVK_D3D9_ASYNC_FLUSH="1",
                          WARVK_D3D9_ASYNC_SCENE="1", WARVK_D3D9_PRESENT_AHEAD="1"),
    # The same again with P4 (2026-11-18): fixed-function batches leave x86 as word deltas inside one
    # message. Frame rate neutral where the transport is no longer the limit; it roughly halves the
    # ring traffic. Its own profile until the author picks the trial configuration.
    "product-async-delta": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1",
                                WARVK_D3D9_X86_BACKING_KIB="32768", WARVK_D3D9_UPLOAD_WINDOW_KIB="1024",
                                WARVK_D3D9_ASYNC_UPLOADS="1", WARVK_D3D9_ASYNC_FLUSH="1",
                                WARVK_D3D9_ASYNC_SCENE="1", WARVK_D3D9_PRESENT_AHEAD="1",
                                WARVK_D3D9_FIXED_DELTA="1"),
    # The same with the final Release of a resource and CreateTexture2D published in order instead of
    # waited for (2026-11-18, f8). Their effect is on loading and map changes, not on steady frames.
    "product-async-resources": dict(STREAM, WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1",
                                    WARVK_D3D9_X86_BACKING_KIB="32768", WARVK_D3D9_UPLOAD_WINDOW_KIB="1024",
                                    WARVK_D3D9_ASYNC_UPLOADS="1", WARVK_D3D9_ASYNC_FLUSH="1",
                                    WARVK_D3D9_ASYNC_SCENE="1", WARVK_D3D9_PRESENT_AHEAD="1",
                                    WARVK_D3D9_FIXED_DELTA="1", WARVK_D3D9_ASYNC_DESTROY="1",
                                    WARVK_D3D9_ASYNC_CREATE_TEXTURE="1"),
}


USER32 = ctypes.WinDLL("user32", use_last_error=True)
USER32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
USER32.PostMessageW.restype = wt.BOOL
USER32.MapVirtualKeyW.argtypes = (wt.UINT, wt.UINT)
USER32.MapVirtualKeyW.restype = wt.UINT
USER32.GetWindowRect.argtypes = USER32.GetClientRect.argtypes = (wt.HWND, ctypes.POINTER(wt.RECT))
USER32.GetWindowRect.restype = USER32.GetClientRect.restype = wt.BOOL
USER32.SetWindowPos.argtypes = (wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT)
USER32.SetWindowPos.restype = wt.BOOL
USER32.ShowWindowAsync.argtypes = (wt.HWND, ctypes.c_int)
USER32.ShowWindowAsync.restype = wt.BOOL
USER32.GetClassNameW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
USER32.InternalGetWindowText.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
USER32.FindWindowExW.argtypes = (wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR)
USER32.FindWindowExW.restype = wt.HWND
KEYS = {"PAUSE": 0x13, "F10": 0x79, "ESC": 0x1B, "ENTER": 0x0D, "SPACE": 0x20}


def window_names(hwnd):
    """Class and title of a window, read without sending it a message."""
    names = {}
    for key, call in (("windowClass", USER32.GetClassNameW), ("windowText", USER32.InternalGetWindowText)):
        text = ctypes.create_unicode_buffer(256)
        names[key] = text.value if call(hwnd, text, 256) else None
    if names["windowClass"] == "#32770":
        # A message box: its text is in the child controls.
        texts = []
        child = USER32.FindWindowExW(hwnd, None, None, None)
        while child and len(texts) < 16:
            text = ctypes.create_unicode_buffer(512)
            if USER32.InternalGetWindowText(child, text, 512):
                texts.append(text.value)
            child = USER32.FindWindowExW(hwnd, child, None, None)
        names["dialogTexts"] = texts
    return names
SHOW = {"minimize": 7, "restore": 9}  # SW_SHOWMINNOACTIVE, SW_RESTORE


def window_act(action):
    """One act on the game's own HWND; raises OSError on a refused call."""
    name, _, value = action.partition("=")

    def post(hwnd, message, wparam, lparam):
        if not USER32.PostMessageW(hwnd, message, wparam, lparam):
            raise ctypes.WinError(ctypes.get_last_error())

    def act(api, hwnd):
        if name == "close":
            post(hwnd, 0x0010, 0, 0)
        elif name == "key":
            key = KEYS[value] if value in KEYS else ord(value)
            scan = USER32.MapVirtualKeyW(key, 0)
            post(hwnd, 0x0100, key, 1 | scan << 16)
            if value in ("SPACE", "ENTER"):
                post(hwnd, 0x0102, key, 1 | scan << 16)  # WM_CHAR, as TranslateMessage would add
            post(hwnd, 0x0101, key, 1 | scan << 16 | 0xC0000000)
        elif name == "click":
            x, y = (int(v) for v in value.split("x"))
            where = y << 16 | x
            post(hwnd, 0x0200, 0, where)
            post(hwnd, 0x0201, 1, where)
            post(hwnd, 0x0202, 0, where)
        elif name == "activate":
            post(hwnd, 0x001C, int(value), 0)
        elif name == "show":
            if not USER32.ShowWindowAsync(hwnd, SHOW[value]):
                pass  # the return value is the previous visibility, not an error
        elif name == "size":
            width, height = (int(v) for v in value.split("x"))
            outer, client = wt.RECT(), wt.RECT()
            if not USER32.GetWindowRect(hwnd, ctypes.byref(outer)) or not USER32.GetClientRect(hwnd, ctypes.byref(client)):
                raise ctypes.WinError(ctypes.get_last_error())
            frame_w = outer.right - outer.left - client.right
            frame_h = outer.bottom - outer.top - client.bottom
            # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
            if not USER32.SetWindowPos(hwnd, None, 0, 0, width + frame_w, height + frame_h, 0x0002 | 0x0004 | 0x0010 | 0x4000):
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            raise ValueError(f"unknown action {action}")
    return act


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


def own_host_pids(game_pid):
    """Process ids of the game's children named host.exe (this run's host copies); reads only."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
    kernel.CreateToolhelp32Snapshot.argtypes = (wt.DWORD, wt.DWORD)
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = (wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    kernel.CloseHandle.argtypes = (wt.HANDLE,)
    snapshot = kernel.CreateToolhelp32Snapshot(0x2, 0)
    require(snapshot and snapshot != wt.HANDLE(-1).value, "process snapshot")
    children = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32ParentProcessID == game_pid and entry.szExeFile.lower() == "host.exe":
                children.append(int(entry.th32ProcessID))
            more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return children


def kill_own_host(game_pid, host_path):
    """Terminate the probe's own host: the game's child whose image is this run's host.exe.

    Nothing else is ever terminated: the parent PID and the full image path both
    have to match the fresh output folder's copy.
    """
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
    kernel.CreateToolhelp32Snapshot.argtypes = (wt.DWORD, wt.DWORD)
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = (wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    kernel.OpenProcess.restype = wt.HANDLE
    kernel.OpenProcess.argtypes = (wt.DWORD, wt.BOOL, wt.DWORD)
    kernel.QueryFullProcessImageNameW.argtypes = (wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD))
    kernel.TerminateProcess.argtypes = (wt.HANDLE, wt.UINT)
    kernel.CloseHandle.argtypes = (wt.HANDLE,)
    snapshot = kernel.CreateToolhelp32Snapshot(0x2, 0)
    require(snapshot and snapshot != wt.HANDLE(-1).value, "process snapshot")
    children = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32ParentProcessID == game_pid and entry.szExeFile.lower() == "host.exe":
                children.append(entry.th32ProcessID)
            more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    result = dict(posted=False, candidates=len(children))
    require(len(children) == 1, f"exactly one own host expected, found {len(children)}")
    handle = kernel.OpenProcess(0x0001 | 0x1000, False, children[0])  # TERMINATE | QUERY_LIMITED_INFORMATION
    require(handle, "open own host")
    try:
        buffer, size = ctypes.create_unicode_buffer(1024), wt.DWORD(1024)
        require(kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)), "own host image")
        require(os.path.normcase(os.path.abspath(buffer.value)) == os.path.normcase(os.path.abspath(str(host_path))),
                "child image is not this run's host.exe")
        require(kernel.TerminateProcess(handle, 99), "terminate own host")
        result.update(posted=True, hostPid=children[0], image=buffer.value, exitCodeGiven=99)
    finally:
        kernel.CloseHandle(handle)
    return result


VA_REGIONS_END = 1 << 32  # the whole address space of a large-address-aware WOW64 process


def va_holes(holes, begin, end):
    """Free-space shape of [begin, end).

    freeBytes is every MEM_FREE byte in the range, as native_game_va_sample
    counts it; usableFreeBytes is what VirtualAlloc can hand out (each hole's
    start rounded up to 64 KiB). The largest hole is aligned the same way.
    """
    clipped, free = [], 0
    for start, stop in holes:
        start, stop = max(start, begin), min(stop, end)
        free += max(0, stop - start)
        aligned = (start + 0xFFFF) & ~0xFFFF
        if stop > aligned:
            clipped.append((stop - aligned, aligned))
    clipped.sort(reverse=True)
    return dict(rangeBegin=begin, rangeEnd=end, freeBytes=free,
                usableFreeBytes=sum(size for size, _ in clipped), holes=len(clipped),
                largestAlignedFreeHoleBytes=clipped[0][0] if clipped else 0,
                largestAlignedFreeHoleStart=clipped[0][1] if clipped else None,
                largestHoles=[[hex(start), size] for size, start in clipped[:5]])


def va_regions(native, output, index):
    """Development aid (va=regions): the whole 4 GiB of the game process by allocation.

    VirtualQueryEx names regions, not owners; grouping them by AllocationBase
    gives each VirtualAlloc / mapping / module as one row. The rows go to
    va-regions-<index>.json, largest committed first. War3.exe is large-address
    aware, so the half above 2 GiB is the game's too; Storm itself only uses the
    low half (signed compares), so every total that existed before the scan
    reached 4 GiB still counts the low 2 GiB only, and the high half has its
    own fields. A row belongs to the half its allocation base is in.
    """
    sampler = native_game_va_sample
    api = sampler._WindowsVaApi()
    names = {sampler.MEM_PRIVATE: "private", sampler.MEM_MAPPED: "mapped", sampler.MEM_IMAGE: "image"}
    cursor, allocations, spans, holes = 0x10000, {}, {}, []
    while cursor < VA_REGIONS_END:
        row = api.query(native.process, cursor)
        end = min(VA_REGIONS_END, row["base"] + row["size"])
        size = end - cursor
        if row["state"] == sampler.MEM_FREE:
            if holes and holes[-1][1] == cursor:
                holes[-1][1] = end
            else:
                holes.append([cursor, end])
        else:
            base = int(api._info.AllocationBase or 0)
            entry = allocations.setdefault(base, dict(base=base, type=names.get(row["type"], "?"), bytes=0,
                                                      committed=0, regions=0))
            entry["bytes"] += size
            entry["regions"] += 1
            if row["state"] == sampler.MEM_COMMIT:
                entry["committed"] += size
                # Guard and no-access pages cannot be read; they are not contents either.
                if not int(api._info.Protect) & 0x101:
                    spans.setdefault(base, []).append((cursor, size))
        cursor = end
    rows = sorted(allocations.values(), key=lambda entry: -entry["committed"])
    # What the large private allocations hold: how many of their pages were ever
    # written, and the first bytes and text found in them. Read only.
    read = ctypes.windll.kernel32.ReadProcessMemory
    read.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                     ctypes.POINTER(ctypes.c_size_t)]
    read.restype = wt.BOOL
    zero_page, sampled = bytes(4096), 0
    for entry in rows:
        if entry["type"] != "private" or entry["committed"] < (1 << 20) or sampled >= 160:
            continue
        sampled += 1
        pages = written = unread = 0
        head, texts, runs = "", [], []
        for begin, size in spans.get(entry["base"], []):
            for offset in range(0, size, 1 << 20):
                count = min(1 << 20, size - offset)
                buffer, got = ctypes.create_string_buffer(count), ctypes.c_size_t()
                if not read(native.process, begin + offset, buffer, count, ctypes.byref(got)) or got.value != count:
                    unread += count // 4096
                    continue
                data = buffer.raw
                for page in range(0, count, 4096):
                    pages += 1
                    chunk = data[page:page + 4096]
                    if chunk != zero_page:
                        written += 1
                        if not head:
                            head = chunk[:96].hex()
                        # Runs of written pages: [first page, pages, first bytes of the run].
                        if runs and runs[-1][0] + runs[-1][1] == pages - 1:
                            runs[-1][1] += 1
                        elif len(runs) < 48:
                            runs.append([pages - 1, 1, chunk[:64].hex()])
                if len(texts) < 8:
                    texts += [m.group().decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{8,64}", data)][:8 - len(texts)]
        entry.update(pages=pages, writtenPages=written, unreadPages=unread, head=head, texts=texts, runs=runs)
    path = within(output / f"va-regions-{index}.json", output)
    path.write_text(json.dumps(rows), encoding="utf-8")
    low_end = sampler.LOW_END
    private = [entry for entry in rows if entry["type"] == "private" and entry["base"] < low_end]

    def half(begin, end):
        inside = [entry for entry in rows if begin <= entry["base"] < end]
        totals = {kind: dict(bytes=sum(e["bytes"] for e in inside if e["type"] == kind),
                             committed=sum(e["committed"] for e in inside if e["type"] == kind))
                  for kind in ("private", "mapped", "image")}
        return dict(va_holes(holes, begin, end), allocations=len(inside), types=totals)

    return dict(posted=True, file=path.name, allocations=len(rows),
                privateCommittedBytes=sum(entry["committed"] for entry in private),
                largestPrivate=[[hex(entry["base"]), entry["committed"], entry["bytes"]] for entry in private[:12]],
                scanBegin=0x10000, scanEnd=VA_REGIONS_END,
                low=half(0x10000, low_end), high=half(low_end, VA_REGIONS_END),
                highPrivate=[[hex(entry["base"]), entry["committed"], entry["bytes"]] for entry in rows
                             if entry["type"] == "private" and entry["base"] >= low_end][:12])


# The product's answer to a lost render host (war3_proxy_host_lost.h).
HOST_LOST_EXIT = 0xE0574B01
NOTICE_TITLE = "WarVK\uff1a\u6e32\u67d3\u8fdb\u7a0b\u5df2\u505c\u6b62"


def dismiss_notice(native, expected, input_name):
    """WM_CLOSE to the game's own host-lost dialog on the owned Desktop.

    The dialog is found by owner PID and title among the Desktop's top-level
    windows; the worker thread binds to that Desktop only for the call.
    """
    api = exit_acceptance._WindowsExitApi(native)
    result = dict(posted=False)

    def work():
        handle, previous, bound = 0, 0, False
        try:
            api.guard_desktop(expected["desktop"], input_name)
            previous = api.thread_desktop()
            handle = api.open_desktop(expected["desktop"])
            require(api.name(handle) == expected["desktop"], "opened Desktop identity mismatch")
            api.bind_desktop(handle)
            bound = True
            rows = []

            def visit(hwnd, unused):
                try:
                    if api.window_owner(hwnd)[0] == expected["pid"]:
                        rows.append((hwnd, api.window_title(hwnd)))
                except Exception:
                    pass
                return True

            callback = api.callback_type(visit)
            api.check(api.u.EnumDesktopWindows(handle, callback, 0), "enumerate owned Desktop")
            result["ownedWindowTitles"] = [title for _, title in rows]
            dialogs = [hwnd for hwnd, title in rows if title == NOTICE_TITLE]
            require(len(dialogs) == 1, "exactly one host-lost dialog expected")
            api.check(api.u.PostMessageW(dialogs[0], 0x0010, 0, 0), "dialog WM_CLOSE")
            result.update(posted=True, dialogTitle=NOTICE_TITLE)
        except Exception as error:
            result["error"] = f"{type(error).__name__}: {error}"
        finally:
            try:
                if bound:
                    api.bind_desktop(previous)
                if handle:
                    api.close_desktop(handle)
            except Exception as error:
                result["cleanupError"] = f"{type(error).__name__}: {error}"

    worker = threading.Thread(target=work, name="WarVK-Notice-Dismiss", daemon=True)
    worker.start()
    worker.join(5.0)
    result["workerPending"] = worker.is_alive()
    return result


# Control-plane acts of the product (development probe only; the gates keep
# the survey runner's own allowlist):
#   api=<command;args>  the public author API through its native carrier,
#                       e.g. api=volumetric.setEnabled;b:1
#   make=<command;args> the same for a command that returns a handle (its
#                       carrier is GetLocalizedHotkey), e.g.
#                       make=weather.createSnow;i:0. "@h" in a later api= or
#                       make= stands for the handle the last make= returned.
#   test=<command>      an internal test command with an empty payload
#   test=<command>:k=v;k=v  the same with a payload; each v is JSON (a number,
#                       true/false, "text") or else text, a|b|c a list of text,
#                       e.g. test=outline.pick:kind="building";showVisible=true
#   capture=<name>      capture_final_frame into <name>.bmp
#   camrel=<dx>;<dy>    camera.apply at the camera target the first camrel=
#                       found (camera.snapshot) moved by dx, dy world units
#   status=<name>       get_runtime_status into status-<name>.json
PLAYER_SETTINGS_SECTION = "PlayerSettings"
PLANE_ACTS = ("api", "make", "test", "capture", "camrel", "status")
LAST_HANDLE = [0]
CAMERA_ORIGIN = []
TEST_COMMANDS = ("game.end_for_exit_test", "game.restart_for_map_test", "visual.snapshot", "weather.status",
                 "visibility.full_map", "camera.snapshot", "game.pause",
                 # T4 probes (outline, off-screen shadows): fixed camera, low sun,
                 # one object registered for the outline.
                 "camera.apply", "camera.fixed", "camera.world_bounds", "shadow.lock_sun",
                 "outline.pick", "outline.clear", "outline.status",
                 # The settings panel in proxy mode (x64 overlay).
                 "ui.panel_open", "ui.panel_close", "ui.panel_status")


def test_payload(text):
    """k=v;k=v of test=<command>:<payload> to a JSON object."""
    payload = {}
    for item in filter(None, text.split(";")):
        key, sep, value = item.partition("=")
        require(sep and key.isidentifier(), f"test payload {item}")
        if "|" in value:
            payload[key] = [part for part in value.split("|") if part]
            continue
        try:
            payload[key] = json.loads(value)
        except ValueError:
            payload[key] = value
    return payload


def plane_act(native, output, action):
    """One control-plane act. Returns the receipt row; never raises."""
    import run_native_d3d9_survey as survey
    name, _, value = action.partition("=")
    try:
        if name == "status":
            reply = native.request("get_runtime_status", {}, timeout=3.0)["response"]
            result = reply.get("result") or {}
            path = within(output / f"status-{value}.json", output)
            path.write_text(json.dumps(result, indent=1), encoding="utf-8")
            proxy = result.get("proxyMode") or {}
            return dict(posted=reply["ok"] is True, error=reply.get("error"), file=path.name,
                        frameIndex=result.get("frameIndex"), proxyFrame=proxy.get("frame"),
                        casters={k: (proxy.get("casters") or {}).get(k) for k in (
                            "sent", "units", "withHandle", "unitsWithHandle", "outlineMatched", "lastFrame")})
        if name == "camrel":
            if not CAMERA_ORIGIN:
                found = plane_act(native, output, "test=camera.snapshot")
                snapshot = found.get("result") or {}
                require(found.get("posted") and isinstance(snapshot.get("targetX"), (int, float)),
                        "camera snapshot for camrel")
                CAMERA_ORIGIN[:] = [float(snapshot["targetX"]), float(snapshot["targetY"])]
            dx, dy = (float(v) for v in value.split(";"))
            done = plane_act(native, output, "test=camera.apply:targetX=%r;targetY=%r;duration=0;quickPosition=true"
                             % (CAMERA_ORIGIN[0] + dx, CAMERA_ORIGIN[1] + dy))
            done["origin"] = list(CAMERA_ORIGIN)
            return done
        if name == "capture":
            picture = within(output / (value + ".bmp"), output)
            reply = native.request("capture_final_frame", dict(outputPath=str(picture), timeoutMs=2500),
                                   timeout=5.0)["response"]
            done = dict(posted=reply["ok"] is True, error=reply.get("error"))
            if reply["ok"] and picture.is_file():
                done.update(file=identity(picture), size=bmp_dimensions(picture))
            return done
        if name in ("api", "make"):
            value = value.replace("@h", str(LAST_HANDLE[0]))
            command, payload = "jass.public_api", dict(
                carrier="Preloader" if name == "api" else "GetLocalizedHotkey", payload="warvk:v1;" + value)
        else:
            command, _, text = value.partition(":")
            payload = test_payload(text)
        allowed = survey.exit_request_allowed
        survey.exit_request_allowed = lambda enabled, c, p: c == "invoke_test_command"
        try:
            reply = native.request("invoke_test_command", dict(command=command, payload=payload, timeoutMs=3000),
                                   timeout=5.0)["response"]
        finally:
            survey.exit_request_allowed = allowed
        result = reply.get("result")
        if name == "make" and isinstance(result, dict) and isinstance(result.get("integer"), int):
            LAST_HANDLE[0] = result["integer"]
        return dict(posted=reply["ok"] is True, error=reply.get("error"), result=result)
    except Exception as error:
        return dict(posted=False, error=f"{type(error).__name__}: {error}"[:300])


def parse_actions(text, seconds):
    """'T:act,...' with act in key=K | activate=0|1 | show=minimize|restore | size=WxH | click=XxY | host=kill
    | notice=dismiss | api=... | test=... | capture=NAME."""
    rows = []
    for item in filter(None, (text or "").split(",")):
        at, _, action = item.partition(":")
        name, _, value = action.partition("=")
        require(name in ("key", "activate", "show", "size", "click", "host", "notice", "va") + PLANE_ACTS and value,
                f"action {item}")
        require(name != "va" or value == "regions", f"va {item}")
        require(name != "notice" or value == "dismiss", f"notice {item}")
        require(name != "test" or value.partition(":")[0] in TEST_COMMANDS, f"test {item}")
        if name == "test":
            test_payload(value.partition(":")[2])
        if name == "camrel":
            require(len(value.split(";")) == 2 and all(abs(float(v)) <= 20000 for v in value.split(";")),
                    f"camrel {item}")
        require(name != "status" or (value.isalnum() and len(value) <= 24), f"status {item}")
        require(name != "capture" or (value.isalnum() and len(value) <= 24), f"capture {item}")
        require(name != "host" or value == "kill", f"host {item}")
        require(name != "key" or value in KEYS or (len(value) == 1 and value.isalnum() and value == value.upper()), f"key {item}")
        require(name != "activate" or value in ("0", "1"), f"activate {item}")
        require(name != "show" or value in SHOW, f"show {item}")
        if name in ("size", "click"):
            parts = value.split("x")
            low = 200 if name == "size" else 0
            require(len(parts) == 2 and all(v.isdigit() and low <= int(v) <= 4096 for v in parts), f"extent {item}")
        require(5 <= float(at) < seconds, f"action time {item}")
        rows.append((float(at), action))
    # The owned close, then the game's own quit confirmation ("X").
    return sorted(rows) + [(seconds, "close"), (seconds + 3, "key=X")]


class WindowApi(exit_acceptance._WindowsExitApi):
    """The exit API's verified owned-HWND path with another act in WM_CLOSE's place."""

    def __init__(self, native, act):
        super().__init__(native)
        self._act = act

    def post_close(self, hwnd):
        self._act(self, hwnd)


def guarded_identity(path):
    """An absent player DLL must stay absent; it is never created here."""
    return identity(path) if Path(path).exists() else {"path": str(path), "exists": False}


def write_png(path, width, height, bgra, scale):
    """BGRA rows to an opaque RGB PNG, subsampled by `scale` (viewing only)."""
    w, h = width // scale, height // scale
    rows = bytearray()
    for y in range(h):
        row = bgra[y * scale * width * 4:(y * scale * width + width) * 4]
        rows.append(0)
        pixels = bytearray(w * 3)
        pixels[0::3] = row[2::4 * scale][:w]
        pixels[1::3] = row[1::4 * scale][:w]
        pixels[2::3] = row[0::4 * scale][:w]
        rows += pixels

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def frame_statistics(bgra):
    """How much of the image is not one flat colour: a blank frame is visible in the receipt."""
    sample = bgra[0::4 * 97]
    distinct = len(set(sample))
    return {"sampledBlueValues": distinct, "sum": sum(sample), "samples": len(sample)}


def control_plane_step(native, receipt, output, elapsed, pace_end=None):
    """One round of the product's control plane (product mode only). Never raises."""
    plane = receipt["controlPlane"]

    def request(command, payload, timeout=3.0):
        row = native.request(command, payload, timeout=timeout)
        plane["requests"] += 1
        return row["response"]

    try:
        if not plane["pinged"]:
            plane["pinged"] = request("ping", {})["ok"] is True
            plane["pingedAtSeconds"] = round(elapsed, 1)
        status = request("get_runtime_status", {})
        if status["ok"]:
            result = status["result"]
            plane["lastStatus"] = {"runtime": result.get("runtime"), "render": result.get("render")}
            # The product's own clock and frame counter: the frame pace of either arm.
            plane["frameSeries"].append([round(elapsed, 2), result.get("timestampMs"), result.get("frameIndex"),
                                         bool((result.get("render") or {}).get("isInGame"))])
            proxy = result.get("proxyMode")
            if proxy is not None:
                plane["proxyMode"] = proxy
            # The product's own frame-time histogram (cumulative). Two snapshots are kept: the first
            # one taken 12 s after the game was ready, and the last one before this probe's own
            # captures begin; the pace of the run is their difference.
            pace = result.get("framePace")
            if pace is not None and plane.get("readyAtSeconds") is not None:
                if plane["framePaceSettled"] is None and elapsed >= plane["readyAtSeconds"] + 12.0:
                    plane["framePaceSettled"] = dict(atSeconds=round(elapsed, 2), **pace)
                elif plane["framePaceSettled"] is not None and (pace_end is None or elapsed <= pace_end):
                    plane["framePaceEnd"] = dict(atSeconds=round(elapsed, 2), **pace)
        if not plane["ready"]:
            reply = request("wait_until", dict(timeoutSec=1.0, pollIntervalMs=50, requireFrameAdvance=True,
                minFrameAdvance=2, requestSemanticFrameBuild=False, refreshSemanticFrameIfStale=False))
            plane["ready"] = reply["ok"] is True and reply["result"].get("ready") is True
            plane["lastWait"] = {k: reply["result"].get(k) for k in ("ready", "reason", "frameAdvance", "blockers")
                                 if k in reply.get("result", {})}
            if plane["ready"]:
                plane["readyAtSeconds"] = round(elapsed, 1)
        elif plane["capture"] is None and elapsed >= plane["readyAtSeconds"] + 6:
            picture = within(output / "game-frame.bmp", output)
            reply = request("capture_final_frame", dict(outputPath=str(picture), timeoutMs=2500), timeout=5.0)
            result = reply["result"]
            plane["capture"] = dict(ok=reply["ok"], error=reply.get("error"), width=result.get("width"),
                                    height=result.get("height"), outputPath=result.get("outputPath"))
            if reply["ok"] and picture.is_file():
                plane["capture"]["file"] = identity(picture)
                plane["captureVerified"] = bmp_dimensions(picture) == [result.get("width"), result.get("height")]
    except Exception as error:
        plane["errors"].append(f"{round(elapsed, 1)}s {type(error).__name__}: {error}"[:300])
        del plane["errors"][:-12]


def observe(desktop, launch_index, executable, output, stop, receipt, seconds, exit_wait, actions, product):
    native = NativeObserver()
    receipt.update(samples=[], errors=[], closed=None, exit=None, modules=None, vaSample=None, actions=[])
    if product:
        receipt["controlPlane"] = dict(pinged=False, ready=False, readyAtSeconds=None, capture=None,
            captureVerified=False, requests=0, errors=[], lastStatus=None, lastWait=None, proxyMode=None,
            frameSeries=[], framePaceSettled=None, framePaceEnd=None)
    try:
        expected = None
        while not stop.wait(0.05):
            launches = desktop.receipt["launches"]
            if len(launches) <= launch_index:
                continue
            try:
                expected = launch_identity(launches[launch_index], executable)
                break
            except ValueError:
                continue
        require(expected is not None, "game never published a verified launch identity")
        receipt["processIdentity"] = expected
        native.bind(expected)
        began = time.monotonic()
        receipt["beganUnixSeconds"] = time.time()
        modules_at = min(20.0, seconds / 2)
        pending = list(actions)
        # The frame-pace window ends a second before the probe's first capture or scan (they stall frames).
        pace_end = min((at for at, action in actions if action.startswith(("capture=", "va="))), default=seconds) - 1.0
        next_sample = 0.0
        while not stop.wait(0.25):
            elapsed = time.monotonic() - began
            if elapsed >= next_sample:
                next_sample = elapsed + 2.0
                try:
                    row = native.memory()
                except Exception as error:  # the process ended on its own
                    if not receipt.get("hostLost"):
                        receipt["errors"].append(f"memory: {type(error).__name__}: {error}")
                    break
                row["seconds"] = round(elapsed, 1)
                receipt["samples"].append(row)
                if product and len(pending) >= 2 and not receipt.get("hostLost"):
                    control_plane_step(native, receipt, output, elapsed, pace_end)
            if receipt["modules"] is None and elapsed >= modules_at:
                rows = native.modules()
                names = {r["name"].lower(): r["path"] for r in rows}
                receipt["modules"] = {
                    "count": len(rows), "d3d9": names.get("d3d9.dll"), "gameDll": names.get("game.dll"),
                    "front": names.get("front.dll"),
                    "vulkanLoaded": "vulkan-1.dll" in names,
                    "gpuDriverModules": sorted(n for n in names if n.startswith(("nvoglv", "nvwgf", "nvd3d", "atig", "amdvlk", "igvk", "vk_"))),
                    "names": sorted(names)}
                try:
                    # Outside frame counters (ETW present events) are per process: the host presents in proxy mode.
                    receipt["hostPids"] = own_host_pids(expected["pid"])
                except Exception as error:
                    receipt["errors"].append(f"host pids: {type(error).__name__}: {error}")
            while pending and elapsed >= pending[0][0]:
                action = pending.pop(0)[1]
                if action == "close":
                    receipt["vaSample"] = native_game_va_sample.sample(native)
                if action == "host=kill":
                    try:
                        done = kill_own_host(expected["pid"], output / "host.exe")
                    except Exception as error:
                        done = dict(posted=False, error=f"{type(error).__name__}: {error}")
                    if product and done.get("posted"):
                        # The product ends its own process now (war3_proxy_host_lost.h): the
                        # window acts that were still to come have no window to go to.
                        receipt["hostLost"] = True
                        pending = [row for row in pending if row[1] == "notice=dismiss"]
                elif action == "notice=dismiss":
                    done = dismiss_notice(native, expected, desktop.input_before)
                elif action == "va=regions":
                    try:
                        done = va_regions(native, output, len(receipt["actions"]))
                    except Exception as error:
                        done = dict(posted=False, error=f"{type(error).__name__}: {error}")
                elif action.partition("=")[0] in PLANE_ACTS:
                    done = plane_act(native, output, action)
                else:
                    done = WindowApi(native, window_act(action)).close_window(native, expected, desktop.input_before)
                    if done.get("hwnd"):
                        done.update(window_names(done["hwnd"]))
                receipt["actions"].append(dict(seconds=round(elapsed, 1), action=action, **done))
                if action == "close":
                    receipt["closed"] = done
            if not pending:
                receipt["exit"] = exit_acceptance._WindowsExitApi(native).wait_exit(native.process, exit_wait)
                break
        if receipt["exit"] is None and receipt.get("hostLost"):
            receipt["exit"] = exit_acceptance._WindowsExitApi(native).wait_exit(native.process, exit_wait)
    except Exception as error:
        receipt["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        receipt["cleanupErrors"] = native.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--proxy", type=Path, help="x86 development proxy DLL (d3d9_proxy.cpp); required unless --vanilla")
    parser.add_argument("--host", type=Path, help="x64 warvk-d3d9-host-dev.exe; required unless --vanilla")
    parser.add_argument("--vanilla", action="store_true",
                        help="no d3d9.dll in the instance, no host and no WarVK environment: the game on Windows' "
                             "own Direct3D 9, as it ships")
    parser.add_argument("--opengl", action="store_true",
                        help="with --vanilla: start the game with -opengl, its own OpenGL renderer. The game's "
                             "Direct3D cannot start on an isolated desktop (it reports that it could not "
                             "initialize DirectX); OpenGL can")
    parser.add_argument("--product-dll", type=Path,
                        help="x86 product d3d9.dll: run it in proxy mode with --proxy as its front end")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--instance-id", required=True)
    parser.add_argument("--map", type=Path, default=DEFAULT_SANDBOX_ROOT / "Maps/(4)LostTemple.w3m")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="stream-fixed-cmdring")
    parser.add_argument("--seconds", type=float, default=60, help="run time before the owned WM_CLOSE")
    parser.add_argument("--actions", default="", metavar="T:ACT,...",
                        help="timed acts on the game's own window before the close: key=K, activate=0|1, "
                             "show=minimize|restore, size=WxH, click=XxY (client pixels)")
    parser.add_argument("--timeout-seconds", type=float, default=120)
    parser.add_argument("--dump-every", type=int, default=300, help="back buffer dump interval in presented frames")
    parser.add_argument("--png-scale", type=int, default=2)
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="extra WARVK_*/DXVK_* variable for the game and the host")
    parser.add_argument("--in-process", action="store_true",
                        help="with --product-dll: no front end and no host, the device lives in the game process "
                             "(the reference arm of the proxy-mode comparisons)")
    parser.add_argument("--stock-exe", action="store_true",
                        help="clear IMAGE_FILE_LARGE_ADDRESS_AWARE in the INSTANCE's own copy of war3.exe: the game as "
                             "shipped, with 2 GiB of address space (the sandbox's file is not touched)")
    parser.add_argument("--fullscreen", action="store_true",
                        help="start the game without -window: it asks for a full-screen device")
    parser.add_argument("--player-setting", action="append", default=[], metavar="KEY=VALUE",
                        help="a line of the instance's WarVK/settings.ini (the player's saved settings), "
                             "e.g. SSAO.Enabled=1")
    parser.add_argument("--actions-file", type=Path,
                        help="more T:ACT entries, one per line (# starts a comment), after --actions")
    parser.add_argument("--extra-map", action="append", default=[], metavar="SRC=REL",
                        help="another map copied into the instance at REL (under Maps/), e.g. the targets "
                             "of a map's own ChangeLevel")
    args = parser.parse_args(argv)
    if args.actions_file:
        lines = [line.split("#", 1)[0].strip() for line in args.actions_file.read_text("utf-8").splitlines()]
        args.actions = ",".join(filter(None, [args.actions, *lines]))
    receipt = dict(schema="warvk-x64-game-proxy-probe/v1", scope="SANDBOX_GAME_ON_X64_HOST_DEVELOPMENT_PROBE", ok=False,
                   profile=args.profile, seconds=args.seconds, globalInputUsed=False, registryWrittenByRunner=False,
                   foregroundChanged=False, playerDllDeployed=False, checks={}, errors=[])
    output = None
    native = None
    protected = []
    try:
        output = within(args.output, ARTIFACT_ROOT)
        require(not output.exists(), "output must be fresh")
        require(args.instance_id == normalize_identifier(args.instance_id, "probe"), "explicit instance id required")
        require(10 <= args.seconds <= 240 and args.seconds + 20 <= args.timeout_seconds <= 300, "seconds / timeout range")
        require(1 <= args.dump_every <= 100000 and args.png_scale in (1, 2, 4), "dump interval / png scale")
        actions = parse_actions(args.actions, args.seconds)
        receipt["actions"] = [f"{at:g}:{action}" for at, action in actions]
        sandbox = checked_path(DEFAULT_SANDBOX_ROOT, exists=True)
        source_map = checked_path(args.map, exists=True)
        proxy = host = product = None
        if args.vanilla:
            require(not (args.proxy or args.host or args.product_dll or args.in_process or args.env or args.player_setting),
                    "--vanilla takes no DLL, host, environment or player setting")
        else:
            require(args.proxy and args.host, "--proxy and --host required")
            proxy, host = (checked_path(p, exists=True) for p in (args.proxy, args.host))
            require(pe_bits(proxy) == 32 and pe_bits(host) == 64, "x86 proxy and x64 host required")
            product = checked_path(args.product_dll, exists=True) if args.product_dll else None
        require(product is None or pe_bits(product) == 32, "x86 product DLL required")
        require(not args.in_process or product is not None, "--in-process needs --product-dll")
        require(not args.opengl or args.vanilla, "--opengl needs --vanilla")
        receipt["opengl"] = bool(args.opengl)
        receipt["mode"] = "vanilla" if args.vanilla else (
            ("product-in-process" if args.in_process else "product-proxy-mode") if product else "bare-proxy")
        extra = {}
        for item in args.env:
            name, _, value = item.partition("=")
            require(name.startswith(("WARVK_", "DXVK_", "VK_")) and value, "extra environment: WARVK_/DXVK_/VK_ NAME=VALUE")
            extra[name] = value
        plan = preflight_instance_pool(sandbox, output, source_map, 1, args.instance_id, "probe")
        require(plan["ok"] and not plan["errors"], "instance preflight failed")
        layout = build_instance_layout(sandbox, output, args.instance_id, "probe-01")
        require(not layout.instance_root.exists() and not layout.instance_root.parent.exists(), "instance must be fresh")
        probes = {bits: checked_path(PROBES / f"desktop-probe-{bits}.exe", exists=True) for bits in (32, 64)}
        extra_maps = []
        for item in args.extra_map:
            source, sep, relative = item.partition("=")
            relative = relative.replace("\\", "/")
            require(sep and relative.startswith("Maps/") and ".." not in relative.split("/")
                    and relative.lower().endswith((".w3x", ".w3m")), f"extra map {item}")
            extra_maps.append((checked_path(Path(source), exists=True), relative))
        protected = [*([proxy, host] if proxy else []), source_map, sandbox / "war3.exe", sandbox / "Game.dll", sandbox / "Storm.dll",
                     *probes.values(), *([product] if product else []), *(source for source, _ in extra_maps)]
        receipt["protectedBefore"] = [identity(p) for p in protected]
        receipt["playerDllBefore"] = guarded_identity(PLAYER_DLL)
        output.mkdir(parents=True, exist_ok=False)
        native = NativeObserver()
        for path in protected:
            native.lease(path)
        receipt["materialization"] = materialize_instance_root(layout, reuse_existing=False)
        require(receipt["materialization"]["ok"], "instance materialization failed")
        instance = layout.instance_root
        receipt["instanceRoot"] = str(instance)
        receipt["fullscreen"] = bool(args.fullscreen)
        if args.stock_exe:
            import struct
            exe = within(instance / "war3.exe", instance)
            data = bytearray(exe.read_bytes())
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            require(data[pe:pe + 4] == b"PE\0\0", "instance war3.exe header")
            characteristics = struct.unpack_from("<H", data, pe + 22)[0]
            struct.pack_into("<H", data, pe + 22, characteristics & ~0x0020)
            # A new file under the instance's name: never a write through a link to the sandbox's file.
            replacement = within(instance / "war3.exe.stock", instance)
            replacement.write_bytes(data)
            exe.unlink()
            replacement.rename(exe)
            require(exe.stat().st_nlink == 1, "instance war3.exe is still a shared link")
            receipt["stockExe"] = dict(largeAddressAwareBefore=bool(characteristics & 0x0020), largeAddressAwareAfter=False,
                                       identity=identity(exe))
        target_dll = within(instance / "d3d9.dll", instance)
        local_host = output / "host.exe"
        local_front = output / "front.dll"
        if args.vanilla:
            # Windows' own d3d9.dll must be the one the game finds.
            require(not target_dll.exists(), "a vanilla instance must have no d3d9.dll")
        else:
            if target_dll.exists():
                require(target_dll.stat().st_nlink == 1, "instance DLL is a shared hardlink")
            shutil.copy2(product or proxy, target_dll)
            shutil.copy2(host, local_host)
            require(identity(target_dll)["sha256"] == identity(product or proxy)["sha256"]
                    and identity(local_host)["sha256"] == identity(host)["sha256"], "candidate copy mismatch")
        if product:
            shutil.copy2(proxy, local_front)
            require(identity(local_front)["sha256"] == identity(proxy)["sha256"], "front end copy mismatch")
        receipt["map"] = deploy_content_addressed_map(instance, source_map)
        receipt["extraMaps"] = []
        for source, relative in extra_maps:
            target = within(instance / relative, instance)
            require(not target.exists(), f"extra map target exists: {relative}")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            require(identity(target)["sha256"] == identity(source)["sha256"], f"extra map copy differs: {relative}")
            receipt["extraMaps"].append(dict(source=identity(source), target=relative))
        receipt["loadfileArgument"] = loadfile_argument(instance, receipt["map"], False)
        if not receipt["loadfileArgument"].isascii():
            # 1.27a -loadfile crosses an ANSI boundary: an ASCII alias copy beside the staged map.
            staged = Path(receipt["map"]["target"])
            alias = within(staged.with_name("map-alias" + staged.suffix.lower()), instance)
            shutil.copy2(staged, alias)
            require(identity(alias)["sha256"] == identity(staged)["sha256"], "ASCII map alias differs")
            receipt["loadfileArgument"] = os.path.relpath(alias, instance).replace("/", "\\")
            receipt["loadfileAlias"] = True
        dumps = output / "frames"
        dumps.mkdir()
        logs = output / "logs"
        logs.mkdir()
        (output / "pipeline.conf").write_text("", encoding="utf-8")
        if args.player_setting:
            # The player's saved settings of this instance only (war3_player_settings.cpp).
            for line in args.player_setting:
                key, sep, value = line.partition("=")
                require(sep and key.replace(".", "").isalnum() and value.replace(".", "").replace("-", "").isalnum(),
                        f"player setting {line}")
            settings = within(instance / "WarVK/settings.ini", instance)
            settings.parent.mkdir(parents=True, exist_ok=True)
            lines = ["[" + PLAYER_SETTINGS_SECTION + "]", *args.player_setting, ""]
            settings.write_text(chr(10).join(lines), encoding="utf-8")
            receipt["playerSettings"] = list(args.player_setting)
        environment = {k: v for k, v in os.environ.items() if k.upper() in ENV_ALLOW}
        environment.update(TEMP=str(instance / "WarVK/Temp"), TMP=str(instance / "WarVK/Temp"),
            DXVK_LOG_PATH=str(logs), DXVK_LOG_LEVEL="info", DXVK_CONFIG_FILE=str(output / "pipeline.conf"),
            DXVK_SHADER_CACHE="0",
            WARVK_D3D9_GAME="sandbox-isolated-v1", WARVK_D3D9_PROFILE="presenter-isolated-v1",
            WARVK_D3D9_HOST_EXE=str(local_host), WARVK_D3D9_HOST_LOG=str(output / "host.log"),
            WARVK_D3D9_FRAME_DUMP=str(dumps), WARVK_D3D9_FRAME_DUMP_EVERY=str(args.dump_every))
        environment.update(PROFILES[args.profile])
        if product:
            # The product's own switches, as the in-process real-machine gates set them
            # (run_native_d3d9_survey.safe_environment), plus the proxy-mode request.
            environment.update(WARVK_D3D9_FRONT=str(local_front), WARVK_D3D9_ANNOTATIONS="1",
                DXVK_WAR3_INTERNAL_TEST_API="1", DXVK_WAR3_INTERNAL_EXIT_TEST="1",
                DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE="1",
                DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE="1", DXVK_WAR3_FRAME_EVIDENCE="0",
                DXVK_WAR3_FRAME_EVIDENCE_INPUTS="0", DXVK_WAR3_FRAME_EVIDENCE_DRAWS="0",
                DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT="0", DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED="0")
        if args.in_process:
            # No front end, and nothing of the transport's: the product runs as players have it.
            environment = {k: v for k, v in environment.items() if not k.startswith("WARVK_D3D9_")}
        if args.vanilla:
            # Nothing of WarVK's or DXVK's: the system's Direct3D 9 reads none of it anyway.
            environment = {k: v for k, v in environment.items() if not k.startswith(("WARVK_", "DXVK_"))}
        environment.update(extra)
        receipt["childEnvironment"] = environment
        stop = threading.Event()
        with IsolatedRenderDesktop(name_prefix="WarVK_GameProxyProbe", restricted_medium=True) as desktop:
            receipt["desktopSafety"] = desktop.receipt
            probe = desktop.run([probes[32], "--expect-desktop", desktop.desktop_name, "--expect-station", "WinSta0",
                "--input-desktop", desktop.input_before, "--child", probes[64], "--child-log", output / "probe-child.log"],
                cwd=output, stdout_path=output / "probe.stdout", stderr_path=output / "probe.stderr", timeout_s=30,
                env=environment)
            records = []
            for path in (output / "probe.stdout", output / "probe-child.log"):
                records.extend(strict_json(line) for line in path.read_text("utf-8").splitlines() if line.startswith("{"))
            require(len(records) == 2, "expected the x86/x64 desktop probe pair")
            validate_probe_receipt(records[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station="WinSta0", bits=32, child=records[1], launch=probe)
            receipt["observer"] = {}
            observer = threading.Thread(target=observe, args=(desktop, len(desktop.receipt["launches"]),
                instance / "war3.exe", output, stop, receipt["observer"], args.seconds, 20.0, actions,
                product is not None), daemon=True)
            observer.start()
            try:
                receipt["gameLaunch"] = desktop.run([instance / "war3.exe", *([] if args.fullscreen else ["-window"]),
                    *(["-opengl"] if args.opengl else []), "-loadfile", receipt["loadfileArgument"]], cwd=instance, stdout_path=output / "game.stdout",
                    stderr_path=output / "game.stderr", timeout_s=args.timeout_seconds, env=environment)
            finally:
                stop.set()
                observer.join(timeout=30)
                receipt["observerJoined"] = not observer.is_alive()
    except Exception as error:
        receipt["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        if native is not None:
            receipt["errors"].extend(native.close())
        if protected:
            receipt["protectedAfter"] = [identity(p) for p in protected]
            receipt["protectedUnchanged"] = receipt.get("protectedBefore") == receipt["protectedAfter"]
        receipt["playerDllAfter"] = guarded_identity(PLAYER_DLL)
    if output is not None and output.exists():
        try:
            summarize(output, receipt, args.png_scale)
        except Exception as error:
            receipt["errors"].append(f"summary: {type(error).__name__}: {error}")
        checks = receipt["checks"]
        launch = receipt.get("gameLaunch") or {}
        observer = receipt.get("observer") or {}
        # A product whose host was ended ends itself, with its own exit code and no WM_CLOSE.
        lost = observer.get("hostLost") is True
        code = launch.get("exitCode")
        checks.update(
            gameExitedByItself=(launch.get("timedOut") is not True and isinstance(code, int)
                                and code & 0xFFFFFFFF == HOST_LOST_EXIT) if lost
                               else (launch.get("ok") is True and code == 0),
            closePosted=lost or bool((observer.get("closed") or {}).get("posted")),
            actionsPosted=all(row.get("posted") for row in observer.get("actions") or [{}]),
            x86VulkanAbsent=(observer.get("modules") or {}).get("vulkanLoaded") is False,
            proxyIsD3d9="_AutoTestInstances" in str((observer.get("modules") or {}).get("d3d9")),
            protectedUnchanged=receipt.get("protectedUnchanged") is True,
            playerDllUntouched=receipt.get("playerDllBefore") == receipt.get("playerDllAfter"),
            noRunnerErrors=not receipt["errors"] and not observer.get("errors"))
        if receipt.get("mode") == "product-in-process":
            # What only a host session has does not apply; the device is in this process.
            for name in ("deviceCreated", "hostLogWritten", "hostsReturnedNormally", "sessionsEndedClean",
                         "framesDumped", "frameNotFlat", "x86VulkanAbsent"):
                checks.pop(name, None)
            plane = observer.get("controlPlane") or {}
            checks.update(x86VulkanLoaded=(observer.get("modules") or {}).get("vulkanLoaded") is True,
                          controlPlaneAnswered=plane.get("pinged") is True,
                          gameReady=plane.get("ready") is True,
                          captureVerified=plane.get("captureVerified") is True)
        if receipt.get("mode") == "vanilla":
            # No WarVK at all: no device of ours, no host, no dumps, no control plane.
            for name in ("deviceCreated", "hostLogWritten", "hostsReturnedNormally", "sessionsEndedClean",
                         "framesDumped", "frameNotFlat", "proxyIsD3d9"):
                checks.pop(name, None)
            modules = observer.get("modules") or {}
            if receipt.get("opengl"):
                # The game's own OpenGL renderer and the GPU vendor's OpenGL driver in its process.
                checks.update(openglDriver="opengl32.dll" in (modules.get("names") or [])
                              and any(n.startswith(("nvoglv", "atio", "ig")) for n in modules.get("names") or []))
            else:
                d3d9 = str(modules.get("d3d9") or "").replace("/", "\\").lower()
                checks.update(systemD3d9=d3d9.endswith("\\syswow64\\d3d9.dll"))
        if receipt.get("mode") == "product-proxy-mode":
            plane = observer.get("controlPlane") or {}
            checks.update(frontEndLoaded="front.dll" in str((observer.get("modules") or {}).get("front")),
                          controlPlaneAnswered=plane.get("pinged") is True,
                          gameReady=plane.get("ready") is True,
                          captureVerified=plane.get("captureVerified") is True)
        receipt["ok"] = all(checks.values())
        (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    brief = {k: receipt.get(k) for k in ("ok", "checks", "errors", "proxyTrace", "terminals")}
    brief["frames"] = [(f["present"], f"{f['width']}x{f['height']}", f.get("fpsSincePrevious")) for f in receipt.get("frames") or []]
    brief["hosts"] = [(h["log"], h["summary"], h["exitIntent"]) for h in receipt.get("hosts") or []]
    brief["actions"] = [(a["seconds"], a["action"], a.get("posted"), a.get("error")) for a in
                        (receipt.get("observer") or {}).get("actions") or []]
    brief["controlPlane"] = (receipt.get("observer") or {}).get("controlPlane")
    brief["observerErrors"] = (receipt.get("observer") or {}).get("errors")
    brief["exit"] = (receipt.get("observer") or {}).get("exit")
    brief["gameExitCode"] = (receipt.get("gameLaunch") or {}).get("exitCode")
    brief["timedOut"] = (receipt.get("gameLaunch") or {}).get("timedOut")
    print(json.dumps(brief, ensure_ascii=False, indent=2, default=str))
    return 0 if receipt["ok"] else 1


def summarize(output, receipt, scale):
    checks = receipt["checks"]
    text = (output / "game.stderr").read_text("utf-8", errors="replace") if (output / "game.stderr").is_file() else ""
    lines = text.splitlines()
    game = [line[len("D3D9 game: "):] for line in lines if line.startswith("D3D9 game: ")]
    unsupported = sorted({line[len("unsupported "):] for line in game if line.startswith("unsupported ")})
    other = [line for line in lines if not line.startswith("D3D9 game: ")]
    receipt["proxyTrace"] = dict(lines=len(game), createDevice=[l for l in game if l.startswith("CreateDevice")][:6],
        unsupported=unsupported, dumps=sum(1 for l in game if l.startswith("frame dump") and "written" in l),
        presentFailures=[l for l in game if l.startswith("Present #")][:4],
        failures=[l for l in other if "failure" in l or "dev worker" in l or "dev call" in l or "terminal" in l][:12],
        otherLines=len(other))
    checks["deviceCreated"] = any("initialize hr=0x00000000" in l for l in game)
    # One host per Direct3D9 object the game made: host.log, host.log.1, ...
    # The last one served the device.
    hosts = []
    logs = sorted(output.glob("host.log*"), key=lambda path: int(path.suffix[1:]) if path.suffix[1:].isdigit() else 0)
    for log in logs:
        text_lines = log.read_text("utf-8", errors="replace").splitlines()
        by_schema = {}
        for line in text_lines:
            if line.startswith("{"):
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                by_schema.setdefault(row.get("schema"), []).append(row)
        main = (by_schema.get("warvk-d3d9-host/v1") or [{}])[-1]
        plain = [l for l in text_lines if not l.startswith("{")]
        hosts.append(dict(log=log.name, schemas={k: len(v) for k, v in by_schema.items() if k},
            summary={k: main.get(k) for k in ("accepted", "rejected", "backendCalls", "draws", "presentQueued",
                "presentApiSucceeded", "presentRetired", "liveResources", "normalTeardown", "textureUploads",
                "fixedBatches", "fixedBatchDraws") if k in main},
            exitIntent=[l[:160] for l in plain if "exit-intent" in l or "host failure" in l or "host transport" in l],
            statistics={l.split(":", 1)[0][5:]: l.split(":", 1)[1].strip()[:900] for l in plain
                        if l.startswith("D3D9 ") and ": {" in l}))
    receipt["hosts"] = hosts
    receipt["host"] = hosts[-1] if hosts else {}
    checks["hostLogWritten"] = bool(hosts)
    killed = any(a.get("action") == "host=kill" and a.get("posted") for a in
                 (receipt.get("observer") or {}).get("actions") or [])
    receipt["hostKilledByProbe"] = killed
    # A probe that ends its own host expects that one host not to return.
    normal = [any("strategy=normal-return code=0" in l for l in h["exitIntent"]) for h in hosts]
    checks["hostsReturnedNormally"] = bool(hosts) and all(normal[:-1] if killed else normal) and (not killed or not normal[-1])
    terminals = [json.loads(l[len("D3D9 terminal: "):]) for l in lines if l.startswith("D3D9 terminal: {")]
    receipt["terminals"] = terminals
    clean = [t.get("kind") == "none" and t.get("published") == t.get("applied") for t in terminals]
    lost = (receipt.get("observer") or {}).get("hostLost") is True
    if lost:
        # The product ended its process at once: the last session printed no account of its own.
        # Its account is the product's report.
        checks["sessionsEndedClean"] = len(terminals) == len(hosts) - 1 and all(clean)
        report = Path(receipt["instanceRoot"]) / "WarVK/Log/render-host-lost.txt"
        text = report.read_text("utf-8-sig", errors="replace") if report.is_file() else ""
        if text:
            shutil.copy2(report, output / "render-host-lost.txt")

        def field(pattern):
            found = re.search(pattern, text, re.M)
            return found.group(1).strip() if found else None

        row = dict(written=bool(text), title=text.splitlines()[0] if text else None, kind=field(r"^kind=(\S+)"),
                   hostExit=field(r"^hostExit=(\d+)"), published=field(r"^published=(\d+)"),
                   applied=field(r"^published=\d+ applied=(\d+)"), hostLog=field(r"^hostLog=(.+)$"),
                   dialog=field(r"^dialog=(\w+)"), exitCode=field(r"^exitCode=(\S+)"))
        receipt["hostLostReport"] = row
        checks["hostLostReported"] = (row["title"] == NOTICE_TITLE and row["kind"] == "host-exited"
            and row["hostExit"] == "99" and (row["published"] or "").isdigit() and (row["applied"] or "").isdigit()
            and int(row["applied"]) <= int(row["published"]) and bool(row["hostLog"])
            and Path(row["hostLog"]).is_file() and Path(row["hostLog"]).parent == output
            and row["exitCode"] == f"0x{HOST_LOST_EXIT:08X}")
    else:
        checks["sessionsEndedClean"] = len(terminals) == len(hosts) and all(clean[:-1] if killed else clean) and (
            not killed or not clean[-1])
    frames = []
    for path in sorted((output / "frames").glob("frame-*.bgra")):
        index, extent = path.stem.split("-")[1:3]
        width, height = (int(v) for v in extent.split("x"))
        data = path.read_bytes()
        require(len(data) == width * height * 4, "frame dump size")
        png = path.with_suffix(".png")
        write_png(png, width, height, data, scale)
        frames.append(dict(present=int(index), width=width, height=height, png=png.name,
                           mtime=path.stat().st_mtime, **frame_statistics(data)))
    frames.sort(key=lambda row: row["present"])
    for row, before in zip(frames[1:], frames):
        seconds = row["mtime"] - before["mtime"]
        row["fpsSincePrevious"] = round((row["present"] - before["present"]) / seconds, 1) if seconds > 0 else None
    receipt["frames"] = frames
    checks["framesDumped"] = len(frames) >= 1
    checks["frameNotFlat"] = any(f["sampledBlueValues"] > 8 for f in frames)


if __name__ == "__main__":
    sys.exit(main())
