import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WARVK_ROOT = ROOT / "WarVK"
JASS_PATH = WARVK_ROOT / "jass/warvk_api.j"
SMOKE_JASS_PATH = WARVK_ROOT / "jass/warvk_smoke_test.j"
INIT_JASS_PATH = WARVK_ROOT / "jass/warvk_init.j"
ACTION_PATH = WARVK_ROOT / "action.txt"
CALL_PATH = WARVK_ROOT / "call.txt"
RUNTIME_PATH = ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp"
BRIDGE_PATH = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
LIGHTNING_HEADER = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.h"
LIGHTNING_SOURCE = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.cpp"


class WarVKJapiV1IntegratedStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.jass = JASS_PATH.read_text(encoding="utf-8")
        cls.smoke_jass = SMOKE_JASS_PATH.read_text(encoding="utf-8")
        cls.init_jass = INIT_JASS_PATH.read_text(encoding="utf-8")
        cls.action = ACTION_PATH.read_text(encoding="utf-8")
        cls.call = CALL_PATH.read_text(encoding="utf-8")
        cls.runtime = RUNTIME_PATH.read_text(encoding="utf-8")
        cls.bridge = BRIDGE_PATH.read_text(encoding="utf-8")

    def test_public_package_uses_the_existing_warvk_layout(self):
        self.assertFalse((WARVK_ROOT / "v1").exists())
        for path in (
            ACTION_PATH,
            CALL_PATH,
            WARVK_ROOT / "define.txt",
            JASS_PATH,
            SMOKE_JASS_PATH,
        ):
            self.assertTrue(path.is_file(), path)
        self.assertIn('#include "warvk_api.j"', self.init_jass)
        self.assertNotIn('API/warvk_render.j', self.init_jass)
        self.assertNotIn('API/warvk_lightning.j', self.init_jass)

    def test_cpp_command_table_matches_public_jass(self):
        rows = re.findall(
            r'\{CommandId::\w+,\s*"([^"]+)",\s*Carrier::(\w+),\s*'
            r'"([bird]*)",\s*[^,]+,\s*(true|false)\}',
            self.runtime,
        )
        self.assertEqual(len(rows), 55)
        cpp_commands = {
            name: (carrier, signature)
            for name, carrier, signature, _required in rows
        }
        self.assertEqual(len(cpp_commands), 55)

        self.assertNotIn("JapiFunc", self.jass)
        self.assertNotRegex(self.jass, r"(?m)^\s*native\s+WarVK")
        jass_commands = {}
        public_functions = set()
        for match in re.finditer(
            r"function\s+(WarVK\w+)\s+takes\b.*?\bendfunction",
            self.jass,
            re.DOTALL,
        ):
            function_name = match.group(1)
            block = match.group(0)
            command_match = re.search(r'"warvk:v1;([^"]+)"', block)
            if not command_match:
                continue
            command_name = command_match.group(1)
            self.assertNotIn(command_name, jass_commands)
            if "Preloader(payload)" in block:
                carrier = "Preloader"
            elif "GetLocalizedHotkey(payload)" in block:
                carrier = "Hotkey"
            elif "GetLocalizedString(payload)" in block:
                carrier = "LocalizedString"
            else:
                self.fail(f"{function_name} has no recognized carrier")
            signature = "".join(re.findall(r'";([bird]):"', block))
            jass_commands[command_name] = (carrier, signature)
            public_functions.add(function_name)

        self.assertEqual(jass_commands, cpp_commands)
        ui_scripts = set(
            re.findall(r'(?m)^script = "(WarVK\w+)"$', self.action + self.call)
        )
        self.assertEqual(ui_scripts, public_functions)

    def test_public_jass_and_ui_have_concise_descriptions(self):
        combined = self.jass + self.smoke_jass + self.action + self.call
        for banned in (
            "本地",
            "仅限本地视觉",
            "禁止用于多人同步玩法分支",
            "多人同步玩法",
            "local_visual_only",
        ):
            self.assertNotIn(banned, combined)

    def test_dxvk_is_the_single_versioned_carrier_owner(self):
        self.assertIn('#include "../japi/war3_japi_v1.h"', self.bridge)
        self.assertIn("IsVersionedPublicCommand(command)", self.bridge)
        self.assertIn("dxvk::war3::japi::Dispatch(carrier, command)", self.bridge)
        self.assertNotIn("kWarVkV1Prefix", self.bridge)
        handled_string = re.search(
            r"uint32_t __cdecl Bridge_GetLocalizedString\(uint32_t nativeArg\) \{"
            r".*?\n\}",
            self.bridge,
            re.DOTALL,
        ).group(0)
        self.assertIn("return result;", handled_string)
        self.assertNotIn(
            "return CallOriginalString(s_string, nativeArg);\n  }",
            handled_string,
        )

    def test_feature_mask_is_honest_and_unsupported_commands_fail_closed(self):
        implemented = re.search(
            r"kImplementedFeatureMask\s*=\s*(.*?);",
            self.runtime,
            re.DOTALL,
        ).group(1)
        for feature in (
            "kFeatureSun",
            "kFeatureCsm",
            "kFeaturePointLight",
            "kFeatureLightning",
            "kFeatureManagedObject",
            "kFeatureTime",
            "kFeatureStats",
        ):
            self.assertIn(feature, implemented)
        for absent in (
            "0x00000008u",
            "0x00000010u",
            "0x00000020u",
            "0x00000040u",
            "0x00000080u",
            "0x00000100u",
        ):
            self.assertNotIn(absent, implemented)
        self.assertIn("return Failure(ErrorCode::UnsupportedFeature);", self.runtime)

    def test_managed_ids_and_lightning_enable_have_real_lifetimes(self):
        self.assertIn("std::unordered_map<int32_t, ManagedObject> g_objects", self.runtime)
        self.assertIn("RegisterObject(ManagedType::PointLight", self.runtime)
        self.assertIn("RegisterObject(ManagedType::Lightning", self.runtime)
        self.assertIn("void Reset() noexcept", self.runtime)
        lightning_header = LIGHTNING_HEADER.read_text(encoding="utf-8")
        lightning_source = LIGHTNING_SOURCE.read_text(encoding="utf-8")
        self.assertIn("bool enabled = true;", lightning_header)
        self.assertIn("bool setEnabled(int32_t id, bool enabled);", lightning_header)
        self.assertIn("bool isAlive(int32_t id) const;", lightning_header)
        self.assertIn("else if (it->second.enabled)", lightning_source)

    def test_protocol_limits_and_error_contract_are_compiled_in(self):
        self.assertIn("kMaximumMessageBytes = 512u", self.runtime)
        self.assertIn("kMaximumArgumentCount = 16u", self.runtime)
        self.assertIn("payload contains a non-ASCII byte", self.runtime)
        self.assertIn("boolean token must be b:0 or b:1", self.runtime)
        self.assertIn("real token is outside the representable range", self.runtime)

    def test_carrier_install_rollback_restores_published_state(self):
        self.assertIn("uintptr_t previousEntry = 0u;", self.bridge)
        self.assertIn("uintptr_t previousOriginalFn = 0u;", self.bridge)
        self.assertIn("item.patch->entry.store(item.previousEntry", self.bridge)
        self.assertIn(
            "item.patch->originalFn.store(item.previousOriginalFn", self.bridge
        )

    def test_visual_smoke_keeps_the_light_across_a_rendered_frame(self):
        begin = re.search(
            r"function WarVKBeginPointLightSmokeTest takes.*?endfunction",
            self.smoke_jass,
            re.DOTALL,
        ).group(0)
        finish = re.search(
            r"function WarVKFinishPointLightSmokeTest takes.*?endfunction",
            self.smoke_jass,
            re.DOTALL,
        ).group(0)
        self.assertIn("returns integer", begin)
        self.assertIn("return lightId", begin)
        self.assertNotIn("WarVKDestroyPointLight", begin)
        self.assertIn("WarVKDestroyPointLight(lightId)", finish)


if __name__ == "__main__":
    unittest.main()
