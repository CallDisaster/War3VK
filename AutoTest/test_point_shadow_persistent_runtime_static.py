"""Production contracts for the opt-in point-shadow persistent worker."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SOURCE = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
PLANNER = ROOT / "src/d3d9/war3/render/war3_point_shadow_cpu_plan.h"
MESON = ROOT / "src/d3d9/meson.build"


class PointShadowPersistentRuntimeContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.planner = PLANNER.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_release_default_is_off_and_thread_is_lazy(self) -> None:
        self.assertIn("kWar3PointShadowCpuPlanRuntimeDefaultEnabled = false", self.planner)
        self.assertIn("kWar3PointShadowCpuPlanConsumeDefaultEnabled = false", self.planner)
        self.assertIn("War3PointShadowPersistentMode::Off", self.source)
        self.assertIn("if (!m_pointShadowPersistentWorker)", self.source)
        ctor = self.source.split("War3ShadowReceiverPass::War3ShadowReceiverPass", 1)[1]
        ctor = ctor.split("War3ShadowReceiverPass::~War3ShadowReceiverPass", 1)[0]
        self.assertNotIn("make_unique<PointShadowPersistentPrepareWorker>", ctor)

    def test_worker_request_is_renderer_free_owned_value(self) -> None:
        self.assertIn("FreezePointShadowCaster", self.source)
        self.assertIn("result.frozenComplete = true", self.source)
        self.assertIn("storage.casters.push_back", self.source)
        self.assertIn("storage.paletteHashes.push_back", self.source)
        self.assertIn("PointShadowHandleIdentity", self.source)
        self.assertIn("War3PointShadowCpuPlanRequestPayload", self.header)
        planner_payload = self.planner.split(
            "struct War3PointShadowCpuPlanRequestPayload", 1
        )[1].split("enum class War3PointShadowCpuPlanDisposition", 1)[0]
        for forbidden in ("War3ShadowCasterDraw", "Rc<", "VkBuffer", "Dxvk", "void*"):
            self.assertNotIn(forbidden, planner_payload)

    def test_exact_owner_tuple_and_three_seals_are_revalidated(self) -> None:
        for token in (
            "m_pointShadowPersistentRendererEpoch",
            "m_pointShadowPersistentJobSerial",
            "input.frameSerial",
            "lightSnapshot.generation",
            "payload.seal.replayGeneration = replaySeal.finish()",
            "payload.seal.policyRevision = PointShadowPolicySeal",
            "payload.seal.lifecycleGeneration = PointShadowLifecycleSeal",
            "proposal.seal != m_pointShadowPersistentPendingSeal",
            "PointShadowSettingsExact",
            "PointShadowHistoryExact",
            "PointShadowLightExact",
            "PointShadowCasterExact",
            "proposal.storage.paletteHashes[palette] != currentHash",
            "m_pointShadowPersistentExpectedDynamicPoseSignature",
            "m_pointShadowPersistentExpectedDynamicPoseCount",
            "m_pointShadowPersistentExpectedDynamicSkinnedOutputCount",
            "replaySeal.mixU64(payload.dynamicPoseSignature)",
            "replaySeal.mixU64(payload.dynamicPoseCount)",
            "replaySeal.mixU64(payload.dynamicSkinnedOutputCount)",
            "input.scene.shadowStats.dynamicPoseSignature !=",
            "input.scene.shadowStats.dynamicPoseCount !=",
            "input.scene.shadowStats.dynamicSkinnedOutputCount !=",
        ):
            self.assertIn(token, self.header + self.source)

    def test_reuse_is_distinct_and_cannot_zero_live_payload(self) -> None:
        self.assertIn("War3PointShadowCpuReuseProposalExact", self.planner)
        collect = self.source.split(
            "tryCollectPointShadowPersistentProposal", 1
        )[1].split("adoptPointShadowPersistentProposal", 1)[0]
        self.assertIn(
            "War3PointShadowCpuPlanDisposition::ReusePublished", collect
        )
        self.assertIn("War3PointShadowCpuReuseProposalExact", collect)
        self.assertIn("m_pointShadowPublishedLightIds", collect)
        self.assertIn("m_pointShadowPersistentExpectedLights", collect)

        reuse = self.source.split(
            "bool War3ShadowReceiverPass::adoptPointShadowPersistentReuseProposal", 1
        )[1].split(
            "bool War3ShadowReceiverPass::adoptPointShadowPersistentRenderProposal", 1
        )[0]
        self.assertIn("m_pointShadowTemporalAge = proposal.nextTemporalAge", reuse)
        self.assertIn("recyclePointShadowPersistentStorage", reuse)
        self.assertNotIn("m_pointShadowData[", reuse)
        self.assertNotIn("faceCasters[face].swap", reuse)
        self.assertNotIn("proposal.lights[", reuse)

        render = self.source.split(
            "bool War3ShadowReceiverPass::adoptPointShadowPersistentRenderProposal", 1
        )[1].split(
            "bool War3ShadowReceiverPass::pointShadowPersistentProposalMatchesCanonical",
            1,
        )[0]
        self.assertIn("m_pointShadowData[light]", render)
        self.assertIn("faceCasters[face].swap", render)

    def test_freeze_and_submit_are_one_exception_transaction(self) -> None:
        begin = self.source.split(
            "beginPointShadowPersistentPrepare", 1
        )[1].split("tryCollectPointShadowPersistentProposal", 1)[0]
        for token in (
            "try {",
            "storage.paletteHashes.reserve",
            "storage.paletteHashes.push_back",
            "storage.casters.reserve",
            "storage.casters.push_back",
            "m_pointShadowPersistentWorker->submit(request)",
            "catch (const std::bad_alloc &)",
            "catch (...)",
            "request.generation = {}",
            "request.payload.seal = {}",
            "m_pointShadowPersistentPending = false",
            "canonical plan in this same frame",
        ):
            self.assertIn(token, begin)
        self.assertIn("std::in_place, std::move(proposal)", self.source)

    def test_render_thread_never_waits_for_persistent_worker(self) -> None:
        collect = self.source.split(
            "tryCollectPointShadowPersistentProposal", 1
        )[1].split("adoptPointShadowPersistentProposal", 1)[0]
        self.assertIn("tryCollectExact(", collect)
        self.assertNotIn("waitAndCollectExact", collect)
        self.assertNotIn(".wait(", collect)
        self.assertIn("War3PointShadowPrepareResultState::NotReady", collect)
        self.assertIn("m_pointShadowPersistentDeadlineFallback", collect)
        self.assertIn("preparePointShadowCpuPlan(syncInput", self.source)

    def test_observe_cannot_adopt_and_consume_is_explicit(self) -> None:
        render = self.source.split(
            "void War3ShadowReceiverPass::renderPointShadow", 1
        )[1].split("void War3ShadowReceiverPass::drawReceiver", 1)[0]
        consume_guard = (
            "persistentMode == War3PointShadowPersistentMode::Consume &&"
        )
        self.assertIn(consume_guard, render)
        self.assertIn("adoptPointShadowPersistentProposal", render)
        self.assertIn("War3PointShadowPersistentMode::Observe", render)
        self.assertIn("pointShadowPersistentProposalMatchesCanonical", render)
        self.assertIn("persistent Observe exact=%llu mismatch=%llu", render)
        self.assertIn("workerDiagnostics.failedJobs", render)
        self.assertLess(render.index(consume_guard), render.index("const bool planNamesCurrentSnapshot"))
        observe_pos = render.index("War3PointShadowPersistentMode::Observe")
        canonical_pos = render.index("preparePointShadowCpuPlan(syncInput")
        self.assertLess(canonical_pos, observe_pos)

    def test_rejected_busy_failed_or_late_work_falls_back_same_frame(self) -> None:
        for token in (
            "submit(request)",
            "War3PointShadowPrepareSubmitStatus::Accepted",
            "submit(Request&) moves only on Accepted",
            "War3PointShadowPrepareResultState::NotReady",
            "m_pointShadowPersistentRejectedFallback",
            "m_pointShadowPersistentDeadlineFallback",
            "preparePointShadowCpuPlan(syncInput, lightSnapshot, replayDrawsOverride)",
        ):
            self.assertIn(token, self.source)

    def test_shutdown_joins_before_renderer_resources_are_destroyed(self) -> None:
        destructor = self.source.split(
            "War3ShadowReceiverPass::~War3ShadowReceiverPass", 1
        )[1].split("Rc<DxvkSampler>", 1)[0]
        self.assertIn("m_pointShadowPersistentWorker->shutdown()", destructor)
        self.assertLess(
            destructor.index("m_pointShadowPersistentWorker->shutdown()"),
            destructor.index("vkDestroyPipeline"),
        )

    def test_planner_is_linked_once_into_production_and_once_into_test(self) -> None:
        self.assertEqual(
            self.meson.count("war3/render/war3_point_shadow_cpu_plan.cpp"), 2
        )
        production = self.meson.split("war3_point_shadow_cpu_plan_test =", 1)[0]
        self.assertIn("war3/render/war3_point_shadow_cpu_plan.cpp", production)


if __name__ == "__main__":
    unittest.main()
