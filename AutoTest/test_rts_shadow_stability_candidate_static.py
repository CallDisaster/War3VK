#!/usr/bin/env python3
"""Release isolation and integration contracts for the RTS CSM candidate."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_rts_shadow_stability_contract.h"
).read_text(encoding="utf-8")
CSM_H = (ROOT / "src/d3d9/d3d9_war3_csm.h").read_text(encoding="utf-8")
CSM_CPP = (ROOT / "src/d3d9/d3d9_war3_csm.cpp").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")


class RtsShadowStabilityCandidateStaticTests(unittest.TestCase):
    def test_release_build_does_not_enable_candidate(self):
        self.assertIn("option('warvk_rts_shadow_candidate_dev'", OPTIONS)
        option = OPTIONS[OPTIONS.index("option('warvk_rts_shadow_candidate_dev'") :]
        self.assertIn("value : false", option.split(")", 1)[0])
        self.assertIn("get_option('warvk_rts_shadow_candidate_dev')", MESON)
        self.assertIn("-DWARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV=1", MESON)
        self.assertIn("kDevelopmentRtsShadowCandidateEnabled = false", POLICY)
        self.assertIn("WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV", SHADOW)
        self.assertNotIn("WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV", CSM_CPP)

    def test_only_explicit_mode_one_can_select_receiver_band(self):
        self.assertIn("configuredMode == 1u", POLICY)
        self.assertIn("War3RtsShadowCandidateMode::ReceiverBand", POLICY)
        self.assertIn("War3RtsShadowCandidateMode::Off", POLICY)
        self.assertIn("DXVK_WAR3_RTS_SHADOW_CANDIDATE_MODE", SHADOW)
        self.assertIn("m_csmConfig.fitMode = War3CsmFitMode::RtsReceiverBand", SHADOW)
        self.assertIn("War3CsmFitMode fitMode = War3CsmFitMode::StableSphere", CSM_H)

    def test_fixed_density_fit_is_fail_closed(self):
        self.assertIn("War3FitRtsShadowReceiverBand", POLICY)
        self.assertIn("query.worldTexelSize *", POLICY)
        self.assertIn("double(query.shadowResolution)", POLICY)
        self.assertIn("std::round(centerX / query.worldTexelSize)", POLICY)
        self.assertIn("result.densitySatisfied", POLICY)
        self.assertIn("result.valid = result.densitySatisfied", POLICY)
        self.assertIn("if (rtsFit.valid)", CSM_CPP)
        self.assertIn("radius = float(rtsFit.halfExtent)", CSM_CPP)
        self.assertIn("else if (config.fitMode != War3CsmFitMode::TightAabb)", CSM_CPP)

    def test_candidate_does_not_touch_filter_or_shader_abi(self):
        for forbidden in (
            "Poisson",
            "hashed",
            "TAA",
            "ShadowCasterPushConstants",
            "descriptor",
            "pointShadow",
        ):
            self.assertNotIn(forbidden, POLICY)


if __name__ == "__main__":
    unittest.main()
