import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
POLICY = ROOT / "src/d3d9/war3/hooks/war3_jass_legacy_command_policy.h"
HOOK = ROOT / "src/d3d9/war3/hooks/war3_hook_jass.cpp"
JAPI = ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp"
LIGHTNING = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.cpp"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"


def body(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


class WarVkJapiSafetyStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.bridge = BRIDGE.read_text(encoding="utf-8")
        cls.policy = POLICY.read_text(encoding="utf-8")
        cls.hook = HOOK.read_text(encoding="utf-8")
        cls.japi = JAPI.read_text(encoding="utf-8")
        cls.lightning = LIGHTNING.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_legacy_command_bridge_is_release_disabled_at_compile_time(self):
        self.assertIn("WARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV", self.policy)
        self.assertIn("kDevelopmentCommandsEnabled = false", self.policy)
        self.assertNotIn("getenv", self.policy.lower())

        command_route = body(
            self.bridge,
            'if (StartsWith(payload, "cmd:"))',
            's_state.lastErrorCode = -404;',
        )
        self.assertIn(
            "if constexpr (!legacy::kDevelopmentCommandsEnabled)", command_route
        )
        self.assertIn("legacy::RequiredFeatureMask", command_route)
        self.assertIn("legacy::IsCommandAllowed", command_route)
        self.assertIn("japi::GetFeatureFlags()", command_route)
        self.assertIn("disabled in release build", command_route)

        # The enable macro is passed only to the dedicated developer runnable,
        # never to the production d3d9 shared_library declaration.
        self.assertEqual(
            self.meson.count("-DWARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV=1"), 1
        )
        production = self.meson[: self.meson.index("war3_jass_legacy_command_policy_dev_test")]
        self.assertNotIn("-DWARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV=1", production)

    def test_every_legacy_shader_command_has_a_feature_policy(self):
        handler_commands = set(
            re.findall(r'args\[0\] == "([^"]+)"', self.bridge)
        )
        policy_body = body(
            self.policy,
            "inline uint32_t RequiredFeatureMask",
            "inline bool IsCommandAllowed",
        )
        policy_commands = set(re.findall(r'"([a-z0-9-]+)"', policy_body))
        self.assertEqual(handler_commands, policy_commands)

    def test_legacy_float_parser_rejects_non_finite_values(self):
        parser = body(
            self.policy,
            "inline bool ParseFiniteFloat",
            "} // namespace dxvk::war3::hooks::legacy",
        )
        self.assertIn("errno == ERANGE", parser)
        self.assertIn("!std::isfinite(parsed)", parser)
        bridge_parser = body(
            self.bridge,
            "bool ParseFloatArg",
            "void SetCommandOk",
        )
        self.assertIn("legacy::ParseFiniteFloat", bridge_parser)

    def test_jass_vm_reset_is_cpu_only(self):
        init_hook = body(
            self.hook,
            "static int __cdecl Hook_InitJassNatives()",
            "int __fastcall Hook_ExecuteJassFunction",
        )
        self.assertIn("japi::ResetAuthorState()", init_hook)
        self.assertNotIn("japi::Reset();", init_hook)

        author_reset = body(
            self.japi,
            "void ResetAuthorState() noexcept",
            "void Reset() noexcept",
        )
        self.assertIn("resetAuthorState()", author_reset)
        self.assertIn("war3shader::RemoveFogVolume", author_reset)
        self.assertNotIn("War3LightningRuntime::instance().reset()", author_reset)

        lightning_author_reset = body(
            self.lightning,
            "void War3LightningRuntime::resetAuthorState()",
            "void War3LightningRuntime::reset()",
        )
        self.assertIn("resetAuthorStateLocked()", lightning_author_reset)
        self.assertNotIn("releaseTexturesLocked", lightning_author_reset)

        lightning_full_reset = body(
            self.lightning,
            "void War3LightningRuntime::reset()",
            "void War3LightningRuntime::resetAuthorStateLocked()",
        )
        self.assertIn("releaseTexturesLocked()", lightning_full_reset)
        self.assertIn("War3LightningRuntime::instance().reset();", self.device)

    def test_japi_does_not_hold_settings_lock_across_delegate_apis(self):
        dispatch = body(
            self.japi,
            "bool CommandUsesDirectRenderSettings",
            "bool IsRuntimeReady() noexcept",
        )
        direct_policy = body(
            dispatch,
            "bool CommandUsesDirectRenderSettings",
            "Reply DispatchBackend",
        )
        direct_commands = set(re.findall(
            r"case CommandId::([A-Za-z0-9_]+):", direct_policy
        ))
        settings_cases = set()
        cases = list(re.finditer(
            r"case CommandId::([A-Za-z0-9_]+):", dispatch
        ))
        for index, match in enumerate(cases):
            finish = cases[index + 1].start() if index + 1 < len(cases) else len(dispatch)
            if "settings->" in dispatch[match.start():finish]:
                settings_cases.add(match.group(1))
        self.assertEqual(settings_cases, direct_commands)

        for delegated in (
            "PointLightCreate",
            "PointLightSetShadowEnabled",
            "VolumetricSetEnabled",
            "VolumetricSetGlobalMediumEnabled",
            "VolumetricFogSetEnabled",
            "VolumetricFogSetSettings",
            "LocalFogCreateSphere",
            "LocalFogSetPosition",
        ):
            self.assertNotIn(f"case CommandId::{delegated}:", direct_policy)

        point_create = body(
            dispatch,
            "case CommandId::PointLightCreate:",
            "case CommandId::PointLightDestroy:",
        )
        self.assertIn("war3shader::AddPointLight", point_create)
        self.assertNotIn("settings->", point_create)

        point_shadow = body(
            dispatch,
            "case CommandId::PointLightSetShadowEnabled:",
            "case CommandId::PointLightSetShadowConfig:",
        )
        self.assertIn("war3shader::SetPointLightShadowIntensity", point_shadow)
        self.assertNotIn("settings->", point_shadow)

        for start, end in (
            ("case CommandId::VolumetricSetEnabled:",
             "case CommandId::VolumetricSetGlobalMediumEnabled:"),
            ("case CommandId::VolumetricSetGlobalMediumEnabled:",
             "case CommandId::VolumetricSetDensity:"),
            ("case CommandId::VolumetricFogSetEnabled:",
             "case CommandId::VolumetricFogSetSettings:"),
            ("case CommandId::VolumetricFogSetSettings:",
             "case CommandId::DayNightSetEnabled:"),
        ):
            delegated_body = body(dispatch, start, end)
            self.assertIn("war3shader::", delegated_body)
            self.assertNotIn("settings->", delegated_body)


if __name__ == "__main__":
    unittest.main()
