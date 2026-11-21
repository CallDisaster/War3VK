"""Wiring checks for the sun dial (plan 12.17).

The dial, the real shadow length and the colour curve reach both renderers:
in process through the settings, in proxy mode through the diagnostic block
(the x86 data layer writes, the host reads). The defaults leave the sun model
on its old path. Pictures and the gates are run separately.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


SETTINGS = source('src/d3d9/d3d9_war3_settings.h')
SUN = source('src/d3d9/war3/effects/war3_sun_cycle.h')
BLOCK = source('src/d3d9/war3/platform/war3_proxy_diag_block.h')
FRAME = source('src/d3d9/war3/platform/war3_proxy_frame.cpp')
HOST = source('tools/render_host/dxvk_core/shadow_before_ui.cpp')
INPUT = source('src/d3d9/war3/effects/war3_shadow_scene_input.h')
PANEL = source('src/d3d9/war3/ui/war3_imgui.cpp')


class Defaults(unittest.TestCase):
    def test_off_by_default(self):
        style = SETTINGS[SETTINGS.index('struct War3SunStyleSettings {'):]
        style = style[:style.index('};')]
        self.assertIn('bool realShadowLength = false;', style)
        self.assertIn('bool customTint = false;', style)
        self.assertIn('bool pathSet = false;', style)
        self.assertIn('War3SunStyleSettings sunStyle{};', SETTINGS)

    def test_default_path_unless_set(self):
        # Unset, the bearing and the altitudes are the old code's.
        self.assertIn('const bool customPath = style.pathSet;', SUN)
        self.assertIn('float cosYaw = customPath ? sunPath.cosYaw : std::cos(sunYaw);', SUN)
        self.assertIn('float sinYaw = customPath ? sunPath.sinYaw : std::sin(sunYaw);', SUN)
        self.assertIn('float cYaw = customPath ? moonPath.cosYaw : std::cos(moonYaw);', SUN)
        self.assertIn('float sYaw = customPath ? moonPath.sinYaw : std::sin(moonYaw);', SUN)
        self.assertIn('if (customPath && realLength && realAltitudeRad > 0.0f)', SUN)
        self.assertIn('if (customPath && realLength && moonRealAlt > 0.0f)', SUN)
        self.assertIn('static constexpr float kMinPathNorth = 0.08f;', SUN)

    def test_live_feedback_while_the_panel_commits(self):
        frame = FRAME[FRAME.index('void FeedSunBack('):FRAME.index('void ResolveSettings()')]
        shadow = source('src/d3d9/d3d9_war3_shadow.cpp')
        shadow = shadow[shadow.index('if (mailbox->pendingRevision == input.settingsRevision) {'):]
        shadow = shadow[:shadow.index('// 调试输出')]
        for text, color in ((frame, 'pending.sun.color = g.sun.color;'),
                            (shadow, 'pending.sun.color = finalLightColor;')):
            self.assertIn('pending.dayNight.timeColorGradingEnabled', text)
            self.assertIn('pending.dayNight.celestialMotionEnabled', text)
            self.assertIn('War3LightingClockMode::GameTime', text)
            self.assertIn(color, text)

    def test_model_changes_only_when_asked(self):
        self.assertIn('return Evaluate(*settings, time01);', SUN)
        self.assertIn('const bool realLength = style.realShadowLength;', SUN)
        self.assertIn('if (dayNightSettings.timeColorGradingEnabled && style.customTint)', SUN)
        self.assertIn('static constexpr float kRealShadowFloorDeg = 3.0f;', SUN)
        self.assertIn('static constexpr float kMinSunAltitudeDeg = 35.0f;', SUN)
        self.assertIn('static constexpr float kMaxSunAltitudeDeg = 55.0f;', SUN)


class Look(unittest.TestCase):
    """The look of the hour (plan 12.19): colour, brightness, shading, the shadow's strength and
    colour follow the light's own elevation; the real shadow length changes the shadow alone."""

    def test_world_is_shaded_from_the_look_not_the_shadow(self):
        frame = FRAME[FRAME.index('void FeedSunBack('):FRAME.index('void ResolveSettings()')]
        shadow = source('src/d3d9/d3d9_war3_shadow.cpp')
        self.assertEqual(frame.count('sun.direction = g.sun.lightDirection;'), 2)
        self.assertEqual(shadow.count('sun.direction = sunCycle.lightDirection;'), 2)
        # The shadow pass itself keeps the direction the shadow is cast from.
        self.assertIn('mutableSettings.sun.direction = finalLightDir;', shadow)
        self.assertIn('result.lightDirection = shadeLightDir;', SUN)

    def test_look_follows_the_lights_own_elevation(self):
        self.assertIn('const float sunLookRad = realAltitudeRad > 0.0f ? LightElevationRad(style, dayPhase) : 0.0f;', SUN)
        self.assertIn('const float moonLookRad = moonRealAlt > 0.0f ? LightElevationRad(style, nightPhase) : 0.0f;', SUN)
        look = SUN[SUN.index('static float LightElevationRad('):]
        look = look[:look.index('\n  }\n')]
        # Neither the clamp of the short shadows nor the real length's floor is in it.
        for word in ('realShadowLength', 'kMinSunAltitudeDeg', 'kRealShadowFloorDeg'):
            self.assertNotIn(word, look)
        self.assertIn('const Vector4 shadowLook = ShadowLook(finalLightColor, lookRad,', SUN)
        self.assertIn('const float shadeAltRad = ShadeAltitudeRad(lookRad);', SUN)

    def test_shadow_keeps_the_skys_light(self):
        shadow = source('src/d3d9/d3d9_war3_shadow.cpp')
        shader = source('subprojects/war3fx/shaders/war3_shadow_receiver.frag')
        block = shader[shader.index('vec4 u_water;'):shader.index('} ubo;')]
        self.assertIn('vec4 u_shadowTint;', block)
        self.assertIn('vec3 strengthRgb = clamp(strength * ubo.u_shadowTint.rgb, 0.0, 1.0);', shader)
        self.assertIn('vec3 mul = vec3(1.0) - strengthRgb + strengthRgb * vis;', shader)
        self.assertRegex(shadow, r'Vector4 water;\n(  //[^\n]*\n)+  Vector4 shadowTint;\n\};')
        self.assertIn('static_assert(sizeof(ShadowReceiverUniform) == 768u,', shadow)
        self.assertIn('const Vector4 receiverShadowTint = sunCycle.shadowTint;', shadow)
        # A cycle that is switched off leaves a grey shadow of the author's strength.
        off = SUN[SUN.index('if (!dayNightSettings.celestialMotionEnabled) {\n      finalLightDir = settings->sun.direction;'):]
        off = off[:off.index('}')]
        self.assertIn('finalShadowStrength = settings->shadows.strength;', off)
        self.assertIn('shadowTint = Vector4(1.0f, 1.0f, 1.0f, 0.0f);', off)

    def test_ambient_follows_the_same_hour(self):
        resolver = source('src/d3d9/war3/render/war3_settings_resolver.cpp')
        self.assertIn('float time01 = settings.shadows.lockSun', resolver)
        self.assertIn('War3SunCycle::AmbientBlend(dayNight.nightAmbient, dayNight.dayAmbient, t) *', resolver)
        self.assertIn('War3SunCycle::AmbientTint(settings.sunStyle, time01, t)', resolver)

    def test_noon_altitude_is_the_looks_own(self):
        # Shown for the default path with either shadow length.
        self.assertIn('if (!style.pathSet)', PANEL)
        self.assertNotIn('BeginDisabled(!settings.sunStyle.realShadowLength)', PANEL)


class LookNumbers(unittest.TestCase):
    """AutoTest/sun_cycle_fixture.cpp, built for both sizes and run."""

    TOOLS = Path(r'E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/llvm-mingw-20260616-ucrt-x86_64/bin')

    def test_fixture(self):
        import subprocess
        import tempfile
        if not self.TOOLS.is_dir():
            self.skipTest('no llvm-mingw toolchain')
        includes = [f'-I{ROOT / p}' for p in ('src/d3d9', 'src', 'include', 'include/vulkan/include',
                                              'include/spirv/include', 'include/native')]
        with tempfile.TemporaryDirectory() as tmp:
            for triple in ('i686', 'x86_64'):
                exe = Path(tmp) / f'sun-{triple}.exe'
                build = subprocess.run([str(self.TOOLS / f'{triple}-w64-mingw32-clang++.exe'), '-std=c++17', '-O1',
                                        '-static', *includes, str(ROOT / 'AutoTest/sun_cycle_fixture.cpp'),
                                        '-o', str(exe)], capture_output=True, text=True, errors='replace')
                self.assertEqual(build.returncode, 0, build.stderr[-2000:])
                run = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace', timeout=300)
                self.assertEqual(run.returncode, 0, run.stdout[-2000:])
                self.assertIn('sun cycle fixture: all checks hold', run.stdout)


class ProxyTransport(unittest.TestCase):
    def test_block_version_and_layout(self):
        self.assertIn('constexpr uint32_t Version = 6u;', BLOCK)
        self.assertIn('uint32_t sunStyle[2 + 4 * 6 + 1];', BLOCK)
        self.assertIn('constexpr unsigned SunStyleWordCount = 2 + 4 * 6;', BLOCK)
        # Appended: everything before it keeps its offset.
        block = BLOCK[BLOCK.index('struct Block {'):]
        block = block[:block.index('\n};')]
        lines = [l.strip() for l in block.rstrip().splitlines() if l.strip() and not l.strip().startswith('//')]
        self.assertTrue(lines[-3].startswith('uint32_t sunStyle['))
        # Sun and v4 water keep their offsets as the in-place tail grows.
        self.assertTrue(lines[-2].startswith('uint32_t water['))
        self.assertEqual(lines[-1], 'alignas(8) uint32_t waterInPlace[8];')

    def test_unwritten_block_and_bad_numbers_mean_defaults(self):
        decode = BLOCK[BLOCK.index('inline void DecodeSunStyle('):]
        decode = decode[:decode.index('\n}\n')]
        self.assertIn('s = Style{};', decode)
        self.assertIn("if (!(w[0] & 0x80000000u))\n    return;", decode)
        self.assertIn('std::isfinite(v) && v >= lo && v <= hi ? v : fallback', decode)
        self.assertIn('value(w[1], 10.0f, 90.0f, s.noonAltitudeDeg)', decode)
        self.assertEqual(len(re.findall(r'value\(w\[[2-5] \+ 4 \* k\], 0\.0f, [13]\.0f, key\.\w+\)', decode)), 4)
        self.assertIn('s.pathSet = (w[0] & 4u) != 0;', decode)
        self.assertIn('s.pathNorth = float((w[0] >> 8) & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;', decode)
        self.assertIn('(s.pathSet ? 4u : 0u) | (pathWord << 8)', BLOCK)

    def test_both_ends(self):
        publish = FRAME.index('view.publishSun(warvk::diag::EncodeSunStyle(g.settings.sunStyle));')
        self.assertLess(FRAME.index('view.publish(words);'), publish)
        self.assertIn('diag::DecodeSunStyle(sunStyle, s.classView.readSun());', HOST)
        self.assertIn('settings->sunStyle = sunStyle;', INPUT)

    def test_seqlock(self):
        read = BLOCK[BLOCK.index('SunStyleWords readSun() {'):]
        read = read[:read.index('\n  }\n')]
        self.assertIn('if (!(seq & 1))', read)
        self.assertIn('if (seq == InterlockedCompareExchange(p + SunStyleWordCount, 0, 0)) lastSun = next;', read)


class Panel(unittest.TestCase):
    def test_dial_uses_the_renderers_model(self):
        disc = PANEL[PANEL.index('void DrawSunDialDisc(War3RenderSettings& s)'):]
        disc = disc[:disc.index('\nvoid DrawSunCurveBar(')]
        self.assertIn('War3SunCycle::Evaluate(', disc)
        self.assertIn('castAltitudeRad', disc)

    def test_length_scales_off_while_real_length(self):
        self.assertIn('BeginDisabled(settings.sunStyle.realShadowLength)', PANEL)

    def test_sandbox_switch_only(self):
        self.assertIn('L"WARVK_D3D9_PANEL_TEST_OPEN"', PANEL)
        # The sun dial's and the water section's (=water), each behind its switch.
        self.assertEqual(PANEL.count('ImGui::SetScrollHereY('), 2)
        self.assertIn('if (SunDialTest())\n    ImGui::SetScrollHereY(0.0f);', PANEL)
        self.assertIn('if (WaterPanelTest())\n            ImGui::SetScrollHereY(0.0f);', PANEL)


if __name__ == '__main__':
    unittest.main()
