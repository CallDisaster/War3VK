#!/usr/bin/env python3
"""Static contracts for the isolated bridge/ramp pause diagnostic."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_H = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.h"
BRIDGE_CPP = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
LIFECYCLE_CPP = ROOT / "src/d3d9/war3/hooks/war3_hook_lifecycle.cpp"
TEST_API = ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp"
RUNNER = ROOT / "AutoTest/run_bridge_ramp_visual_probe.py"


class JassPauseTestApiContracts(unittest.TestCase):
    def test_pause_native_is_signature_gated(self) -> None:
        text = BRIDGE_CPP.read_text(encoding="utf-8")
        self.assertIn('LookupNativeEntry("PauseGame")', text)
        self.assertIn("result.paramCount == 1u", text)
        self.assertIn("result.signature[1] == 'B'", text)
        self.assertIn("result.signature[3] == 'V'", text)
        self.assertLess(
            text.index("result.signatureValidated ="),
            text.index("pauseFn(paused ? 1u : 0u)"),
        )

    def test_pause_api_is_diagnostic_only(self) -> None:
        header = BRIDGE_H.read_text(encoding="utf-8")
        api = TEST_API.read_text(encoding="utf-8")
        self.assertIn("SetJassGamePausedForTest", header)
        self.assertEqual(api.count('state->request.command == "game.pause"'), 1)
        self.assertIn("SetJassGamePausedForTest(paused)", api)

    def test_pause_request_has_thread_local_scoped_bypass(self) -> None:
        bridge = BRIDGE_CPP.read_text(encoding="utf-8")
        lifecycle = LIFECYCLE_CPP.read_text(encoding="utf-8")
        self.assertIn(
            "thread_local bool s_internalTestGamePauseBypass = false",
            lifecycle,
        )
        self.assertIn("if (s_internalTestGamePauseBypass)", lifecycle)
        enable = bridge.index(
            "SetInternalTestGamePauseBypassForCurrentThread(true)"
        )
        invoke = bridge.index("pauseFn(paused ? 1u : 0u)", enable)
        restore = bridge.index(
            "SetInternalTestGamePauseBypassForCurrentThread(previousBypass)",
            invoke,
        )
        self.assertLess(enable, invoke)
        self.assertLess(invoke, restore)

    def test_all_shadow_debug_layers_are_reachable(self) -> None:
        api = TEST_API.read_text(encoding="utf-8")
        self.assertIn("(std::min)(mode, 9u)", api)

    def test_sun_lock_keeps_simulation_independent(self) -> None:
        api = TEST_API.read_text(encoding="utf-8")
        runner = RUNNER.read_text(encoding="utf-8")
        self.assertEqual(
            api.count('state->request.command == "shadow.lock_sun"'), 1
        )
        self.assertIn("settings->shadows.lockSun = enabled", api)
        self.assertIn('command="shadow.lock_sun"', runner)
        self.assertIn('"DXVK_WAR3_CSM_CONTINUITY_TRACE": "1"', runner)

    def test_runner_resumes_before_cleanup(self) -> None:
        text = RUNNER.read_text(encoding="utf-8")
        pause_call = text.index('payload={"paused": True}')
        resume_call = text.index('payload={"paused": False}')
        cleanup_call = text.index("cleanup = gate._cleanup_owned_process()", resume_call)
        self.assertLess(pause_call, resume_call)
        self.assertLess(resume_call, cleanup_call)


if __name__ == "__main__":
    unittest.main()
