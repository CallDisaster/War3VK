"""Production wiring guard for the A->B->A native-table reset signal.

The component test exercises the atomic one-shot policy. This test verifies
that real hooks call it at the intended sides of the original game functions.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOOK = (ROOT / "src/d3d9/d3d9_war3_hook.cpp").read_text(encoding="utf-8")
JASS = (ROOT / "src/d3d9/war3/hooks/war3_hook_jass.cpp").read_text(
    encoding="utf-8")
LIFECYCLE = (ROOT / "src/d3d9/war3/hooks/war3_hook_lifecycle.cpp").read_text(
    encoding="utf-8")


def body(text: str, begin: str, end: str) -> str:
    return text.split(begin, 1)[1].split(end, 1)[0]


class MapTransitionSignalWiring(unittest.TestCase):
    def test_native_signal_closes_before_original_rebuild(self) -> None:
        section = body(JASS, "static int __cdecl Hook_InitJassNatives() {",
                       "int __fastcall Hook_ExecuteJassFunction")
        self.assertLess(section.index("ObserveWar3JassNativeTableRebuild()"),
                        section.index("g_trampolineInitJassNatives()"))

    def test_both_runner_entries_have_fallback_and_rearm(self) -> None:
        for begin, end, original in (
            ("static int __fastcall Hook_MainRunner(",
             "static int __fastcall Hook_MainRunner_Alt(",
             "g_trampolineMainRunner(thisPtr, edx)"),
            ("static int __fastcall Hook_MainRunner_Alt(",
             "DWORD __fastcall Hook_GetD3d9Parameters(",
             "g_trampolineMainRunnerAlt(thisPtr, edx)"),
        ):
            section = body(LIFECYCLE, begin, end)
            self.assertLess(section.index("ObserveWar3MainRunnerEnter()"),
                            section.index(original))
            self.assertGreater(section.index("ObserveWar3MainRunnerComplete()"),
                               section.index("ActivateWar3Runtime("))

    def test_one_shot_gate_and_present_owned_request(self) -> None:
        section = body(HOOK, "void ObserveWar3JassNativeTableRebuild() {",
                       "/**\n * @brief 运行时激活入口")
        self.assertEqual(section.count(
            "war3::RequestActiveDeviceShadowMapResetOrCpuFallback()"), 2)
        self.assertIn("s_mapTransitionGate.onNativeTableRebuild()", section)
        self.assertIn("s_mapTransitionGate.onMainRunnerEnter()", section)
        self.assertNotIn("War3ApplyShadowMapEpochResetAtPresent", section)

    def test_explicit_game_exit_disarms(self) -> None:
        section = body(HOOK, "void ResetWar3RuntimeState() {",
                       "bool ValidateGameModule(")
        self.assertLess(section.index("s_mapTransitionGate.onGameExit()"),
                        section.index("RequestActiveDeviceShadowMapResetOrCpuFallback()"))


if __name__ == "__main__":
    unittest.main()
