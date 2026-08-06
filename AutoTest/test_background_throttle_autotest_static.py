#!/usr/bin/env python3
"""Static contracts for the 1.27a WM_ACTIVATEAPP background-throttle bypass."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
ADDRESS_BOOK_H = ROOT / "src/d3d9/war3/hooks/war3_hook_address_book.h"
ADDRESS_BOOK_CPP = ROOT / "src/d3d9/war3/hooks/war3_hook_address_book.cpp"
LIFECYCLE = ROOT / "src/d3d9/war3/hooks/war3_hook_lifecycle.cpp"
AUTOTEST = ROOT / "AutoTest/war3_autotest_mcp.py"
PERF_GATE = ROOT / "AutoTest/run_unattended_perf_gate.py"
VISUAL_PROBE = ROOT / "AutoTest/run_bridge_ramp_visual_probe.py"


class BackgroundThrottleAutoTestContracts(unittest.TestCase):
    def test_release_defaults_do_not_change_game_pause_or_background_sleep(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn("kAutoTestDisableGamePause = false", text)
        self.assertIn("kAutoTestDisableBackgroundIdleSleep = false", text)

    def test_127a_address_is_the_narrow_background_sleep_source(self) -> None:
        header = ADDRESS_BOOK_H.read_text(encoding="utf-8")
        source = ADDRESS_BOOK_CPP.read_text(encoding="utf-8")
        self.assertIn("uintptr_t backgroundIdleSleepMs = 0;", header)
        self.assertIn("0x1552E0, // backgroundIdleSleepMs", source)

    def test_hook_only_rewrites_the_background_idle_sleep_value(self) -> None:
        text = LIFECYCLE.read_text(encoding="utf-8")
        self.assertIn("using BackgroundIdleSleepMsFn = DWORD(__cdecl *)();", text)
        self.assertIn("Hook_BackgroundIdleSleepMs", text)
        self.assertIn(
            "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE", text
        )
        self.assertIn("const DWORD nativeSleepMs = targetFn();", text)
        self.assertIn("return 0u;", text)
        install = text.index('"Lifecycle", "BackgroundIdleSleepMs", false, false);')
        pause_install = text.index('"Lifecycle", "GamePause", false, false);')
        self.assertLess(install, pause_install)

    def test_all_autotest_launchers_default_to_the_opt_in_child_environment(self) -> None:
        text = AUTOTEST.read_text(encoding="utf-8")
        self.assertIn(
            'AUTOTEST_BACKGROUND_THROTTLE_ENV = '
            '"DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE"',
            text,
        )
        self.assertIn(
            'AUTOTEST_GAME_PAUSE_ENV = "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE"',
            text,
        )
        self.assertIn(
            'AUTOTEST_INTERNAL_TEST_API_ENV = "DXVK_WAR3_INTERNAL_TEST_API"',
            text,
        )
        self.assertEqual(
            text.count(
                'extra_env.setdefault(AUTOTEST_BACKGROUND_THROTTLE_ENV, "1")'
            ),
            2,
        )
        self.assertEqual(
            text.count('extra_env.setdefault(AUTOTEST_GAME_PAUSE_ENV, "1")'),
            2,
        )
        self.assertEqual(
            text.count(
                'extra_env.setdefault(AUTOTEST_INTERNAL_TEST_API_ENV, "1")'
            ),
            2,
        )
        self.assertIn("backgroundIdleSleepBypassed", text)

    def test_perf_gate_no_longer_silently_demotes_the_process(self) -> None:
        text = PERF_GATE.read_text(encoding="utf-8")
        main_text = text[text.index("def main() -> int:"):]
        self.assertIn('"high": (0x00000080, "HIGH")', text)
        self.assertIn('default="high"', text)
        self.assertIn(
            'attempt = _set_owned_process_priority(args.process_priority)',
            main_text,
        )
        self.assertNotIn("attempt = _lower_owned_process_priority()", main_text)
        self.assertIn(
            'extra_env["DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE"] = (',
            text,
        )
        self.assertIn('default="disabled"', text)

    def test_visual_probe_keeps_legacy_low_priority_only_as_an_explicit_option(self) -> None:
        text = VISUAL_PROBE.read_text(encoding="utf-8")
        self.assertIn('default="below-normal"', text)
        self.assertIn(
            'attempt = gate._set_owned_process_priority(args.process_priority)',
            text,
        )
        self.assertNotIn("attempt = gate._lower_owned_process_priority()", text)
        self.assertIn('default="disabled"', text)


if __name__ == "__main__":
    unittest.main()
