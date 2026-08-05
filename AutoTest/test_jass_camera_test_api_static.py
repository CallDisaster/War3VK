import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class JassCameraTestApiStaticTests(unittest.TestCase):
    def test_camera_native_contract_is_fail_closed(self) -> None:
        source = (
            ROOT
            / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn('LookupNativeEntry("ConvertCameraField")', source)
        self.assertIn('LookupNativeEntry("SetCameraField")', source)
        self.assertIn('result.convertSignature.rfind("(I)", 0u) == 0u', source)
        self.assertIn(
            'result.setSignature.rfind("(Hcamerafield;RR)", 0u) == 0u',
            source,
        )
        self.assertIn("result.cameraFieldHandle = convertFn(2)", source)
        self.assertLess(
            source.index("if (!result.signaturesValidated)"),
            source.index("result.cameraFieldHandle = convertFn(2)"),
        )
        self.assertIn('LookupNativeEntry("SetCameraPosition")', source)
        self.assertIn('result.positionSignature == "(RR)V"', source)
        self.assertIn("for (int32_t index = 0; index < 7; ++index)", source)
        self.assertIn(
            "if (index != 0 && result.cameraFieldHandles[index] == 0u)",
            source,
        )
        self.assertLess(
            source.index("if (!result.signaturesValidated)"),
            source.index("positionFn(&x, &y)"),
        )

    def test_internal_api_exposes_only_the_test_command(self) -> None:
        api = (
            ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp"
        ).read_text(encoding="utf-8")
        self.assertEqual(
            api.count(
                'state->request.command == "camera.angle_of_attack"'
            ),
            1,
        )
        self.assertIn("SetJassCameraAngleOfAttackForTest", api)
        self.assertEqual(
            api.count('state->request.command == "camera.fixed"'),
            1,
        )
        self.assertIn("SetJassFixedCameraForTest", api)

    def test_bridge_probe_refreshes_scripted_camera_override(self) -> None:
        runner = (
            ROOT / "AutoTest/run_bridge_ramp_visual_probe.py"
        ).read_text(encoding="utf-8")
        self.assertIn('"--camera-angle-deg"', runner)
        self.assertIn('"camera.angle_of_attack"', runner)
        self.assertIn('"camera.fixed"', runner)
        self.assertIn('"--camera-target-x"', runner)
        self.assertIn('"--camera-target-y"', runner)
        self.assertIn('"--camera-pan-x-amplitude"', runner)
        self.assertIn("math.sin(phase)", runner)
        self.assertIn("camera_request(index)", runner)
        self.assertIn('"refreshFailures"', runner)
        self.assertIn(
            'int(camera_angle.get("refreshFailures", 0) or 0) == 0',
            runner,
        )


if __name__ == "__main__":
    unittest.main()
