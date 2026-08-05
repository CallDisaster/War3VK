import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
PERF_CPP = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


class ShadowAlphaBlendFailClosedStaticTest(unittest.TestCase):
    def test_alpha_blend_rejection_is_default_on_and_independent(self):
        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER", 1u)',
            DEVICE_CPP,
        )

    def test_default_policy_does_not_resolve_material_during_preselection(self):
        start = DEVICE_CPP.index(
            "bool War3CurrentDrawRecordIsUnsafeAlphaCaster("
        )
        end = DEVICE_CPP.index(
            "bool War3RejectCurrentDrawRecordByUnsafeAlphaVisualPolicy(", start
        )
        block = DEVICE_CPP[start:end]
        self.assertIn("if (!rejectUnsafeAlpha)\n    return false;", block)
        self.assertNotIn("War3SemanticRejectAlphaBlendCasterRuntime()", block)
        self.assertLess(
            block.index("if (!rejectUnsafeAlpha)"),
            block.index("War3TryBuildCurrentDrawRecordMaterialSignature"),
        )

    def test_canonical_append_has_a_second_fail_closed_guard(self):
        self.assertIn(
            "if (War3SemanticRejectAlphaBlendCasterRuntime() &&\n"
            "      canonicalMaterial.alphaMode ==\n"
            "          dxvk::war3::shadow::ShadowAlphaMode::AlphaBlend &&\n"
            "      semanticAlphaPayloadState != 5u)",
            DEVICE_CPP,
        )
        self.assertIn(
            "semanticSceneRejectedAlphaBlendVisualPolicy++", DEVICE_CPP
        )
        self.assertIn("semanticSceneRejectedAlphaBlendGeometry++", DEVICE_CPP)

    def test_cutout_payload_path_remains_available(self):
        self.assertIn(
            "material.alphaMode == dxvk::war3::shadow::ShadowAlphaMode::Cutout ||",
            DEVICE_CPP,
        )
        self.assertIn("War3ShadowDrawMetadataStore().lookupAlpha(", DEVICE_CPP)
        self.assertIn("semanticAlphaPayloadState == 5u", DEVICE_CPP)
        self.assertIn("draw.alphaRef = semanticAlphaPayload.alphaRef;", DEVICE_CPP)
        self.assertIn("candidate.alphaTestEnabled = false;", DEVICE_CPP)
        self.assertNotIn(
            "candidate.uvStorage = semanticAlphaPayload.uvStorage", DEVICE_CPP
        )

    def test_runtime_choice_is_reported(self):
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER"', PERF_CPP
        )


if __name__ == "__main__":
    unittest.main()
