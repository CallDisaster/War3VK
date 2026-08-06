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
        for command in (
            "camera.snapshot",
            "camera.apply",
            "camera.pan_to",
            "camera.world_bounds",
            "visibility.full_map",
            "autotest.waypoint",
        ):
            self.assertEqual(
                api.count(f'state->request.command == "{command}"'), 1
            )
        self.assertIn('"war3-main-loop"', api)
        self.assertIn("SnapshotJassCameraForTest", api)
        self.assertIn("ApplyJassCameraForTest", api)
        self.assertIn("PanJassCameraForTest", api)
        self.assertIn("QueryJassWorldBoundsForTest", api)
        self.assertIn("SetJassFullMapVisibilityForTest", api)

    def test_extended_native_contracts_validate_before_call(self) -> None:
        source = (
            ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
        ).read_text(encoding="utf-8")
        for name, signature in (
            ("GetCameraField", "(Hcamerafield;)R"),
            ("GetCameraTargetPositionX", "()R"),
            ("GetCameraTargetPositionY", "()R"),
            ("GetCameraTargetPositionZ", "()R"),
            ("PanCameraToTimed", "(RRR)V"),
            ("GetWorldBounds", "()Hrect;"),
            ("GetRectMinX", "(Hrect;)R"),
            ("GetRectMaxY", "(Hrect;)R"),
            ("FogEnable", "(B)V"),
            ("FogMaskEnable", "(B)V"),
            ("IsFogEnabled", "()B"),
            ("IsFogMaskEnabled", "()B"),
        ):
            self.assertIn(f'ResolveNativeStrict("{name}", "{signature}"', source)
        self.assertIn("paramCount != expectedParamCount", source)
        self.assertIn("actualSignature != expectedSignature", source)
        self.assertIn("s_visibilityLease.fogEnabled", source)
        self.assertIn("s_visibilityLease.fogMaskEnabled", source)
        self.assertIn("DecodeJassRealReturn(natives.getTargetX())", source)
        self.assertIn("NativeGetCameraFieldFn = uint32_t", source)
        self.assertIn("constexpr float kRadiansToDegrees", source)
        self.assertIn(
            "result.angleOfAttack = fields[2] * kRadiansToDegrees", source
        )
        self.assertIn(
            "result.fieldOfView = fields[3] * kRadiansToDegrees", source
        )

    def test_internal_api_is_autotest_opt_in(self) -> None:
        api = (
            ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp"
        ).read_text(encoding="utf-8")
        runner = (ROOT / "AutoTest/war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        self.assertIn('getEnvVar("DXVK_WAR3_INTERNAL_TEST_API")', api)
        self.assertIn('"internal test api is disabled"', api)
        self.assertIn(
            'AUTOTEST_INTERNAL_TEST_API_ENV = "DXVK_WAR3_INTERNAL_TEST_API"',
            runner,
        )
        self.assertEqual(
            runner.count(
                'extra_env.setdefault(AUTOTEST_INTERNAL_TEST_API_ENV, "1")'
            ),
            2,
        )

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

    def test_life_and_death_route_is_bounded_and_restores_visibility(self) -> None:
        runner = (ROOT / "AutoTest/war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        api = (
            ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn('"life_and_death_tdr": {', runner)
        self.assertIn("def run_life_and_death_tdr_scenario(", runner)
        self.assertIn("ISOLATED_DESKTOP_QUARANTINED = True", runner)
        self.assertIn('"useIsolatedDesktop": False', runner)
        self.assertIn("use_isolated_desktop: bool = False", runner)
        self.assertIn("life_and_death_tdr 禁止隔离桌面启动", runner)
        self.assertIn('"isolatedDesktopQuarantined": True', runner)
        self.assertIn('"camera.world_bounds"', runner)
        self.assertIn('"camera.pan_to"', runner)
        self.assertIn("for row, y in enumerate(ys)", runner)
        self.assertIn("distance / 4096.0", runner)
        self.assertIn("max(0.75, min(4.0", runner)
        self.assertIn("current_aoa + 24.0", runner)
        self.assertIn("low_aoa = min(335.0, max(280.0", runner)
        self.assertIn('"visibility.full_map", {"enabled": False}', runner)
        self.assertIn("_runtime_status_device_lost(status)", runner)
        self.assertIn("startup_input_actions", runner)
        self.assertIn('{"type": "key", "vk": 0x20, "holdMs": 80}', runner)
        self.assertIn("launcher_mode: str = YDWE_LAUNCHER_MODE_DIRECT", runner)
        self.assertIn('helper.pop("_nativeProcessWitness", None)', runner)
        self.assertIn("helper_witness.close()", runner)
        self.assertIn("_query_windows_gpu_events()", runner)
        self.assertIn("gpu_incident_*.json", runner)
        self.assertIn("attach_pid: int = 0", runner)
        self.assertIn('"mode": "attach-only"', runner)
        self.assertIn('"autotest.waypoint"', runner)
        self.assertIn("SetGpuFlightAutoTestContext", api)
        self.assertIn("screenshot_count: int = 12", runner)
        self.assertIn("birth_hold_sec: int = 120", runner)
        self.assertIn('"phase": "birth-hold"', runner)
        self.assertIn("war3 process exited during birth hold", runner)
        self.assertIn("def capture_aligned_screenshot(", runner)
        self.assertIn('"status": _compact_life_and_death_status(status)', runner)
        self.assertIn("fallback_to_window_capture=False", runner)
        self.assertIn('"screenshots": screenshot_rows', runner)


if __name__ == "__main__":
    unittest.main()
