import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WarVKLightingClockStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.settings = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
            encoding="utf-8"
        )
        cls.pipeline = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(
            encoding="utf-8"
        )
        cls.shadow = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
            encoding="utf-8"
        )
        # Phase 5.1-c: the frame-start settings resolution and the sun of the
        # day/night cycle are single implementations shared with proxy mode.
        cls.resolver = (
            ROOT / "src/d3d9/war3/render/war3_settings_resolver.cpp"
        ).read_text(encoding="utf-8")
        cls.sun_cycle = (
            ROOT / "src/d3d9/war3/effects/war3_sun_cycle.h"
        ).read_text(encoding="utf-8")
        cls.japi = (ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp").read_text(
            encoding="utf-8"
        )
        cls.jass = (ROOT / "WarVK/jass/warvk_api.j").read_text(
            encoding="utf-8"
        )
        cls.action = (ROOT / "WarVK/action.txt").read_text(encoding="utf-8")

    def test_clock_motion_and_color_are_independent_settings(self):
        self.assertIn("enum class War3LightingClockMode", self.settings)
        self.assertIn("celestialMotionEnabled = true", self.settings)
        self.assertIn("timeColorGradingEnabled = true", self.settings)
        self.assertIn("clockMode = War3LightingClockMode::GameTime", self.settings)
        self.assertIn("independentDayLengthSeconds = 480.0f", self.settings)

    def test_independent_clock_is_render_only_and_advances_without_shadows(self):
        pipeline_frame_start = re.search(
            r"void War3RenderPipeline::OnFrameStart\(\) \{(.*?)"
            r"// ={20,}",
            self.pipeline,
            re.DOTALL,
        ).group(1)
        # Before the bypass decision, whether or not shadows are wanted.
        self.assertIn(
            "m_settingsResolver.BeginFrame(*m_settingsMailbox, m_settings);",
            pipeline_frame_start,
        )
        on_frame_start = self.resolver.split(
            "void War3SettingsFrameResolver::BeginFrame(", 1
        )[1].split("void War3SettingsFrameResolver::FinishFrame(", 1)[0]
        self.assertIn("War3LightingClockMode::Independent", on_frame_start)
        self.assertIn("lightingClock.renderTimeHours", on_frame_start)
        self.assertNotIn("War3RenderState::SetGameTime", on_frame_start)

    def test_manual_sun_values_are_not_written_by_disabled_cycles(self):
        self.assertIn("if (!dayNightSettings.celestialMotionEnabled)", self.sun_cycle)
        self.assertIn("finalLightDir = settings->sun.direction", self.sun_cycle)
        self.assertIn("finalShadowStrength = settings->shadows.strength", self.sun_cycle)
        self.assertIn("if (!dayNightSettings.timeColorGradingEnabled)", self.sun_cycle)
        self.assertIn("finalLightColor = settings->sun.color", self.sun_cycle)
        self.assertIn("m_sunCycle.Resolve(&*settings,", self.shadow)
        self.assertIn("Vector4 finalLightDir = sunCycle.direction;", self.shadow)
        self.assertRegex(
            self.shadow,
            r"if \(dayNightSettings\.celestialMotionEnabled\)\s+"
            r"mailbox->pending\.sun\.direction = finalLightDir",
        )
        self.assertRegex(
            self.shadow,
            r"if \(dayNightSettings\.timeColorGradingEnabled\)\s+"
            r"mailbox->pending\.sun\.color = finalLightColor",
        )
        self.assertIn("input.lighting->sunDirection = finalLightDir", self.shadow)
        self.assertIn("input.lighting->sunColor = finalLightColor", self.shadow)
        self.assertNotIn("const_cast<War3RenderSettings", self.shadow)

    def test_custom_temperature_profile_is_bounded_and_cyclic(self):
        self.assertIn("customColorTemperatureProfile", self.settings)
        for key in ("midnightKelvin", "dawnKelvin", "noonKelvin", "duskKelvin"):
            self.assertIn(key, self.settings)
        self.assertIn("std::clamp(value, 1000.0f, 20000.0f)", self.sun_cycle)
        self.assertIn("const uint32_t next = (segment + 1u) & 3u", self.sun_cycle)
        self.assertIn("LightingCycleSetColorTemperatureProfile", self.japi)
        self.assertIn("a[index].real > 20000.0f", self.japi)

    def test_public_api_uses_specific_author_names(self):
        for function_name in (
            "WarVKSetLightingClockMode",
            "WarVKSetLightingClockTime",
            "WarVKSetLightingDayDuration",
            "WarVKSetCelestialMotionEnabled",
            "WarVKSetTimeColorGradingEnabled",
            "WarVKSetTimeColorTemperatureProfile",
            "WarVKResetTimeColorTemperatureProfile",
        ):
            self.assertIn(f"function {function_name} ", self.jass)
            self.assertIn(f'script = "{function_name}"', self.action)
        self.assertIn("TC_WarVKLightingClock", self.action)

    def test_setting_author_time_holds_instead_of_being_overwritten(self):
        self.assertIn('"lightingClock.holdTime"', self.japi)
        self.assertIn("settings.clockMode = War3LightingClockMode::Held", self.japi)
        self.assertIn("AdvanceLightingClockRevision(settings)", self.japi)
        self.assertNotIn("War3RenderState::SetGameTime", self.japi)


if __name__ == "__main__":
    unittest.main()
