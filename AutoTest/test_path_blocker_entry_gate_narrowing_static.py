#!/usr/bin/env python3
"""Static contracts for P2 batch 3 legacy path-blocker EntryGate narrowing.

This is a wiring/predicate change, not a visual fix. The tests lock both the
reject side and the positive side so fail-closed is not mistaken for a repair.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"


def function_block(source: str, begin: str, end: str) -> str:
    start = source.index(begin)
    stop = source.index(end, start)
    return source[start:stop]


class PathBlockerEntryGateNarrowingStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.lane = function_block(
            cls.device,
            "const auto earlyBatchTag = War3RenderState::GetCurrentBatchTag();",
            "if (dxvk::war3::internal::kPathBlockerHideEnabled && pathBlockerLane)",
        )
        # Capture-path WorldObject stage-only caster (the one that still names
        # Stage13). Pose-path objectCasterByStage is a different, narrower
        # predicate and is left unchanged.
        capture_begin = cls.device.index(
            "const bool objectCasterByTls = (tag == War3BatchTag::WorldObjects ||"
        )
        capture_end = cls.device.index(
            "const bool objectCasterByCurrentObj =",
            capture_begin,
        )
        cls.capture_object = cls.device[capture_begin:capture_end]

    def test_terrain_non_s1_requires_object_evidence(self) -> None:
        self.assertIn(
            "((cat == War3RenderState::StageCategory::Terrain) && stage != 1 &&",
            self.lane,
        )
        self.assertIn("hasObjectEvidenceForBlocker)", self.lane)
        self.assertNotIn(
            "(cat == War3RenderState::StageCategory::Terrain && stage != 1) ||",
            self.lane,
        )

    def test_world_object_without_evidence_or_dispatch_stays_out(self) -> None:
        self.assertIn(
            "(cat == War3RenderState::StageCategory::WorldObject &&",
            self.lane,
        )
        self.assertIn(
            "(hasObjectEvidenceForBlocker || earlyObjectDrawDispatch))",
            self.lane,
        )
        self.assertNotIn(
            "cat == War3RenderState::StageCategory::WorldObject ||",
            self.lane,
        )

    def test_positive_object_evidence_still_enters_lane(self) -> None:
        self.assertIn("GetCurrentBatchObject()", self.lane)
        self.assertIn("shadowSemantic.HasAnyContext()", self.lane)
        evidence = self.lane.index("const bool hasObjectEvidenceForBlocker =")
        lane = self.lane.index("const bool pathBlockerLane =", evidence)
        self.assertLess(evidence, lane)
        self.assertIn("currentObjForBlocker != nullptr", self.lane)
        self.assertIn("batchTagObjectLane", self.lane)
        # Terrain doodad / WorldObject with evidence still OR into the lane.
        terrain = self.lane.index("StageCategory::Terrain")
        world = self.lane.index("StageCategory::WorldObject", terrain)
        self.assertLess(terrain, world)
        self.assertIn("hasObjectEvidenceForBlocker", self.lane[terrain:world + 80])

    def test_positive_dispatch_still_enters_world_object_lane(self) -> None:
        self.assertIn("GetCurrentDrawDispatchContext()", self.lane)
        self.assertIn("CurrentDrawDispatchDomain::Common", self.lane)
        self.assertIn("CurrentDrawDispatchDomain::Special", self.lane)
        self.assertIn(
            "CurrentDrawDispatchDomain::TransparentType0", self.lane
        )
        self.assertIn("earlyObjectDrawDispatch", self.lane)
        world = self.lane.index("StageCategory::WorldObject")
        self.assertIn(
            "earlyObjectDrawDispatch",
            self.lane[world : world + 180],
        )

    def test_batch_tags_remain_an_independent_positive_lane(self) -> None:
        # Current B status: Decorations / WorldObjects / SelectionOverlay tags
        # still form their own object lane and also count as object evidence.
        self.assertIn("War3BatchTag::Decorations", self.lane)
        self.assertIn("War3BatchTag::WorldObjects", self.lane)
        self.assertIn("War3BatchTag::SelectionOverlay", self.lane)
        tag = self.lane.index("const bool batchTagObjectLane =")
        lane = self.lane.index("const bool pathBlockerLane =", tag)
        tail = self.lane[lane:]
        self.assertIn("batchTagObjectLane", tail)

    def test_stage_only_world_object_caster_requires_dispatch(self) -> None:
        self.assertIn("objectCasterByActiveDispatch", self.capture_object)
        self.assertIn("stage == 13", self.capture_object)
        self.assertIn("&&", self.capture_object)
        self.assertIn(
            "CurrentDrawDispatchDomain::Common", self.capture_object
        )
        self.assertIn(
            "CurrentDrawDispatchDomain::Special", self.capture_object
        )
        self.assertIn(
            "CurrentDrawDispatchDomain::TransparentType0",
            self.capture_object,
        )
        # Positive: TLS tags and currentObj remain independent admission.
        self.assertIn("War3BatchTag::WorldObjects", self.capture_object)
        self.assertIn("War3BatchTag::Decorations", self.capture_object)
        self.assertIn("War3BatchTag::SelectionOverlay", self.capture_object)

    def test_pose_path_stage_caster_is_not_this_batch(self) -> None:
        pose = self.device.index(
            "semanticSceneDrawTimePoseRejectNoVertexBlendCount"
        )
        pose_stage = self.device.index(
            "const bool objectCasterByStage =", pose
        )
        pose_block = self.device[pose_stage : pose_stage + 280]
        self.assertIn("stage == 7 || stage == 10 || stage == 11", pose_block)
        self.assertNotIn("objectCasterByActiveDispatch", pose_block)
        self.assertNotIn("stage == 13", pose_block)


if __name__ == "__main__":
    unittest.main()
