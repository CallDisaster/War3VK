#!/usr/bin/env python3
"""The product DLL's own emulated-TLS runtime stays wired in (text checks).

docs/research/2026-11-17-x64-static-tls-b-prime.md. Behaviour is proved by
run_war3_emutls_test.py (CPU test) and the same-map A/B gate; this keeps:
  - war3_emutls.cpp in the d3d9.dll sources, classified as data layer;
  - the C symbol clang calls, the thread-exit TLS callback after mingw's
    thread_local destructors, the last static destructor for FreeLibrary,
    no registry lock taken while the process terminates;
  - no thread_local inside the runtime itself; clang only (GCC keeps libgcc);
  - x86 llvm cross files that build the product DLL use -femulated-tls, the
    GCC cross files do not.
"""
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "src/d3d9/war3/memory/war3_emutls.cpp"
LLVM_X86_CROSS = ("build-win32-weather-llvm.txt",)
GCC_CROSS = ("build-win32.txt", "build-win64.txt")


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class EmutlsRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = RUNTIME.read_text(encoding="utf-8")
        cls.code = code_only(cls.text)

    def test_linked_into_the_product_dll(self):
        meson = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
        self.assertIn("'war3/memory/war3_emutls.cpp',", meson)
        sys.path.insert(0, str(ROOT / "AutoTest"))
        from run_x86_data_layer_cut_gate import classify
        self.assertEqual(classify(None, "war3_memory_war3_emutls.cpp"), "data")

    def test_defines_the_symbol_clang_calls(self):
        self.assertIn('extern "C" void *__emutls_get_address(void *opaque)', self.code)
        self.assertIn("#if defined(__clang__) && defined(_WIN32)", self.code)

    def test_thread_exit_callback_runs_after_thread_local_destructors(self):
        # mingw: tls_atexit .CRT$XLB (thread_local destructors), tlssup XLC/XLD, XLZ ends.
        self.assertIn('__attribute__((section(".CRT$XLY"), used))', self.code)
        self.assertIn("if (reason != DLL_THREAD_DETACH)", self.code)

    def test_freelibrary_release_is_the_last_static_destructor(self):
        self.assertIn("__attribute__((init_priority(101))) ImageTeardown g_imageTeardown;", self.code)
        self.assertIn('"RtlDllShutdownInProgress"', self.code)
        release = self.code[self.code.index("void EmutlsReleaseForUnload(bool processTerminating) noexcept {"):]
        release = release[:release.index("\n}\n")]
        self.assertLess(release.index("if (processTerminating)"), release.index("TlsFree(tlsIndex)"))

    def test_no_registry_lock_during_process_termination(self):
        # Threads stopped by ExitProcess may have died inside the registry lock.
        body = self.code[self.code.index("ThreadBlock *NewBlock(DWORD tlsIndex) {"):]
        body = body[:body.index("\n}\n")]
        self.assertLess(body.index("ShutdownQuery()"), body.index("LockRegistry()"))
        self.assertIn("if (!query || !query()) {", body)
        self.assertIn("else if (g_threads == block)", self.code)

    def test_every_access_keeps_the_last_error(self):
        # Hot path: the TEB slot, no Win32 call at all (TlsGetValue zeroes the last error).
        hot = self.code[self.code.index('extern "C" void *__emutls_get_address'):]
        hot = hot[:hot.index("\n}\n")]
        self.assertIn("ThreadSlot(tlsIndex)", hot)
        self.assertIn("return SlowAddress(control);", hot)
        self.assertFalse(re.search(r"\b(Tls\w+|\w*LastError|Heap\w+|NewBlock|NewObject)\s*\(", hot), hot)
        self.assertIn("static_assert(offsetof(TEB, TlsSlots) == (sizeof(void *) == 4 ? 0xE10 : 0x1480)", self.code)
        # Slow path: every call between saving and restoring it.
        slow = self.code[self.code.index("void *SlowAddress(EmutlsControl *control) {"):]
        slow = slow[:slow.index("\n}\n")]
        self.assertLess(slow.index("const DWORD error = GetLastError();"), slow.index("VariableIndex"))
        self.assertTrue(slow.rstrip().endswith("SetLastError(error);\n  return object;"), slow[-120:])

    def test_runtime_status_reports_the_counters(self):
        plane = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "../memory/war3_emutls.h"', plane)
        self.assertIn('response["result"]["emutls"]', plane)
        self.assertIn('{"lateAccesses", tls.lateAccesses}', plane)

    def test_runtime_has_no_thread_local(self):
        self.assertFalse(re.search(r"\bthread_local\b|\b__thread\b", self.code), "TLS inside the runtime")


class CrossFiles(unittest.TestCase):
    def test_llvm_x86_product_builds_use_emulated_tls(self):
        for name in LLVM_X86_CROSS:
            text = (ROOT / name).read_text(encoding="utf-8")
            for key in ("c_args", "cpp_args"):
                line = re.search(rf"^{key}\s*=\s*\[(.*)\]", text, re.M)
                self.assertTrue(line and "'-femulated-tls'" in line.group(1), f"{name} {key}")

    def test_gcc_cross_files_keep_their_own_tls(self):
        for name in GCC_CROSS:
            path = ROOT / name
            if path.is_file():
                self.assertNotIn("femulated-tls", path.read_text(encoding="utf-8"), name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
