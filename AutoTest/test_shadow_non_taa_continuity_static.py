#!/usr/bin/env python3
"""Static contracts for non-temporal CSM receiver continuity."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
RECEIVER = (
    ROOT / "subprojects" / "war3fx" / "shaders" / "war3_shadow_receiver.frag"
)
VISIBILITY = (
    ROOT / "subprojects" / "war3fx" / "shaders" / "war3_shadow_visibility.frag"
)
CSM_H = ROOT / "src" / "d3d9" / "d3d9_war3_csm.h"
CSM_CPP = ROOT / "src" / "d3d9" / "d3d9_war3_csm.cpp"
SHADOW_H = ROOT / "src" / "d3d9" / "d3d9_war3_shadow.h"
SHADOW_CPP = ROOT / "src" / "d3d9" / "d3d9_war3_shadow.cpp"
BRIDGE_H = (
    ROOT / "src" / "d3d9" / "war3" / "render"
    / "war3_shadow_runtime_bridge.h"
)
FRAME_CAPTURE_H = (
    ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_frame_capture.h"
)
PERF_H = ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_perf_monitor.h"
PERF_CPP = (
    ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_perf_monitor.cpp"
)


class ShadowNonTaaContinuityStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.receiver = RECEIVER.read_text(encoding="utf-8")
        cls.visibility = VISIBILITY.read_text(encoding="utf-8")
        cls.csm_h = CSM_H.read_text(encoding="utf-8")
        cls.csm_cpp = CSM_CPP.read_text(encoding="utf-8")
        cls.shadow_h = SHADOW_H.read_text(encoding="utf-8")
        cls.shadow_cpp = SHADOW_CPP.read_text(encoding="utf-8")
        cls.bridge_h = BRIDGE_H.read_text(encoding="utf-8")
        cls.frame_capture_h = FRAME_CAPTURE_H.read_text(encoding="utf-8")
        cls.perf_h = PERF_H.read_text(encoding="utf-8")
        cls.perf_cpp = PERF_CPP.read_text(encoding="utf-8")

    def test_stable_wall_does_not_return_before_cascade_blend(self) -> None:
        forbidden = re.compile(
            r"if\s*\(\s*stableWallPath\s*\)\s*"
            r"return\s+sampleShadowStableWall",
            re.MULTILINE,
        )
        self.assertIsNone(forbidden.search(self.receiver))
        self.assertIsNone(forbidden.search(self.visibility))

    def test_stable_wall_uses_continuous_filter_parameters(self) -> None:
        for source in (self.receiver, self.visibility):
            self.assertNotIn("sampleShadowStableWall", source)
            self.assertNotIn("snappedUv", source)
            self.assertIn("wallFilterWeight", source)
            self.assertIn(
                "mix(radius0, max(radius0, 1.50), wallFilterWeight)", source
            )
            self.assertIn("return mix(vis0, vis1, w);", source)

    def test_non_taa_release_default_has_no_periodic_rotation(self) -> None:
        start = self.receiver.index("vec2 pcfRot = vec2(1.0, 0.0);")
        end = self.receiver.index("float vis = 1.0;", start)
        rotation_block = self.receiver[start:end]
        self.assertNotIn("dot(worldPos.xy", rotation_block)
        self.assertNotIn("dot(vec2(pix)", rotation_block)
        self.assertNotIn("fract(", rotation_block)

    def test_csm_publishes_exact_snap_and_texel_diagnostics(self) -> None:
        for token in ("snappedCenterLightSpace", "texelSize"):
            self.assertIn(token, self.csm_h)
            self.assertIn(token, self.csm_cpp)
        for token in (
            "MaxCsmSnappedCenterDeltaTexels",
            "MaxCsmTexelSizeDelta",
            "receiverSunDeltaNano",
            "receiverSnappedCenterDeltaTexelsNano",
            "receiverTexelSizeDeltaNano",
        ):
            self.assertIn(token, self.shadow_cpp)
        for token in (
            "receiverCameraDeltaNano",
            "receiverSunDeltaNano",
            "receiverCsmDeltaNano",
            "receiverSnappedCenterDeltaTexelsNano",
            "receiverTexelSizeDeltaNano",
            "shadowMapRenderSerial",
        ):
            self.assertIn(token, self.bridge_h)
            self.assertIn(token, self.frame_capture_h)
            self.assertIn(token, self.perf_h)
            self.assertIn(f'\\"{token}\\"', self.perf_cpp)

    def test_adaptive_reuse_requires_exact_content_csm_and_generation(self) -> None:
        start = self.shadow_cpp.index("bool reuseLastShadowMap = false;")
        end = self.shadow_cpp.index(
            "if (holdForInvalidCsm || holdForTransientEmptyReplay", start
        )
        adaptive = self.shadow_cpp[start:end]
        for token in (
            "exactReplayContractStable",
            "stagePolicyStable",
            "exactCsmContractStable",
            "shadowMapGenerationStable",
            "dynamicContentStable",
        ):
            self.assertIn(token, adaptive)
        self.assertNotIn(
            "kShadowAdaptiveMapUpdateCameraMaxDelta", adaptive
        )
        for token in (
            "m_lastShadowMapReplayContentHash",
            "m_lastShadowMapReplayBackingHash",
            "m_lastShadowMapStagePolicyRevision",
            "m_lastShadowMapCsmHash",
            "m_lastShadowMapResourceGeneration",
        ):
            self.assertIn(token, self.shadow_h)
            self.assertIn(token, self.shadow_cpp)

    def test_lifecycle_transition_revokes_all_shadow_continuity(self) -> None:
        start = self.shadow_cpp.index("if (lifecycleInvalidated) {")
        end = self.shadow_cpp.index(
            "const bool shadowTaaRuntimeModuleEnabled", start
        )
        invalidation = self.shadow_cpp[start:end]
        for token in (
            "m_hasCompleteShadowMap = false",
            "m_shadowHistoryValid = false",
            "m_shadowTaaHistoryContractValid = false",
            "m_transientEmptyReplayHoldFramesRemaining = 0u",
            "m_recentSemanticDynamicHoldFramesRemaining = 0u",
            "m_semanticIdentityChurnHoldFramesRemaining = 0u",
            "m_semanticCoverageDropHoldStreak = 0u",
            "m_lastShadowMapCsmHash = 0u",
            "m_lastShadowMapResourceGeneration = 0u",
        ):
            self.assertIn(token, invalidation)


if __name__ == "__main__":
    unittest.main()
