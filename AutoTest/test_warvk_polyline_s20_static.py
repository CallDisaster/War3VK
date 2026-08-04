import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WarVkPolylineS20StaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.curve_h = (ROOT / "src/d3d9/war3/math/war3_curve_runtime.h").read_text(encoding="utf-8")
        cls.curve_cpp = (ROOT / "src/d3d9/war3/math/war3_curve_runtime.cpp").read_text(encoding="utf-8")
        cls.light_h = (ROOT / "src/d3d9/war3/render/war3_lightning_runtime.h").read_text(encoding="utf-8")
        cls.light_cpp = (ROOT / "src/d3d9/war3/render/war3_lightning_runtime.cpp").read_text(encoding="utf-8")
        cls.japi = (ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp").read_text(encoding="utf-8")
        cls.jass = (ROOT / "WarVK/jass/warvk_api.j").read_text(encoding="utf-8")
        cls.constants = (ROOT / "WarVK/jass/warvk_constant.j").read_text(encoding="utf-8")

    def test_point_upload_is_bounded_and_finalize_publishes_arc_length_snapshot(self):
        self.assertIn("kMaximumPointCurvePoints = 1024u", self.curve_h)
        self.assertIn("std::shared_ptr<const PointCurveData> pointCurve", self.curve_h)
        self.assertIn("uint32_t expectedPointCount", self.curve_h)
        self.assertIn("pointCount > 4u", self.curve_cpp)
        self.assertIn("pointBuilder.size() != curve->second.expectedPointCount", self.curve_cpp)
        self.assertIn("data->cumulativeLengths.resize", self.curve_cpp)
        self.assertLess(
            self.curve_cpp.index("data->totalLength ="),
            self.curve_cpp.index("curve->second.snapshot.pointCurve ="),
        )

    def test_polyline_is_one_record_and_one_continuous_triangle_strip(self):
        self.assertIn("createPolylineFromTemplate", self.light_h)
        self.assertIn("BuildPolylineRibbonVertices", self.light_cpp)
        builder = re.search(
            r"void BuildPolylineRibbonVertices\(.*?\n\}", self.light_cpp, re.DOTALL
        ).group(0)
        self.assertIn("out.reserve(curve.points.size() * 2u)", builder)
        self.assertIn("curve.cumulativeLengths[index]", builder)
        self.assertIn("previousSide", builder)
        creator = re.search(
            r"int32_t War3LightningRuntime::createPolylineFromTemplate\(.*?\n\}",
            self.light_cpp,
            re.DOTALL,
        ).group(0)
        self.assertEqual(creator.count("m_records.emplace"), 1)
        self.assertIn("record.branchCount = 0u", creator)
        self.assertIn("record.polylineCurve = std::move(points)", creator)

    def test_public_protocol_and_jass_expose_chunked_atomic_workflow(self):
        self.assertIn("kFeaturePolylineCurve = 0x00004000u", self.japi)
        self.assertIn("WARVK_FEATURE_POLYLINE_CURVE = 16384", self.constants)
        for command in (
            "curve.points.create",
            "curve.points.append4",
            "curve.points.finalize",
            "lightning.createPolylineFromTemplate",
            "lightning.setPolylineCurve",
        ):
            self.assertIn(f'"{command}"', self.japi)
        for function in (
            "WarVKCreatePointCurve",
            "WarVKAppendPointCurve4",
            "WarVKFinalizePointCurve",
            "WarVKCreatePolylineLightning",
            "WarVKSetPolylineLightningCurve",
        ):
            self.assertIn(f"function {function}", self.jass)
            metadata = (
                (ROOT / "WarVK/action.txt").read_text(encoding="utf-8")
                + (ROOT / "WarVK/call.txt").read_text(encoding="utf-8")
            )
            self.assertIn(f'script = "{function}"', metadata)
        append_spec = re.search(
            r'"curve\.points\.append4".*?"([dir]+)"', self.japi
        ).group(1)
        self.assertEqual(append_spec, "di" + "r" * 12)
        self.assertLessEqual(len(append_spec), 16)
        smoke = (ROOT / "WarVK/jass/warvk_smoke_test.j").read_text(encoding="utf-8")
        self.assertIn("function WarVKBeginLorenzPolylineSmokeTest", smoke)
        self.assertIn("local integer pointCount = 640", smoke)
        self.assertIn("WarVKAppendPointCurve4", smoke)
        self.assertIn("WarVKCreatePolylineLightning", smoke)

    def test_warvk_draw_runs_after_native_s20_world_dispatch(self):
        hook = (ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp").read_text(encoding="utf-8")
        lightning_hook = re.search(
            r"static void TryWarVkLightningWorldStageDraw\(.*?\n\}", hook, re.DOTALL
        ).group(0)
        self.assertIn("kWarVkLightningStage = 20", lightning_hook)
        self.assertNotIn("kNativeSemanticShadowExecuteStage", lightning_hook)
        world = re.search(
            r"int __fastcall Hook_WorldDispatch\(.*?\n\}", hook, re.DOTALL
        ).group(0)
        self.assertLess(world.index("g_trampolineWorldDispatch"),
                        world.index("TryWarVkLightningWorldStageDraw"))
        pipeline = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(encoding="utf-8")
        self.assertNotIn("wantsLightning", pipeline)

    def test_s20_is_lightning_effect_and_all_shadow_producers_reject_it(self):
        stage_map = (ROOT / "src/d3d9/war3/hooks/war3_stage_tag_map.cpp").read_text(encoding="utf-8")
        render_state = (ROOT / "src/d3d9/war3/render/war3_render_state.cpp").read_text(encoding="utf-8")
        policy_h = (ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.h").read_text(encoding="utf-8")
        policy_cpp = (ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.cpp").read_text(encoding="utf-8")
        self.assertEqual(stage_map.count("return War3BatchTag::Lightning;"), 2)
        self.assertRegex(render_state, r"case 18:\s*case 20:\s*return StageCategory::Effect;")
        self.assertIn("RejectNativeLightningStage", policy_h)
        predicate = re.search(
            r"bool IsShadowNativeLightning\(.*?\n\}", policy_cpp, re.DOTALL
        ).group(0)
        self.assertIn("context.stage == 20", predicate)
        self.assertIn("War3BatchTag::Lightning", predicate)
        evaluate = re.search(
            r"ShadowProducerPolicyDecision EvaluateShadowProducerPolicy\(.*?\n\}",
            policy_cpp,
            re.DOTALL,
        ).group(0)
        self.assertLess(evaluate.index("IsShadowNativeLightning"),
                        evaluate.index("IsShadowVisualOverlay"))
        self.assertIn("rejectedLightning", policy_h)


if __name__ == "__main__":
    unittest.main()
