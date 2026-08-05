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
        cls.define = (WARVK_ROOT / "define.txt").read_text(encoding="utf-8")
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
        self.assertEqual(len(rows), 91)
        cpp_commands = {
            name: (carrier, signature)
            for name, carrier, signature, _required in rows
        }
        self.assertEqual(len(cpp_commands), 91)

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
        self.assertTrue(ui_scripts.issubset(public_functions))
        for hidden_until_implemented in (
            "WarVKSetOutlineEnabled",
            "WarVKSetOutlineColor",
            "WarVKSetOutlineParameters",
            "WarVKSetBloomEnabled",
            "WarVKSetBloomParameters",
            "WarVKSetBloomRadius",
            "WarVKSetPostfxEnabled",
            "WarVKSetPostfxExposureGamma",
            "WarVKSetPostfxColorGrade",
            "WarVKSetAaMode",
            "WarVKSetAaSharpness",
            "WarVKSetDayNightEnabled",
            "WarVKSetDayNightTime",
            "WarVKSetDayNightSpeed",
        ):
            self.assertIn(hidden_until_implemented, public_functions)
            self.assertNotIn(hidden_until_implemented, ui_scripts)

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
            "kFeatureVolumetric",
            "kFeatureDayNight",
            "kFeatureLightning",
            "kFeatureManagedObject",
            "kFeatureTime",
            "kFeatureStats",
            "kFeatureMathCurve",
            "kFeaturePolylineCurve",
        ):
            self.assertIn(feature, implemented)
        for absent in (
            "0x00000010u",
            "0x00000020u",
            "0x00000040u",
            "0x00000080u",
        ):
            self.assertNotIn(absent, implemented)
        self.assertIn("return Failure(ErrorCode::UnsupportedFeature);", self.runtime)

    def test_ydwe_categories_are_specific_and_root_is_system_only(self):
        for category in (
            "TC_WarVKDiagnostics",
            "TC_WarVKSunShadow",
            "TC_WarVKLightingClock",
            "TC_WarVKPointLight",
            "TC_WarVKVolumetricLight",
            "TC_WarVKVolumetricFog",
            "TC_WarVKLightning",
            "TC_WarVKLightningTemplate",
            "TC_WarVKMath",
            "TC_WarVKCurve",
        ):
            self.assertIn(f"{category}=[", self.define)
        self.assertNotIn("\\\\CommandButtons\\\\", self.define)
        root_scripts = set()
        for block in re.split(r"\n\s*\n(?=\[)", self.action + "\n\n" + self.call):
            if 'category = "TC_WarVK"' in block:
                match = re.search(r'(?m)^script = "(WarVK\w+)"$', block)
                if match:
                    root_scripts.add(match.group(1))
        self.assertEqual(root_scripts, {
            "WarVKGetVersion",
            "WarVKGetProtocolVersion",
            "WarVKGetFeatureFlags",
            "WarVKIsRuntimeReady",
        })

    def test_ydwe_enumerations_are_clickable_trigger_types(self):
        custom_types = (
            "WarVKCsmCascadeCount",
            "WarVKPointShadowResolution",
            "WarVKLightingClockMode",
            "WarVKLightningRenderMode",
            "WarVKCurveCoordinateMode",
            "WarVKCurveComponent",
            "WarVKMathRoundingMode",
        )
        for type_name in custom_types:
            self.assertRegex(
                self.define,
                rf"(?m)^{type_name}=0,0,0,[^,]+,integer$",
            )

        symbolic_params = {
            "WarVKLightingClockMode": (
                "WARVK_LIGHTING_CLOCK_GAME_TIME",
                "WARVK_LIGHTING_CLOCK_HELD",
                "WARVK_LIGHTING_CLOCK_INDEPENDENT",
            ),
            "WarVKLightningRenderMode": (
                "WARVK_LIGHTNING_RENDER_ALPHA_NO_DEPTH",
                "WARVK_LIGHTNING_RENDER_ALPHA_DEPTH",
                "WARVK_LIGHTNING_RENDER_ADDITIVE_NO_DEPTH",
                "WARVK_LIGHTNING_RENDER_ADDITIVE_DEPTH",
            ),
            "WarVKCurveCoordinateMode": (
                "WARVK_CURVE_COORDINATE_OFFSET",
                "WARVK_CURVE_COORDINATE_LOCAL",
                "WARVK_CURVE_COORDINATE_WORLD",
            ),
            "WarVKCurveComponent": (
                "WARVK_CURVE_COMPONENT_X",
                "WARVK_CURVE_COMPONENT_Y",
                "WARVK_CURVE_COMPONENT_Z",
            ),
            "WarVKMathRoundingMode": (
                "WARVK_MATH_ROUND_NEAREST",
                "WARVK_MATH_ROUND_FLOOR",
                "WARVK_MATH_ROUND_CEIL",
                "WARVK_MATH_ROUND_TRUNCATE",
            ),
        }
        for type_name, symbols in symbolic_params.items():
            for symbol in symbols:
                self.assertRegex(
                    self.define,
                    rf'(?m)^{symbol}=0,{type_name},"{symbol}",[^,]+$',
                )

        def argument_types(source, function_name):
            block = re.search(
                rf"(?ms)^\[{function_name}\]\n(.*?)(?=^\[(?!\[)|\Z)", source
            ).group(1)
            return re.findall(r"(?m)^type\s*=\s*(\S+)$", block)

        expected = {
            "WarVKSetCsmLayout": (self.action, 0, "WarVKCsmCascadeCount"),
            "WarVKSetLightingClockMode": (
                self.action, 0, "WarVKLightingClockMode"
            ),
            "WarVKSetPointLightShadowConfig": (
                self.action, 1, "WarVKPointShadowResolution"
            ),
            "WarVKSetLightningTemplateBasic": (
                self.action, 14, "WarVKLightningRenderMode"
            ),
            "WarVKSetCurveCoordinateMode": (
                self.action, 1, "WarVKCurveCoordinateMode"
            ),
            "WarVKEvaluateMathInteger": (
                self.call, 4, "WarVKMathRoundingMode"
            ),
            "WarVKEvaluateCurveComponent": (
                self.call, 1, "WarVKCurveComponent"
            ),
            "WarVKEvaluateCurveDerivativeComponent": (
                self.call, 1, "WarVKCurveComponent"
            ),
        }
        for function_name, (source, index, type_name) in expected.items():
            self.assertEqual(argument_types(source, function_name)[index], type_name)

        self.assertNotIn(
            "WarVKCurveCoordinateMode",
            argument_types(self.action, "WarVKSetLightningTemplateAdvanced"),
        )

    def test_volumetric_and_scalar_author_controls_are_real_commands(self):
        for command in (
            "volumetric.setEnabled",
            "volumetric.setDensity",
            "volumetric.setScattering",
            "volumetric.setQuality",
            "volumetricFog.setEnabled",
            "volumetricFog.setSettings",
            "math.evaluateReal",
            "math.evaluateInteger",
        ):
            self.assertIn(f'"{command}"', self.runtime)
        for dispatch in (
            "case CommandId::VolumetricSetEnabled",
            "case CommandId::VolumetricFogSetEnabled",
            "case CommandId::VolumetricFogSetSettings",
            "case CommandId::MathEvaluateReal",
            "case CommandId::MathEvaluateInteger",
        ):
            self.assertIn(dispatch, self.runtime)
        for script in (
            "WarVKSetGlobalVolumetricFogEnabled",
            "WarVKSetGlobalVolumetricFog",
            "WarVKEvaluateMathReal",
            "WarVKEvaluateMathInteger",
        ):
            self.assertIn(f"function {script}", self.jass)
            self.assertIn(f'script = "{script}"', self.action + self.call)

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
