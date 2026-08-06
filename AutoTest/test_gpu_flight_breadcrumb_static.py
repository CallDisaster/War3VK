import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class GpuFlightBreadcrumbStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = (
            ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h"
        ).read_text(encoding="utf-8")
        cls.hub = (
            ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
        ).read_text(encoding="utf-8")
        cls.shadow = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
            encoding="utf-8"
        )
        cls.pipeline = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(
            encoding="utf-8"
        )
        cls.autotest = (ROOT / "AutoTest/war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )

    def test_fixed_before_ui_label_is_replaced_by_atomic_breadcrumb(self) -> None:
        self.assertNotIn(
            'frame.lastRenderStage = "War3Pipeline.BeforeUi.PostPass"', self.hub
        )
        self.assertIn("enum class GpuFlightBreadcrumb", self.header)
        self.assertIn("s_gpuFlightBreadcrumb.load", self.hub)
        for stage in (
            "CsmPreflight",
            "CsmCascade",
            "CsmTerrainMask",
            "PointShadowPlan",
            "PointShadowFace",
            "ShadowReceiverDraw",
            "VolumetricLight",
            "Aa",
        ):
            self.assertIn(stage, self.header)

    def test_csm_and_point_face_work_is_recorded(self) -> None:
        self.assertIn("ResetGpuFlightCsmWork", self.shadow)
        self.assertIn("SetGpuFlightCsmCascadeWork", self.shadow)
        self.assertIn("ResetGpuFlightPointShadowWork", self.shadow)
        self.assertIn("SetGpuFlightPointShadowFacePlan", self.shadow)
        self.assertIn("SetGpuFlightPointShadowFaceWork", self.shadow)
        for key in (
            '"csmCascadeDrawCount"',
            '"csmCascadeTriangleCount"',
            '"pointShadowFaceCandidateCount"',
            '"pointShadowFaceDrawCount"',
            '"arenaFrameUsedDeltaBytes"',
        ):
            self.assertIn(key, self.hub)

    def test_autotest_context_is_bounded_and_cleared(self) -> None:
        self.assertIn("SetGpuFlightAutoTestContext", self.header)
        self.assertIn("ClearGpuFlightAutoTestContext", self.header)
        self.assertIn('"autoTestWaypointIndex"', self.hub)
        self.assertIn('"worldBounds"', self.hub)

    def test_tdr_route_samples_full_fresh_runtime_status(self) -> None:
        self.assertIn(
            "file_status = _read_runtime_status_file(w3)", self.autotest
        )
        self.assertIn(
            "abs(now_timestamp - file_timestamp) <= 5000", self.autotest
        )
        self.assertIn('encoding="utf-8"', self.autotest)
        self.assertIn('errors="replace"', self.autotest)


if __name__ == "__main__":
    unittest.main()
