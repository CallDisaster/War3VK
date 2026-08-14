from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class VolumetricShaderWorkAdmissionStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpp = (ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp").read_text(
            encoding="utf-8"
        )
        cls.header = (
            ROOT
            / "src/d3d9/war3/render/war3_volumetric_shader_work_admission.h"
        ).read_text(encoding="utf-8")
        cls.policy = (
            ROOT
            / "src/d3d9/war3/render/war3_volumetric_shader_work_admission.cpp"
        ).read_text(encoding="utf-8")
        cls.shader = (
            ROOT / "subprojects/war3fx/shaders/war3_volumetric_light.frag"
        ).read_text(encoding="utf-8")
        cls.composite = (
            ROOT / "subprojects/war3fx/shaders/war3_volumetric_composite.frag"
        ).read_text(encoding="utf-8")
        cls.meson = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")

    def test_policy_models_every_bounded_shader_multiplier(self) -> None:
        for field in (
            "rayWidth",
            "rayHeight",
            "sampleCount",
            "directionalProbeCount",
            "directionalCascadeSamples",
            "directionalTapsPerCascade",
            "shadowedPointLightCount",
            "pointProbesPerLight",
            "compositeWidth",
            "compositeHeight",
            "compositeTextureReadsPerPixel",
        ):
            self.assertIn(field, self.header)
        self.assertGreaterEqual(self.policy.count("War3VolumetricCheckedMultiply"), 10)
        self.assertGreaterEqual(self.policy.count("War3VolumetricCheckedAdd"), 4)

    def test_shader_bounds_match_the_runtime_upper_bound(self) -> None:
        self.assertIn("for (int probeIndex = 0; probeIndex < 8; ++probeIndex)", self.shader)
        self.assertIn("for (int dy = -1; dy <= 1; ++dy)", self.shader)
        self.assertIn("for (int dx = -1; dx <= 1; ++dx)", self.shader)
        self.assertIn("for (int pointProbe = 0; pointProbe < 2; ++pointProbe)", self.shader)
        self.assertIn("kVolumetricDirectionalProbesPerSegment = 8u", self.cpp)
        self.assertIn("kVolumetricDirectionalCascadeSamples = 2u", self.cpp)
        self.assertIn("kVolumetricVolumeSunTapsPerCascade = 9u", self.cpp)

    def test_composite_fixed_full_resolution_reads_are_charged(self) -> None:
        self.assertIn("for (int i = 0; i < 4; i++)", self.composite)
        self.assertIn("kVolumetricCompositeTextureReadsPerPixel = 24u", self.cpp)
        self.assertIn("maxTotalWork = 350ull * 1024ull * 1024ull", self.header)
        self.assertIn("compositeTextureReads", self.header)

    def test_admission_precedes_every_gpu_side_volume_action(self) -> None:
        run = self.cpp[self.cpp.index("void War3VolumetricLightPass::Run(") :]
        admission = run.index("EvaluateWar3VolumetricShaderWork")
        self.assertLess(admission, run.index("ensureResources("))
        self.assertLess(admission, run.index("copyColor("))
        self.assertLess(admission, run.index("copyDepth("))
        self.assertLess(admission, run.index("drawVolumetricLight("))
        self.assertIn("if (!workEstimate.accepted)", run)

    def test_diagnostics_keep_components_and_reject_reason(self) -> None:
        for field in (
            "raySegments",
            "directionalD32Reads",
            "pointCubeReads",
            "compositeTextureReads",
            "totalWork",
            "rejectReason",
        ):
            self.assertIn(field, self.header)
        self.assertIn("QueryWar3VolumetricShaderWorkRuntimeDiagnostics", self.cpp)
        self.assertIn("PublishVolumetricShaderWorkDiagnostics", self.cpp)

    def test_value_only_runnable_is_registered(self) -> None:
        self.assertIn("war3_volumetric_shader_work_admission.cpp", self.meson)
        self.assertIn("war3_volumetric_shader_work_admission_test.cpp", self.meson)
        self.assertIn("'war3_volumetric_shader_work_admission'", self.meson)


if __name__ == "__main__":
    unittest.main()
