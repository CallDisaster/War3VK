"""d3d9.dll must stay loaded once a hook into the game or Storm exists.

Game.dll frees d3d9.dll while it shuts the graphics device down and keeps calling
Storm afterwards. Without the pin the main-menu exit ends in 0xC0000005 inside the
unmapped Hooked_SMemFree. No game, build or Win32 call here.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOOK = (ROOT / "src/d3d9/d3d9_war3_hook.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src/d3d9/d3d9_main.cpp").read_text(encoding="utf-8")
PIN = "PinModuleForInstalledHooks("


def body(signature: str) -> str:
    start = HOOK.index(signature)
    open_brace = HOOK.index("{", start)
    depth = 0
    for index in range(open_brace, len(HOOK)):
        depth += (HOOK[index] == "{") - (HOOK[index] == "}")
        if depth == 0:
            return HOOK[open_brace:index + 1]
    raise AssertionError(f"unterminated body: {signature}")


class ModulePin(unittest.TestCase):
    def before(self, signature: str, *later: str) -> None:
        text = body(signature)
        self.assertEqual(text.count(PIN), 1, signature)
        for marker in later:
            self.assertIn(marker, text, signature)
            self.assertLess(text.index(PIN), text.index(marker), f"{signature}: {marker}")

    def test_pin_is_permanent_and_targets_this_module(self):
        text = body("void PinModuleForInstalledHooks(const char *source)")
        self.assertRegex(text, r"GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS\s*\|\s*GET_MODULE_HANDLE_EX_FLAG_PIN")
        self.assertIn("&PinModuleForInstalledHooks", text)
        self.assertIn("GetModuleHandleExW", text)

    def test_every_install_path_pins_first(self):
        self.before("void War3Hook::InstallHooks(IDirect3DDevice9 *device)",
                    "TryInstallShadowHooksEarly(", "War3HookJass::Install(",
                    "War3HookLifecycle::Install(")
        self.before("void ActivateWar3Runtime(uintptr_t gameBase, const char *source)",
                    "InitializeRuntimeCore(", "War3Hook::InstallGameHooks(")
        self.before("void TryInstallShadowHooksEarly(uintptr_t gameBase, const char *source)",
                    "TryInstallStormBreakerEarly(")

    def test_disabled_hooks_do_not_pin(self):
        text = body("void War3Hook::InstallHooks(IDirect3DDevice9 *device)")
        self.assertLess(text.index('"DXVK_WAR3_ENABLE_HOOKS"'), text.index(PIN))
        self.assertLess(text.index('"DXVK_WAR3_BOOTSTRAP_NO_GAME_HOOKS"'), text.index(PIN))

    def test_a_plain_load_is_not_pinned(self):
        self.assertIsNone(re.search(r"GET_MODULE_HANDLE_EX_FLAG_PIN", MAIN))


if __name__ == "__main__":
    unittest.main()
