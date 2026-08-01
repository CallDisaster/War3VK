"""Static contracts for the isolated union-consumer visibility policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/render/war3_union_consumer_visibility.h"
SOURCE = ROOT / "src/d3d9/war3/render/war3_union_consumer_visibility.cpp"
UNIT = (
    ROOT
    / "src/d3d9/war3/render/tests/war3_union_consumer_visibility_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


class UnionConsumerVisibilityContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.unit = UNIT.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_modes_consumers_and_generation_proofs_are_explicit(self) -> None:
        for token in (
            "Off = 0u",
            "Observe = 1u",
            "Consume = 2u",
            "War3UnionConsumerMain",
            "War3UnionConsumerCsm0",
            "War3UnionConsumerCsm1",
            "War3UnionConsumerCsm2",
            "War3UnionConsumerCsm3",
            "War3UnionConsumerPointShadow",
            "War3UnionConsumerOutline",
            "candidateFrameGeneration",
            "boundsFrameGeneration",
            "cameraFrameGeneration",
            "consumerStateFrameGeneration",
            "resourceGeneration",
            "expectedResourceGeneration",
        ):
            self.assertIn(token, self.header)

    def test_fail_visible_is_the_default_and_observe_cannot_consume(self) -> None:
        self.assertIn("bool failVisible = true", self.header)
        self.assertIn("bool consumeAdmissionGranted = false", self.header)
        start = self.source.index("War3UnionMakeFailVisibleDecision(")
        end = self.source.index("War3EvaluateConservativeCsmSphere(", start)
        block = self.source[start:end]
        self.assertIn("predictedVisibleMask = requestedMask", block)
        self.assertIn("effectiveVisibleMask = requestedMask", block)
        self.assertIn("result.failVisible = true", block)
        self.assertIn(
            "query.mode == War3UnionVisibilityMode::Consume", self.source
        )
        self.assertIn("query.consumeAdmissionGranted", self.source)
        self.assertIn(
            "result.effectiveVisibleMask = result.predictedVisibleMask",
            self.source,
        )

    def test_only_far_static_rigid_exact_current_input_can_be_culled(self) -> None:
        for token in (
            "query.cascadeIndex < 2u",
            "query.dynamic || query.skinned",
            "!query.staticRigidProven",
            "!query.identityKnown",
            "!query.exactCurrentFrameSource",
            "generation.candidateFrameGeneration !=",
            "generation.boundsFrameGeneration !=",
            "generation.cameraFrameGeneration !=",
            "generation.consumerStateFrameGeneration !=",
            "generation.resourceGeneration !=",
        ):
            self.assertIn(token, self.source)
        outside = self.source.index("const bool outside =")
        self.assertLess(self.source.index("query.cascadeIndex < 2u"), outside)
        self.assertLess(self.source.index("query.dynamic || query.skinned"), outside)

    def test_non_finite_or_degenerate_math_never_rejects_a_consumer(self) -> None:
        for token in (
            "War3UnionIsFiniteBounds",
            "War3UnionIsFiniteMatrix",
            "std::isfinite(absW)",
            "kMinimumClipW",
            "NonFiniteProjection",
            "InvalidGuardBand",
        ):
            self.assertIn(token, self.source)
        proof = self.source.index("result.proofBits |= War3UnionProofOutside")
        effective = self.source.index(
            "result.effectiveVisibleMask = result.predictedVisibleMask"
        )
        self.assertLess(proof, effective)

    def test_module_is_built_and_has_a_runnable_meson_test(self) -> None:
        self.assertIn(
            "war3/render/war3_union_consumer_visibility.cpp", self.meson
        )
        self.assertIn("war3_union_consumer_visibility_test", self.meson)
        self.assertIn("War3EvaluateConservativeCsmSphere", self.unit)
        self.assertIn("int main()", self.unit)

    def test_runtime_integration_remains_observe_only(self) -> None:
        include = '#include "war3/render/war3_union_consumer_visibility.h"'
        self.assertNotIn(include, DEVICE.read_text(encoding="utf-8"))
        shadow = SHADOW.read_text(encoding="utf-8")
        self.assertIn(include, shadow)
        self.assertIn("DXVK_WAR3_UNION_CONSUMER_CULL_MODE", shadow)
        self.assertIn("query.consumeAdmissionGranted = false", shadow)
        self.assertNotIn("query.consumeAdmissionGranted = true", shadow)
        self.assertNotIn("decision.effectiveVisibleMask", shadow)
        self.assertIn("unionCullFalseNegativeCount", shadow)
        self.assertIn("War3WorldCameraIsFreshForFrame", shadow)
        self.assertIn("War3ShadowPartLifecycleState::RequiredCurrent", shadow)


if __name__ == "__main__":
    unittest.main()
