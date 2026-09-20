"""Source contracts; native carrier execution and pixels need separate runtime gates."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


class VisualApiContracts(unittest.TestCase):
    def test_sun_off_only_zeros_native_main_directional(self):
        device = read("src/d3d9/d3d9_device.cpp")
        light = device[device.index("D3D9DeviceEx::SetLight("):device.index("D3D9DeviceEx::GetLight(")]
        self.assertIn("Index == 0 && m_war3Pipeline", light)
        off = light[light.index("if (!sun.enabled)"):light.index("// Check if settings")]
        self.assertIn("if (finalLight.Type == D3DLIGHT_DIRECTIONAL)", off)
        self.assertIn("ApplyWar3DisabledSun(finalLight)", off)
        self.assertIn("m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData)", off)

    def test_policy_preserves_indirect_and_other_fields(self):
        policy = read("src/d3d9/war3/render/war3_sun_light_policy.h")
        helper = policy[policy.index("inline void ApplyWar3DisabledSun"):policy.index("inline float")]
        self.assertEqual(helper.count("light."), 6)
        self.assertNotIn("Ambient", helper)
        self.assertNotIn("Direction", helper)
        self.assertIn("std::isfinite(intensity)", policy)

    def test_custom_material_and_normal_shadow_obey_sun(self):
        self.assertIn("War3SunDirectIntensity(\n            settings->sun.enabled", read("src/d3d9/d3d9_device.cpp"))
        shadow = read("src/d3d9/d3d9_war3_shadow.cpp")
        self.assertEqual(shadow.count("shadowsEnabled && mutableSettings.sun.enabled"), 2)
        self.assertIn("(mutableSettings.sun.enabled &&\n       mutableSettings.shadows.strength", shadow)
        self.assertIn("m_pointLightsEnabled = settings->shadows.pointLightsEnabled", shadow)

    def test_wire_and_feature_mask_remain_compatible(self):
        japi = read("src/d3d9/war3/japi/war3_japi_v1.cpp")
        self.assertIn('"sun.setEnabled", Carrier::Preloader, "b", kFeatureSun, true', japi)
        self.assertIn("settings->sun.enabled = a[0].boolean", japi)
        self.assertIn("return Failure(ErrorCode::UnsupportedFeature)", japi)

    def test_test_carriers_cannot_forward_paths_or_embedded_nulls(self):
        bridge = read("src/d3d9/war3/hooks/war3_jass_command_bridge.cpp")
        test = bridge[bridge.index("JassPublicApiTestResult InvokeJassPublicApiForTest"):bridge.index("JassCommandBridgeSelfTestResult RunJassCommandBridgeSelfTest")]
        for text in ('payload.size() > 2048u', '"warvk:v1;"', "payload.find('\\0')", 'carrier != "Preloader"', 'carrier != "GetLocalizedHotkey"', 'carrier != "GetLocalizedString"', 'pre != &Bridge_Preloader', 'hot != &Bridge_GetLocalizedHotkey', 'str != &Bridge_GetLocalizedString'):
            self.assertIn(text, test)
        self.assertNotIn("japi::Dispatch(", test)

    def test_internal_test_queue_and_evidence_do_not_claim_jass_execution(self):
        test = read("src/d3d9/war3/tools/war3_internal_test_api.cpp")
        self.assertIn("if (!IsInternalTestApiEnabled())", test)
        self.assertIn("native-carrier-not-jass-bytecode", test)
        self.assertIn("author-pending-not-gpu-completion", test)
        self.assertIn('response["apiAccepted"] = test.invoked && test.errorCode == 0', test)
        self.assertIn("GetSettingsSnapshot(settings)", test)

    def test_execution_diagnostics_are_opt_in_and_record_actual_backend(self):
        cpp = read("src/d3d9/d3d9_war3_volumetric_light.cpp")
        self.assertIn('env::getEnvVar("DXVK_WAR3_VISUAL_API_DIAGNOSTICS") == "1"', cpp)
        witness = cpp[cpp.index("class VolumetricExecutionWitness"):cpp.index("class VolumetricExecutionWitness") + 700]
        self.assertLess(witness.index("if (!enabled)"), witness.index("std::lock_guard"))
        self.assertIn("outEffectiveBackend = static_cast<int32_t>(settings.quality)", cpp)
        self.assertIn("outEffectiveBackend = 0;", cpp)
        self.assertIn("execution.compositeSubmitted = true", cpp)
        self.assertNotIn("active backend=%u", cpp)
        self.assertIn('execution.stage = "waiting-for-csm"', cpp)
        self.assertIn("execution.mapEpoch = input.mapEpoch", cpp)

    def test_no_budget_relaxation_or_shader_change(self):
        cpp = read("src/d3d9/d3d9_war3_volumetric_light.cpp")
        for text in ("kVolumetricRaySegmentBudget = 4'000'000ull", "kVolumetricFogSegmentTestBudget = 96'000'000ull", "if (!workEstimate.accepted)"):
            self.assertIn(text, cpp)
        self.assertIn("maxTotalWork = 350ull * 1024ull * 1024ull", read("src/d3d9/war3/render/war3_volumetric_shader_work_admission.h"))


if __name__ == "__main__":
    unittest.main()
