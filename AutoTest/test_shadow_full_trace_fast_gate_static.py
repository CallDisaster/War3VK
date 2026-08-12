"""Static contracts for the disabled full-trace render-thread fast path."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp"
).read_text(encoding="utf-8", errors="ignore")


def function_body(signature: str, next_signature: str) -> str:
    start = SOURCE.index(signature)
    end = SOURCE.index(next_signature, start)
    return SOURCE[start:end]


class ShadowFullTraceFastGateContracts(unittest.TestCase):
    def test_gate_is_atomic_and_initially_allows_environment_probe(self) -> None:
        self.assertIn(
            "std::atomic<bool> g_shadowPoseFullTraceFastEnabled{true};",
            SOURCE,
        )
        getter = function_body(
            "bool ShadowPoseFullTraceFastEnabled() noexcept {",
            "bool ShadowPoseFullTraceDeadlineReachedLocked()",
        )
        self.assertIn("load(std::memory_order_acquire)", getter)
        config = function_body(
            "ShadowPoseFullTraceConfigSnapshot ShadowPoseFullTraceConfigLocked()",
            "bool ShadowPoseFullTraceFastEnabled() noexcept",
        )
        self.assertIn("store(config.enabled,", config)
        self.assertIn("std::memory_order_release", config)

    def test_render_hot_paths_gate_before_trace_mutex(self) -> None:
        functions = (
            (
                "void MaybeWriteShadowPoseFullTrace(",
                "void NoteCurrentDrawSnapshotFrame(",
            ),
            (
                "void NoteFinalShadowCasterFrame(",
                "void NoteCurrentDrawSnapshotFrame(",
            ),
            (
                "void NoteCurrentDrawSnapshotFrame(",
                "void NoteShadowFrameCadenceSample(",
            ),
        )
        for signature, next_signature in functions:
            body = function_body(signature, next_signature)
            gate = body.index("if (!ShadowPoseFullTraceFastEnabled())")
            lock = body.index("g_shadowPoseFullTraceMutex")
            self.assertLess(gate, lock, signature)

    def test_manual_start_and_stop_publish_gate(self) -> None:
        start = function_body(
            "void StartShadowPoseFullTrace(",
            "void StopShadowPoseFullTrace()",
        )
        stop = function_body(
            "void StopShadowPoseFullTrace()",
            "ShadowPoseFullTraceStatus QueryShadowPoseFullTraceStatus()",
        )
        self.assertIn("store(true, std::memory_order_release)", start)
        self.assertIn("store(false, std::memory_order_release)", stop)

    def test_terminal_trace_states_close_gate(self) -> None:
        self.assertGreaterEqual(
            SOURCE.count("store(false, std::memory_order_release)"), 5
        )
        open_failure = function_body(
            "bool EnsureShadowPoseFullTraceOpenLocked(",
            "void MaybeWriteShadowPoseFullTrace(",
        )
        failure = open_failure.index("failed to open shadow pose full trace log")
        gate = open_failure.index(
            "g_shadowPoseFullTraceFastEnabled.store(false", failure
        )
        returned = open_failure.index("return false;", gate)
        self.assertLess(failure, gate)
        self.assertLess(gate, returned)


if __name__ == "__main__":
    unittest.main()
