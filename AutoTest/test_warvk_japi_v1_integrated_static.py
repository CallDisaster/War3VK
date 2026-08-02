import hashlib
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "WarVK/v1/manifest/warvk_v1.json"
JASS_PATH = ROOT / "WarVK/v1/jass/warvk_v1.j"
SMOKE_JASS_PATH = ROOT / "WarVK/v1/jass/warvk_v1_smoke_test.j"
RUNTIME_PATH = ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp"
BRIDGE_PATH = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
LIGHTNING_HEADER = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.h"
LIGHTNING_SOURCE = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.cpp"


class WarVKJapiV1IntegratedStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest_bytes = MANIFEST_PATH.read_bytes()
        cls.manifest = json.loads(cls.manifest_bytes)
        cls.jass = JASS_PATH.read_text(encoding="utf-8")
        cls.smoke_jass = SMOKE_JASS_PATH.read_text(encoding="utf-8")
        cls.runtime = RUNTIME_PATH.read_text(encoding="utf-8")
        cls.bridge = BRIDGE_PATH.read_text(encoding="utf-8")

    def test_manifest_is_the_exact_clean_room_source(self):
        self.assertEqual(
            hashlib.sha256(self.manifest_bytes).hexdigest().upper(),
            "920872221B3836A5EFF69D3EC721915B21E0C4B5399C0F09F05B028CF46D27BF",
        )
        self.assertEqual(len(self.manifest["commands"]), 55)
        self.assertFalse(
            self.manifest["provenance"]["restrictedImplementationConsulted"]
        )

    def test_cpp_command_table_matches_manifest(self):
        rows = re.findall(
            r'\{CommandId::\w+,\s*"([^"]+)",\s*Carrier::(\w+),\s*'
            r'"([bird]*)",\s*[^,]+,\s*(true|false)\}',
            self.runtime,
        )
        self.assertEqual(len(rows), 55)
        by_name = {
            name: (carrier, signature, required == "true")
            for name, carrier, signature, required in rows
        }
        carrier_names = {
            "preloader": "Preloader",
            "hotkey": "Hotkey",
            "localized_string": "LocalizedString",
        }
        type_codes = {"bool": "b", "i32": "i", "id": "d", "real": "r"}
        for command in self.manifest["commands"]:
            self.assertIn(command["name"], by_name)
            carrier, signature, required = by_name[command["name"]]
            self.assertEqual(carrier, carrier_names[command["carrier"]])
            self.assertEqual(
                signature,
                "".join(type_codes[arg["type"]] for arg in command["args"]),
            )
            self.assertEqual(required, command["backendRequired"])

    def test_all_public_jass_wrappers_match_manifest_carriers(self):
        self.assertNotIn("JapiFunc", self.jass)
        self.assertNotRegex(self.jass, r"(?m)^\s*native\s+WarVK")
        for command in self.manifest["commands"]:
            block_match = re.search(
                rf"function\s+{re.escape(command['jassName'])}\s+takes\b"
                rf".*?\bendfunction",
                self.jass,
                re.DOTALL,
            )
            self.assertIsNotNone(block_match, command["jassName"])
            block = block_match.group(0)
            self.assertIn(f"warvk:v1;{command['name']}", block)
            expected_native = {
                "preloader": "Preloader(payload)",
                "hotkey": "GetLocalizedHotkey(payload)",
                "localized_string": "GetLocalizedString(payload)",
            }[command["carrier"]]
            self.assertIn(expected_native, block)

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
