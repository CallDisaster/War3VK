#!/usr/bin/env python3
"""Contracts for non-invasive first-chance and fatal-only dump capture."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


HANDLER = read("src/d3d9/war3/core/war3_crash_handler.cpp")
POLICY = read("src/d3d9/war3/core/war3_crash_capture_policy.h")
RUNNABLE = read(
    "src/d3d9/war3/core/tests/war3_crash_capture_policy_test.cpp"
)
MESON = read("src/d3d9/meson.build")


class CrashHandlerCapturePolicyStaticTests(unittest.TestCase):
    def test_default_install_does_not_register_first_chance_handler(self) -> None:
        self.assertIn("DXVK_WAR3_CRASH_FIRST_CHANCE_TRACE", HANDLER)
        self.assertIn("if (firstChanceTraceEnabled)", HANDLER)
        self.assertIn("AddVectoredExceptionHandler(", HANDLER)
        self.assertIn("0, War3VectoredExceptionHandler", HANDLER)
        self.assertIn('"disabled"', HANDLER)

    def test_vectored_path_is_memory_only(self) -> None:
        vectored = HANDLER.split(
            "LONG WINAPI War3VectoredExceptionHandler", 1
        )[1].split("LONG WINAPI War3UnhandledExceptionFilter", 1)[0]
        self.assertIn("recordFirstChance", vectored)
        for forbidden in (
            "MiniDumpWriteDump",
            "CreateFileW",
            "MakeCrashDirectory",
            "WriteSummaryJson",
            "Print(",
        ):
            self.assertNotIn(forbidden, vectored)

    def test_fatal_gate_is_independent_and_latest_means_fatal(self) -> None:
        self.assertNotIn("g_dumpInProgress", HANDLER)
        self.assertIn("tryBeginFatalCapture", HANDLER)
        self.assertIn('\\"kind\\": \\"fatal\\"', HANDLER)
        self.assertIn("latest_crash.json", HANDLER)
        self.assertIn('\\"firstChance\\": false', HANDLER)

    def test_policy_has_only_atomic_scalar_first_chance_state(self) -> None:
        for token in (
            "std::atomic<uint32_t> m_firstChanceCount",
            "std::atomic<uintptr_t> m_lastFirstChanceAddress",
            "std::atomic<bool> m_fatalCaptureStarted",
            "std::memory_order_relaxed",
            "std::memory_order_release",
        ):
            self.assertIn(token, POLICY)

    def test_runnable_proves_first_chance_does_not_consume_fatal(self) -> None:
        self.assertIn("recordFirstChance", RUNNABLE)
        self.assertIn("assert(coordinator.tryBeginFatalCapture())", RUNNABLE)
        self.assertIn("assert(!coordinator.tryBeginFatalCapture())", RUNNABLE)
        self.assertIn("war3_crash_capture_policy_test", MESON)


if __name__ == "__main__":
    unittest.main()
