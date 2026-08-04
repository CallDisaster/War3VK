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
STB_IMAGE_HEADER = ROOT / "src/MemHack/3rd/stb/stb_image.h"
RUNTIME_BOOTSTRAP = ROOT / "src/d3d9/war3/platform/war3_runtime_bootstrap.cpp"
WORLD_HOOK = ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp"
DIAGNOSTICS_HEADER = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h"
DIAGNOSTICS_SOURCE = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"


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
        self.assertFalse(
            (WARVK_ROOT / "jass/warvk_lightning_templates.j").exists(),
            "demo lightning template section must not ship in UI package",
        )
        self.assertIn('#include "warvk_api.j"', self.init_jass)
        self.assertNotIn('warvk_lightning_templates.j', self.init_jass)
        self.assertNotIn('API/warvk_render.j', self.init_jass)
        self.assertNotIn('API/warvk_lightning.j', self.init_jass)

    def test_cpp_command_table_matches_public_jass(self):
        rows = re.findall(
            r'\{CommandId::\w+,\s*"([^"]+)",\s*Carrier::(\w+),\s*'
            r'"([birds]*)",\s*[^,]+,\s*(true|false)\}',
            self.runtime,
        )
        self.assertEqual(len(rows), 80)
        cpp_commands = {
            name: (carrier, signature)
            for name, carrier, signature, _required in rows
        }
        self.assertEqual(len(cpp_commands), 80)

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
            signature = "".join(re.findall(r'";([birds]):"', block))
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

    def test_ydwe_metadata_has_matching_arguments_and_hints(self):
        for kind, source in (("action", self.action), ("call", self.call)):
            blocks = re.split(r"\n\s*\n(?=\[)", source)
            for block in blocks:
                header = re.match(r"\[([^\]]+)\]", block.strip())
                if not header:
                    continue
                name = header.group(1)
                self.assertRegex(block, r'(?m)^comment = "[^"].*"$', name)
                if kind == "action":
                    self.assertNotRegex(block, r"(?m)^returns\s*=", name)
                else:
                    self.assertNotRegex(block, r"(?m)^type\s*=\s*nothing$", name)
                args = [
                    value
                    for value in re.findall(r"(?m)^type\s*=\s*(\S+)", block)
                    if value != "nothing"
                ]
                placeholders = re.findall(r"\$\{[^}]+\}", block)
                self.assertEqual(
                    len(args),
                    len(placeholders),
                    f"{kind}:{name} metadata arity",
                )

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
            "kFeatureMathCurve",
            "kFeaturePolylineCurve",
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
        self.assertIn("int32_t createTemplate(const std::string& name);", lightning_header)
        self.assertIn("bool finalizeTemplate(int32_t templateId);", lightning_header)
        self.assertIn("else if (it->second.enabled)", lightning_source)

    def test_lightning_templates_are_jass_owned_and_frozen_before_use(self):
        for symbol in (
            "WarVKCreateLightningTemplate",
            "WarVKSetLightningTemplateBasic",
            "WarVKSetLightningTemplateAdvanced",
            "WarVKSetLightningTemplateOptional",
            "WarVKFinalizeLightningTemplate",
            "WarVKCreateLightningFromTemplate",
        ):
            self.assertIn(symbol, self.jass)
            self.assertIn(symbol, self.action + self.call)
        # Authors configure templates via public API / YDWE GUI only.
        self.assertNotIn("WarVKDefineLightningTemplates", self.jass)
        self.assertNotIn("WarVKRegisterLightningTemplatesNow", self.jass)
        self.assertIn("WarVKIsBridgeAvailable", self.jass)
        # Parameter docs must remain discoverable in JASS and YDWE comments.
        self.assertIn("startAlpha：起点不透明度，硬限制 0..1", self.jass)
        self.assertIn("averageSegmentLength：每段折线目标世界长度", self.jass)
        self.assertIn("lifetimeSec：总寿命（秒）", self.jass)
        self.assertIn("Alpha 必须 0..1", self.action)
        self.assertIn("seed=噪声/分叉随机种子", self.call)
        lightning_source = LIGHTNING_SOURCE.read_text(encoding="utf-8")
        self.assertIn("it->second.finalized", lightning_source)
        self.assertIn("IsLightningTexturePath", self.runtime)
        self.assertIn("DecodeBlp1Jpeg", lightning_source)
        self.assertIn("DecodeStbImage", lightning_source)
        self.assertIn("ApplyBlp1JpegSoftAlphaFromBorder", lightning_source)
        jpeg_decode = re.search(
            r"bool DecodeBlp1Jpeg\(.*?\n\}", lightning_source, re.DOTALL
        ).group(0)
        self.assertIn("ApplyBlp1JpegSoftAlphaFromBorder(width, height, argb)",
                      jpeg_decode)
        self.assertIn("sourceAlpha", lightning_source)
        self.assertIn("hasAuthoredAlpha", lightning_source)
        stb_image = STB_IMAGE_HEADER.read_text(encoding="utf-8")
        self.assertIn("is_bgra_jfif = z->s->img_n == 4", stb_image)
        self.assertIn("out[0] = coutput[2][i]", stb_image)
        self.assertIn("out[1] = coutput[1][i]", stb_image)
        self.assertIn("out[2] = coutput[0][i]", stb_image)
        self.assertIn("out[3] = coutput[3][i]", stb_image)
        self.assertIn("record.branchLengthScale <= 0.0f", lightning_source)
        self.assertIn("WarVKMakeLightningTemplateWireName", self.jass)
        self.assertIn('return "T" + I2S(StringHash(name))', self.jass)
        self.assertIn("isLegacyLocalizedTemplateName", self.runtime)

    def test_lightning_runtime_status_exposes_creation_draw_and_texture_state(self):
        diagnostics_header = DIAGNOSTICS_HEADER.read_text(encoding="utf-8")
        diagnostics_source = DIAGNOSTICS_SOURCE.read_text(encoding="utf-8")
        self.assertIn("struct War3RuntimeStatusLightningSnapshot", diagnostics_header)
        self.assertIn("War3RuntimeStatusLightningSnapshot lightning", diagnostics_header)
        for field in (
            "templateCount",
            "finalizedTemplateCount",
            "drawAttemptCount",
            "drawSuccessCount",
            "textureLoadFallbackCount",
            "hasDevice",
        ):
            self.assertIn(field, diagnostics_header)
            self.assertIn(f'{{"{field}", snapshot.lightning.{field}}}', diagnostics_source)
        self.assertIn("War3LightningRuntime::instance().snapshot()", diagnostics_source)

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
        lightning_begin = re.search(
            r"function WarVKBeginLightningTemplateSmokeTest takes.*?endfunction",
            self.smoke_jass,
            re.DOTALL,
        ).group(0)
        self.assertIn("WarVKCreateLightningTemplate", lightning_begin)
        self.assertIn("WarVKSetLightningTemplateBasic", lightning_begin)
        self.assertIn("WarVKSetLightningTemplateAdvanced", lightning_begin)
        self.assertIn("WarVKSetLightningTemplateOptional", lightning_begin)
        self.assertIn("WarVKFinalizeLightningTemplate", lightning_begin)
        self.assertIn("WarVKCreateLightningFromTemplate", lightning_begin)

    def test_lightning_has_one_gated_world_stage_execution_path(self):
        bootstrap = RUNTIME_BOOTSTRAP.read_text(encoding="utf-8")
        world_hook = WORLD_HOOK.read_text(encoding="utf-8")
        execute_helper = re.search(
            r"bool ExecuteNativeShadowBackendPreparedFrame\(\) \{.*?\n\}",
            bootstrap,
            re.DOTALL,
        ).group(0)
        self.assertNotIn("War3LightningRuntime", execute_helper)
        lightning_hook = re.search(
            r"static void TryWarVkLightningWorldStageDraw\(.*?\n\}",
            world_hook,
            re.DOTALL,
        ).group(0)
        self.assertIn("if (a5 != 0)", lightning_hook)
        self.assertIn("lightning.executePreparedFrame();", lightning_hook)

    def test_lightning_branch_anchors_share_parent_curve_and_uv_phase(self):
        lightning_source = LIGHTNING_SOURCE.read_text(encoding="utf-8")
        self.assertIn("War3LightningPoint ResolveRibbonCenterPoint", lightning_source)
        self.assertIn("War3LightningPoint ResolveRibbonLocalSide", lightning_source)
        branch_begin = lightning_source.index("const auto parentSide =")
        branch_end = lightning_source.index("if (texture != nullptr)", branch_begin)
        branch = lightning_source[branch_begin:branch_end]
        self.assertIn(
            "ResolveRibbonCenterPoint(\n"
            "          record, record.start, record.end, parentSide, t, nowSec, 0u,\n"
            "          segmentCount)",
            branch,
        )
        self.assertIn("ResolveRibbonLocalSide(", branch)
        self.assertIn("b + 1u, t * record.uvTiling, vertices", branch)
        self.assertNotIn("const War3LightningPoint base = Lerp(", branch)

    def test_lightning_draw_isolates_blend_operation_and_color_writes(self):
        lightning_source = LIGHTNING_SOURCE.read_text(encoding="utf-8")
        draw_ribbon = re.search(
            r"bool DrawRibbon\(.*?\n\}", lightning_source, re.DOTALL
        ).group(0)
        self.assertIn("D3DRS_BLENDOP, D3DBLENDOP_ADD", draw_ribbon)
        self.assertIn("D3DRS_SEPARATEALPHABLENDENABLE, FALSE", draw_ribbon)
        self.assertIn("D3DRS_COLORWRITEENABLE, 0x0fu", draw_ribbon)
        self.assertIn("D3DRS_SRGBWRITEENABLE, FALSE", draw_ribbon)
        self.assertIn("D3DRS_FOGENABLE, FALSE", draw_ribbon)


if __name__ == "__main__":
    unittest.main()
