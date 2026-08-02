import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WarVKVisualBridgeV1StaticTests(unittest.TestCase):
    def setUp(self) -> None:
        self.old_bridge = (ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp").read_text(
            encoding="utf-8")
        self.visual_bridge = (ROOT / "src/d3d9/war3/bridge/war3_visual_bridge_v1.cpp").read_text(
            encoding="utf-8")
        self.exports = (ROOT / "src/d3d9/d3d9.def").read_text(encoding="utf-8")

    def test_canonical_v1_is_forwarded_before_old_payload_dispatch(self) -> None:
        decode = self.old_bridge.index("bool TryHandleCarrier")
        pass_v1 = self.old_bridge.index("StartsWith(command, kWarVkV1Prefix)", decode)
        old_dispatch = self.old_bridge.index("HandleWarVkPayload", pass_v1)
        self.assertLess(pass_v1, old_dispatch)
        self.assertIn('kWarVkV1Prefix = "warvk:v1;"', self.old_bridge)

    def test_bridge_is_versioned_narrow_c_abi(self) -> None:
        self.assertIn("kAbiVersion = 0x00010000u", self.visual_bridge)
        self.assertIn(
            "920872221B3836A5EFF69D3EC721915B21E0C4B5399C0F09F05B028CF46D27BF",
            self.visual_bridge)
        self.assertNotIn("JapiFunc", self.visual_bridge)
        self.assertNotIn("void**", self.visual_bridge)
        self.assertNotIn("LoadLibrary", self.visual_bridge)

    def test_all_point_light_entrypoints_are_named_exports(self) -> None:
        for name in (
            "GetAbiVersion", "GetManifestSha256", "GetFeatureFlags",
            "IsRuntimeReady", "PointLightCreate", "PointLightDestroy",
            "PointLightSetEnabled", "PointLightSetPosition",
            "PointLightSetColorIntensity", "PointLightSetRadius",
            "PointLightSetShadowEnabled", "PointLightSetShadowConfig",
            "PointLightIsAlive", "PointLightCount",
        ):
            self.assertIn(f"WarVKVisualV1_{name}", self.exports)


if __name__ == "__main__":
    unittest.main()
