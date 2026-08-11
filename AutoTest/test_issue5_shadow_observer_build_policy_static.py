#!/usr/bin/env python3
"""Release/default-deny contracts for Issue #5 development observers."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_shadow_observer_build_policy.h"
).read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
INTERNAL = (
    ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
).read_text(encoding="utf-8")
UNION = (
    ROOT / "src/d3d9/war3/render/war3_union_consumer_visibility.cpp"
).read_text(encoding="utf-8")
RECEIVER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"
).read_text(encoding="utf-8")
VISIBILITY = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_visibility.frag"
).read_text(encoding="utf-8")
POINT = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_point_frag.frag"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class Issue5ShadowObserverBuildPolicyStaticTests(unittest.TestCase):
    def test_meson_default_is_release_off_and_dev_is_explicit(self):
        self.assertIn("option('warvk_shadow_observers_dev'", OPTIONS)
        self.assertIn("value : false", OPTIONS)
        self.assertIn("get_option('warvk_shadow_observers_dev')", MESON)
        self.assertIn("-DWARVK_ENABLE_SHADOW_OBSERVERS_DEV=1", MESON)
        self.assertIn("war3_shadow_observer_build_policy_release_test", MESON)
        self.assertIn("war3_shadow_observer_build_policy_dev_test", MESON)
        self.assertIn("-DWARVK_EXPECT_SHADOW_OBSERVERS_DEV=1", MESON)

    def test_build_policy_is_pure_and_cannot_produce_consume(self):
        self.assertIn("enum class War3ShadowObserverBuildMode", POLICY)
        self.assertIn("Off = 0u", POLICY)
        self.assertIn("Observe = 1u", POLICY)
        self.assertNotIn("Consume", POLICY)
        self.assertIn("WARVK_ENABLE_SHADOW_OBSERVERS_DEV", POLICY)
        self.assertIn("kDevelopmentShadowObserversEnabled = true", POLICY)
        self.assertIn("kDevelopmentShadowObserversEnabled = false", POLICY)
        parser = function_body(POLICY, "ParseShadowObserverBuildMode")
        self.assertIn("configuredMode == 1u", parser)
        self.assertIn("War3ShadowObserverBuildMode::Observe", parser)
        self.assertIn("War3ShadowObserverBuildMode::Off", parser)
        for forbidden in ("env::", "getenv", "Logger", "new "):
            self.assertNotIn(forbidden, POLICY)

    def test_runtime_entrypoints_share_observe_only_policy(self):
        union = function_body(SHADOW, "War3UnionCullModeRuntime")
        terrain_consumer = function_body(
            SHADOW, "War3TerrainBoundsCullModeRuntime"
        )
        terrain_producer = function_body(
            DEVICE, "War3TerrainBoundsCullModeRuntime"
        )
        for runtime, variable in (
            (union, "DXVK_WAR3_UNION_CONSUMER_CULL_MODE"),
            (terrain_consumer, "DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE"),
            (terrain_producer, "DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE"),
        ):
            self.assertIn("kDevelopmentShadowObserversEnabled", runtime)
            self.assertIn("ParseShadowObserverBuildMode", runtime)
            self.assertIn(variable, runtime)
            self.assertIn("::Observe", runtime)
            self.assertIn("::Off", runtime)
            self.assertNotIn("::Consume", runtime)
        for terrain_runtime in (terrain_consumer, terrain_producer):
            self.assertNotIn(
                "DXVK_WAR3_CSM_TERRAIN_BOUNDS_CULL", terrain_runtime
            )
            self.assertNotIn("EnvFlagDefault", terrain_runtime)

    def test_release_freeze_and_consume_denials_remain_intact(self):
        self.assertIn(
            "kReleaseFreezeExperimentalShadowRoutes = true", INTERNAL
        )
        self.assertIn("consumeAdmissionGranted = false", SHADOW)
        self.assertIn("if (!query.consumeAdmissionGranted)", UNION)
        self.assertIn("s_objectBoundsCullConsume && c >= 2u", SHADOW)
        self.assertIn("!consumeObjectCascade || objectWouldBeVisible", SHADOW)

    def test_observer_policy_does_not_touch_shader_or_point_shadow_abi(self):
        for shader in (RECEIVER, VISIBILITY, POINT):
            self.assertNotIn("WARVK_ENABLE_SHADOW_OBSERVERS_DEV", shader)
            self.assertNotIn("ShadowObserverBuildMode", shader)
        self.assertNotIn("ShadowObserverBuildMode", POINT)


if __name__ == "__main__":
    unittest.main()
