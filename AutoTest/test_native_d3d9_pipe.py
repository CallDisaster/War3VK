"""真实本机命名管道 CPU 回归；不启动游戏、不加载产品 DLL、不操作桌面。

服务端仅占用本 Python PID 的首实例管道，客户端复用生产 request/_transfer。
两端都是真实 Win32 消息管道。用事件握手分别制造立即 MORE_DATA 和
IO_PENDING -> MORE_DATA，不依赖 sleep 或调度碰运气。进程 bind/模块证明不在
本测试范围：客户端仅跳过 verify_process，保留真实服务端 PID 与 nonce 校验。
缺片测试是合成损坏反例，不据此追认旧实机失败的原因。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes as wt
import json
import os
from pathlib import Path
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_native_d3d9_survey as survey


# A catastrophic timeout must not let Python GC free kernel-owned OVERLAPPED
# memory. Successful fixtures remove themselves; failed cleanup stays pinned.
_RETAINED_FIXTURES = []


def sized_response(request, size):
    response = dict(requestId=request["requestId"], command=request["command"],
                    ok=True, result=dict(pid=os.getpid(), padding=""))
    empty = json.dumps(response, separators=(",", ":")).encode("ascii")
    needed = size - len(empty)
    if needed < 0:
        raise ValueError("fixture response size is too small")
    response["result"]["padding"] = ("0123456789abcdef" * ((needed + 15) // 16))[:needed]
    raw = json.dumps(response, separators=(",", ":")).encode("ascii")
    if len(raw) != size:
        raise AssertionError("fixture response size drift")
    return raw


def boundary_response(request):
    """A valid three-chunk message whose lost middle chunk creates a duplicate."""
    header = json.dumps(dict(requestId=request["requestId"], command=request["command"],
                             ok=True), separators=(",", ":")).encode("ascii")[:-1]
    prefix_start = header + b',"result":{"pid":' + str(os.getpid()).encode("ascii") + b',"left":{"padding":"'
    prefix_end = b'","currentDrawLastRenderFrameIndex":1,'
    prefix = prefix_start + b"a" * (65536 - len(prefix_start) - len(prefix_end)) + prefix_end
    middle_start, middle_end = b'"filler":"', b'"},"right":{'
    middle = middle_start + b"b" * (65536 - len(middle_start) - len(middle_end)) + middle_end
    return prefix + middle + b'"currentDrawLastRenderFrameIndex":2}}}'


class TracedApi:
    """Forward every IO to Win32; retain only small completion facts."""
    def __init__(self, native, *, first_read_gate=None, read_pending=None,
                 write_gate=None, write_issued=None):
        self.native = native
        self.first_read_gate = first_read_gate
        self.read_pending = read_pending
        self.write_gate = write_gate
        self.write_issued = write_issued
        self.reads = []
        self.completions = []

    def __getattr__(self, name):
        return getattr(self.native, name)

    @staticmethod
    def await_gate(gate):
        if gate is not None and not gate.wait(5):
            raise TimeoutError("fixture handshake timed out")

    def ReadFile(self, handle, buffer, size, transferred, overlap):
        if not self.reads:
            self.await_gate(self.first_read_gate)
        ok = self.native.ReadFile(handle, buffer, size, transferred, overlap)
        error = 0 if ok else ctypes.get_last_error()
        self.reads.append(dict(ok=bool(ok), error=error, count=transferred._obj.value))
        if error == 997 and self.read_pending is not None:
            self.read_pending.set()
        ctypes.set_last_error(error)
        return ok

    def WriteFile(self, handle, buffer, size, transferred, overlap):
        self.await_gate(self.write_gate)
        ok = self.native.WriteFile(handle, buffer, size, transferred, overlap)
        error = 0 if ok else ctypes.get_last_error()
        if self.write_issued is not None:
            self.write_issued.set()
        ctypes.set_last_error(error)
        return ok

    def GetOverlappedResult(self, handle, overlap, transferred, wait):
        ok = self.native.GetOverlappedResult(handle, overlap, transferred, wait)
        error = 0 if ok else ctypes.get_last_error()
        self.completions.append(dict(ok=bool(ok), error=error, count=transferred._obj.value))
        ctypes.set_last_error(error)
        return ok


class PipeRoundTrip:
    """One bounded request on a pipe owned by this Python process only."""
    def __init__(self, response, *, mode="immediate", mutate=lambda raw: raw):
        self.response, self.mutate = response, mutate
        self.client, self.server = survey.NativeObserver(), survey.NativeObserver()
        self.client.expected = dict(pid=os.getpid(), creation100ns=0)
        self.client.verify_process = lambda: None  # No game process in this CPU fixture.
        issued, pending = threading.Event(), threading.Event()
        self.client.k = TracedApi(self.client.k,
            first_read_gate=issued if mode == "immediate" else None, read_pending=pending)
        self.server.k = TracedApi(self.server.k,
            write_gate=pending if mode == "pending" else None, write_issued=issued)
        kernel = self.server.k
        kernel.CreateNamedPipeW.restype = wt.HANDLE
        kernel.CreateNamedPipeW.argtypes = (wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD,
                                         wt.DWORD, wt.DWORD, wt.DWORD, wt.LPVOID)
        kernel.ConnectNamedPipe.restype = wt.BOOL
        kernel.ConnectNamedPipe.argtypes = (wt.HANDLE, ctypes.POINTER(survey._Overlapped))
        self.name = rf"\\.\pipe\War3ControlPlane_{os.getpid()}"
        self.handle = kernel.CreateNamedPipeW(self.name,
            3 | 0x40000000 | 0x00080000, 4 | 2 | 8, 1,
            survey.MAX_RESPONSE + 65536, 65536, 5000, None)
        if self.handle in (None, 0, ctypes.c_void_p(-1).value):
            raise OSError(ctypes.get_last_error(), "fixture first-instance pipe refused")
        self.errors = []
        self.retained_connect = None
        self.original = self.sent = None
        self.worker = threading.Thread(target=self.serve, name="survey-pipe-cpu-fixture")
        _RETAINED_FIXTURES.append(self)

    def connect(self):
        kernel = self.server.k
        event = self.server.check(kernel.CreateEventW(None, True, False, None), "fixture connect event")
        overlap = survey._Overlapped()
        overlap.hEvent = event
        try:
            ok = kernel.ConnectNamedPipe(self.handle, ctypes.byref(overlap))
            error = 0 if ok else ctypes.get_last_error()
            if ok or error == 535:  # The client connected before ConnectNamedPipe.
                return
            survey.require(error == 997, "fixture ConnectNamedPipe: " + str(error))
            if kernel.WaitForSingleObject(event, 5000) != 0:
                kernel.CancelIoEx(self.handle, ctypes.byref(overlap))
                if kernel.WaitForSingleObject(event, 1000) != 0:
                    self.retained_connect = (self.handle, event, overlap)
                    raise RuntimeError("fixture connect cancellation unsettled; retained")
                completed = wt.DWORD()
                kernel.GetOverlappedResult(self.handle, ctypes.byref(overlap), ctypes.byref(completed), False)
                raise TimeoutError("fixture connect timeout; cancellation drained")
            completed = wt.DWORD()
            self.server.check(kernel.GetOverlappedResult(self.handle, ctypes.byref(overlap),
                              ctypes.byref(completed), False), "fixture connect completion")
        finally:
            if self.retained_connect is None:
                self.server.check(kernel.CloseHandle(event), "fixture connect event close")

    def serve(self):
        try:
            self.connect()
            incoming = ctypes.create_string_buffer(65536)
            count, more = self.server._transfer(self.handle, incoming, len(incoming),
                                               False, time.monotonic() + 5)
            survey.require(not more, "fixture request unexpectedly fragmented")
            request = survey.strict_json(incoming.raw[:count].decode("utf-8"))
            self.original = self.response(request)
            self.sent = self.mutate(self.original)
            outgoing = ctypes.create_string_buffer(self.sent)
            count, more = self.server._transfer(self.handle, outgoing, len(self.sent),
                                               True, time.monotonic() + 5)
            survey.require(count == len(self.sent) and not more, "fixture partial message write")
        except BaseException as error:
            self.errors.append(error)

    def run(self):
        self.worker.start()
        try:
            return self.client.request("ping", {}, timeout=5)
        finally:
            self.worker.join(7)
            if self.worker.is_alive():
                raise RuntimeError("fixture worker still running; handles retained")
            if self.errors:
                raise RuntimeError("fixture server failed: " + repr(self.errors[0])) from self.errors[0]

    def __enter__(self):
        return self

    def __exit__(self, kind, error, traceback):
        # Never free an OVERLAPPED/buffer while the kernel can still own it.
        if self.worker.is_alive():
            return False
        server_pending = {row[0] for row in self.server.pending}
        failures = self.client.close() + self.server.close()
        if self.retained_connect is None and self.handle not in server_pending:
            self.server.check(self.server.k.CloseHandle(self.handle), "fixture server close")
        if failures or self.retained_connect is not None:
            raise RuntimeError("fixture cleanup failed; unsettled IO retained: " + repr(failures))
        _RETAINED_FIXTURES.remove(self)
        return False


@unittest.skipUnless(os.name == "nt", "true message-pipe regression requires Windows")
class RealMessagePipeTests(unittest.TestCase):
    def test_immediate_more_data_keeps_every_byte(self):
        with PipeRoundTrip(lambda request: sized_response(request, 100803)) as fixture:
            row = fixture.run()
            self.assertEqual(row["response"], survey.strict_json(fixture.original.decode("ascii")))
            self.assertEqual(fixture.client.k.reads[0]["error"], 234)
            self.assertEqual(fixture.client.k.completions[0], dict(ok=False, error=234, count=65536))
            self.assertEqual(len(fixture.client.k.reads), 2)

    def test_pending_more_data_keeps_every_byte(self):
        with PipeRoundTrip(lambda request: sized_response(request, 100803), mode="pending") as fixture:
            row = fixture.run()
            self.assertEqual(row["response"], survey.strict_json(fixture.original.decode("ascii")))
            self.assertEqual(fixture.client.k.reads[0]["error"], 997)
            self.assertEqual(fixture.client.k.completions[0], dict(ok=False, error=234, count=65536))
            self.assertEqual(len(fixture.client.k.reads), 2)

    def test_same_field_in_separate_objects_is_legal_across_three_chunks(self):
        with PipeRoundTrip(boundary_response) as fixture:
            row = fixture.run()
            self.assertEqual(row["response"], survey.strict_json(fixture.original.decode("ascii")))
            self.assertEqual(len(fixture.client.k.reads), 3)
            self.assertEqual(row["response"]["result"]["left"]["currentDrawLastRenderFrameIndex"], 1)
            self.assertEqual(row["response"]["result"]["right"]["currentDrawLastRenderFrameIndex"], 2)

    def test_pending_first_chunk_then_immediate_middle_chunk_is_preserved(self):
        with PipeRoundTrip(boundary_response, mode="pending") as fixture:
            row = fixture.run()
            self.assertEqual(row["response"], survey.strict_json(fixture.original.decode("ascii")))
            self.assertEqual([call["error"] for call in fixture.client.k.reads], [997, 234, 0])
            self.assertEqual([call["count"] for call in fixture.client.k.completions], [65536, 65536])

    def test_exact_two_mib_limit_is_accepted(self):
        with PipeRoundTrip(lambda request: sized_response(request, survey.MAX_RESPONSE)) as fixture:
            row = fixture.run()
            self.assertEqual(row["response"], survey.strict_json(fixture.original.decode("ascii")))
            self.assertEqual(len(fixture.sent), 2 * 1024 * 1024)
            self.assertEqual(len(fixture.client.k.reads), 32)

    def test_one_byte_over_limit_is_refused(self):
        with PipeRoundTrip(lambda request: sized_response(request, survey.MAX_RESPONSE + 1)) as fixture:
            with self.assertRaisesRegex(ValueError, "IPC response too large"):
                fixture.run()
            self.assertEqual(len(fixture.sent), 2 * 1024 * 1024 + 1)

    def test_missing_middle_chunk_is_not_repaired_or_permissively_parsed(self):
        with PipeRoundTrip(boundary_response,
                           mutate=lambda raw: raw[:65536] + raw[131072:]) as fixture:
            with self.assertRaisesRegex(survey.RejectedReply,
                                        "duplicate JSON field: currentDrawLastRenderFrameIndex") as raised:
                fixture.run()
            # The original three-chunk response is legal; the received bytes are
            # a different two-chunk message. Retain exactly what was rejected.
            survey.strict_json(fixture.original.decode("ascii"))
            self.assertEqual(raised.exception.raw, fixture.sent)
            self.assertEqual(len(fixture.original) - len(fixture.sent), 65536)

    def test_truncated_json_preserves_raw_failure(self):
        with PipeRoundTrip(lambda request: sized_response(request, 100803),
                           mutate=lambda raw: raw[:-1]) as fixture:
            with self.assertRaises(survey.RejectedReply) as raised:
                fixture.run()
            self.assertEqual(raised.exception.raw, fixture.sent)

    def test_genuine_duplicate_field_stays_refused(self):
        with PipeRoundTrip(lambda request: sized_response(request, 100803),
                           mutate=lambda raw: raw.replace(b'"ok":true,', b'"ok":true,"ok":true,', 1)) as fixture:
            with self.assertRaisesRegex(survey.RejectedReply, "duplicate JSON field: ok") as raised:
                fixture.run()
            self.assertEqual(raised.exception.raw, fixture.sent)


if __name__ == "__main__":
    unittest.main(verbosity=2)
