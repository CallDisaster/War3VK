"""Wiring checks for the saved Ctrl+F1 panel settings (WarVK\\settings.ini).

The C++ test (test_player_settings.cpp) proves the codec, file I/O and store.
This file pins how the product uses it: load order in the pipeline, which
panel controls are saved, and that only the panel records player choices.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


PIPELINE = source('src/d3d9/d3d9_war3_pipeline.cpp')
# The environment overrides of the constructor: one implementation, shared
# with proxy mode (phase 5.1-c).
RESOLVER = source('src/d3d9/war3/render/war3_settings_resolver.cpp')
PANEL = source('src/d3d9/war3/ui/war3_imgui.cpp')
MODULE = source('src/d3d9/war3/platform/war3_player_settings.cpp')
HEADER = source('src/d3d9/war3/platform/war3_player_settings.h')
MESON = source('src/d3d9/meson.build')

# Written by the panel but deliberately not saved: a debug view and two values
# derived from the TAA mode (re-derived whenever the mode is loaded or reset).
NOT_SAVED = {
    'shadows.debugMode',
    'shadows.shadowTaaEnabled',
    'shadows.shadowTaaSettingsRevision',
}


def settings_block():
    start = PANEL.index('auto &settings = *settingsWrite;')
    end = PANEL.index('player::RecordPanelEdit(settingsBefore, settings);')
    return PANEL[start:end]


def function_body(text, signature):
    start = text.index('{', text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start + 1:end - 1]


def sun_dial_writes():
    # The sun dial (12.17) edits the settings it is handed as `s`, the sun
    # style through `auto& style = s.sunStyle;`. Only writes count: the dial
    # also reads the game clock to draw the current time.
    dial = PANEL[PANEL.index('float DialCurrentHours(const War3RenderSettings& s)'):]
    dial = dial[:dial.index('void DrawSunDial(War3RenderSettings& s)')] + \
        function_body(PANEL, 'void DrawSunDial(War3RenderSettings& s)')
    dial = re.sub(r'"(?:\\.|[^"\\])*"', '""', dial)
    found = set()
    for prefix, member in re.findall(r'&(s|style)\.([A-Za-z_][A-Za-z0-9_.]*)', dial) + \
            re.findall(r'(?<![A-Za-z0-9_.])(s|style)\.([A-Za-z_][A-Za-z0-9_.]*)\s*=[^=]', dial):
        found.add(('sunStyle.' if prefix == 'style' else '') + member)
    return found


def panel_members():
    # Drop string literals (labels mention settings.ini), then join member
    # chains that the formatter split across lines.
    block = re.sub(r'"(?:\\.|[^"\\])*"', '""', settings_block())
    block = re.sub(r'\s*\.\s*', '.', block)
    found = set()
    for match in re.finditer(r'(?<![A-Za-z0-9_])settings\.([A-Za-z_][A-Za-z0-9_.]*)', block):
        found.add(match.group(1).rstrip('.'))
    if 'DrawSunDial(settings);' in block:
        found |= sun_dial_writes()
    return found


def saved_members():
    found = {m.group(2) for m in re.finditer(
        r'\bF(?:_BOOL|_POW2)?\("([^"]+)",\s*([A-Za-z0-9_.]+)', MODULE)}
    # The six keys of the sun tint curve are saved by one macro, four fields each.
    if re.search(r'#define WARVK_SUN_KEY\(name, index\)', MODULE):
        found.add('sunStyle.tint')
    return found


def saved_keys():
    return [m.group(1) for m in re.finditer(
        r'\bF(?:_BOOL|_POW2)?\("([^"]+)",\s*[A-Za-z0-9_.]+', MODULE)]


class PipelineOrder(unittest.TestCase):
    def test_saved_choices_after_vram_tier_before_env_and_mailbox(self):
        ctor = PIPELINE.split('War3RenderPipeline::War3RenderPipeline(', 1)[1]
        vram = ctor.index('CurrentVramPreset()')
        player = ctor.index('war3::player::ApplyAtStartup(m_settings, extras)')
        env = ctor.index('ApplyWar3SettingsEnvironmentOverrides(m_settings);')
        overrides = RESOLVER.split('void ApplyWar3SettingsEnvironmentOverrides(', 1)[1]
        self.assertIn('"DXVK_WAR3_SHADOW_DEBUG"', overrides)
        seed = ctor.index('m_settingsMailbox->pending = m_settings;')
        self.assertLess(vram, player)
        self.assertLess(player, env)
        self.assertLess(env, seed)

    def test_extras_are_applied_back(self):
        block = PIPELINE.split('war3::player::Extras extras;', 1)[1].split('Logger::info', 1)[0]
        self.assertIn('extras.fpsUnlocked = war3::War3Imgui::isFpsUnlocked();', block)
        self.assertIn('war3::War3Imgui::setFpsUnlocked(extras.fpsUnlocked);', block)
        self.assertIn('War3RenderState::SetNativeShadowMode(extras.nativeShadowMode);', block)

    def test_pipeline_only_reads(self):
        for name in ('FlushIfDue', 'RecordPanelEdit', 'WriteTextFileAtomically'):
            self.assertNotIn(name, PIPELINE)


class PanelRecording(unittest.TestCase):
    def test_in_place_water_defaults_ranges_and_panel_bindings(self):
        settings = source('src/d3d9/d3d9_war3_settings.h').split('struct War3WaterSettings {', 1)[1].split('};', 1)[0]
        panel = settings_block().split('TreeNode("水面##panel")', 1)[1].split('ImGui::TreePop()', 1)[0]
        for key, member, hi in (('InPlace', 'inPlace', 2), ('Style', 'style', 1),
                                ('DepthColor', 'depthColor', 2), ('Foam', 'foam', 2),
                                ('Refraction', 'refraction', 2), ('Caustics', 'caustics', 2)):
            with self.subTest(key=key):
                self.assertIn(f'F("Water.{key}", water.{member}, 0, {hi})', MODULE)
                if member == 'inPlace':
                    self.assertIn('int inPlace = 2;', settings)
                    self.assertRegex(panel, r'ImGui::SliderInt\("[^"]+", &settings\.water\.inPlace, 0, 2,')
                else:
                    self.assertIn(f'float {member} = 1.0f;', settings)
                    self.assertRegex(panel, rf'ImGui::SliderFloat\("[^"]+", &settings\.water\.{member}, 0\.0f, {hi}\.0f,')

    def test_water_lighting_sliders_and_post_clarity_scope(self):
        water = function_body(settings_block(), 'if (ImGui::TreeNode("水面##panel"))')
        post = 'if (ImGui::TreeNode("后合成（原位水面关闭时）"))'
        self.assertEqual(re.findall(r'&settings\.water\.(\w+)', function_body(water, post)), ['clarity'])
        self.assertEqual(water.count('&settings.water.clarity'), 1)
        settings = function_body(source('src/d3d9/d3d9_war3_settings.h'), 'struct War3WaterSettings')
        for label, key, member, hi, default in (('水面反射', 'Reflection', 'reflection', 1, '0.6'),
                                                ('阳光闪烁', 'Specular', 'specular', 2, '1.0'),
                                                ('水底焦散', 'Caustics', 'caustics', 2, '1.0')):
            with self.subTest(key=key):
                self.assertIn(f'float {member} = {default}f;', settings)
                self.assertIn(f'F("Water.{key}", water.{member}, 0, {hi})', MODULE)
                self.assertIn(f'ImGui::SliderFloat("{label}##water", &settings.water.{member}, 0.0f, {hi}.0f,',
                              water[:water.index(post)])

    def test_diff_taken_inside_one_settings_guard(self):
        guard = PANEL.index('auto settingsWrite = dxvk::war3::GetMutableSettings();')
        before = PANEL.index('const War3RenderSettings settingsBefore = settings;')
        record = PANEL.index('player::RecordPanelEdit(settingsBefore, settings);')
        next_header = PANEL.index('CollapsingHeader("Vulkan ShaderPack")')
        self.assertLess(guard, before)
        self.assertLess(before, record)
        self.assertLess(record, next_header)
        self.assertIn('if (recordSettings)\n          player::RecordPanelEdit', PANEL)

    def test_debug_and_reset_frames_are_not_recorded(self):
        debug = PANEL.split('"调试：强制所有对象描边"', 1)[1].split('SetOutlineDebugAllObjectsEnabled', 1)[0]
        self.assertIn('recordSettings = false;', debug)
        reset = PANEL.split('ImGui::Button("恢复默认设置")', 1)[1].split('ImGui::SameLine();', 1)[0]
        self.assertIn('player::ResetToBaseline(settings, extras)', reset)
        self.assertIn('recordSettings = false;', reset)
        self.assertIn('recordExtras = false;', reset)

    def test_every_panel_setting_is_saved(self):
        members = panel_members()
        self.assertGreater(len(members), 100)
        missing = sorted(members - saved_members() - NOT_SAVED)
        self.assertEqual(missing, [], 'panel controls without a saved key')

    def test_nothing_saved_that_the_panel_cannot_change(self):
        extra = sorted(saved_members() - panel_members())
        self.assertEqual(extra, [])

    def test_sun_dial_controls_saved(self):
        self.assertIn('DrawSunDial(settings);', settings_block())
        self.assertEqual(sun_dial_writes(), {
            'shadows.lockSun', 'shadows.lockSunTime', 'sunStyle.realShadowLength',
            'sunStyle.noonAltitudeDeg', 'sunStyle.customTint', 'sunStyle.tint',
            'sunStyle.pathSet', 'sunStyle.pathNorth'})
        macro = MODULE[MODULE.index('#define WARVK_SUN_KEY(name, index)'):MODULE.index('#undef WARVK_SUN_KEY')]
        for field, high in (('r', 1), ('g', 1), ('b', 1), ('intensity', 3)):
            self.assertRegex(macro, r'sunStyle\.tint\[index\]\.' + field + r', 0, ' + str(high) + r'\)')
        uses = re.findall(r'^\s*WARVK_SUN_KEY\("([A-Za-z]+)", (\d)\),$', macro, re.M)
        self.assertEqual([int(i) for _, i in uses], list(range(6)))
        self.assertEqual(len({name for name, _ in uses}), 6)
        self.assertIn('static constexpr size_t KeyCount = 6;', source('src/d3d9/d3d9_war3_settings.h'))

    def test_debug_controls_stay_unsaved(self):
        for member in NOT_SAVED:
            self.assertNotIn(member, saved_members())
        self.assertNotIn('kStageDebug', MODULE)
        self.assertNotIn('kShadowRenderGroup', MODULE)

    def test_keys_are_unique_and_stable_shape(self):
        keys = saved_keys()
        self.assertEqual(len(keys), len(set(keys)))
        for key in keys:
            self.assertRegex(key, r'^[A-Z][A-Za-z]*\.[A-Z][A-Za-z0-9]*$')

    def test_extras_recorded_and_flushed(self):
        draw = PANEL.split('void War3Imgui::drawDebugWindow()', 1)[1].split('void War3Imgui::drawRenderStatsPanel()', 1)[0]
        self.assertIn('const player::Extras extrasBefore = currentExtras();', draw)
        tail = draw.rsplit('ImGui::End();', 1)[1]
        self.assertIn('player::RecordExtrasEdit(extrasBefore, currentExtras());', tail)
        self.assertIn('player::FlushIfDue(!m_visible);', tail)
        toggle = PANEL.split('void War3Imgui::toggleVisible()', 1)[1].split('bool War3Imgui::wantsCaptureMouse', 1)[0]
        self.assertIn('player::FlushIfDue(true);', toggle)
        shutdown = PANEL.split('void War3Imgui::shutdown(', 1)[1].split('beginDeviceTeardown(owner);', 1)[0]
        self.assertIn('player::FlushIfDue(true);', shutdown)


class OnlyThePanelRecords(unittest.TestCase):
    def test_no_other_writer_records_player_choices(self):
        for path in (ROOT / 'src').rglob('*.cpp'):
            text = path.read_text(encoding='utf-8', errors='replace')
            rel = path.relative_to(ROOT).as_posix()
            if rel in ('src/d3d9/war3/ui/war3_imgui.cpp',
                       'src/d3d9/war3/platform/war3_player_settings.cpp'):
                continue
            for name in ('RecordPanelEdit', 'RecordExtrasEdit', 'ResetToBaseline'):
                self.assertNotIn(name, text, f'{rel} calls {name}')


class FileAndBuild(unittest.TestCase):
    def test_separate_file_from_hand_written_warvk_ini(self):
        self.assertIn('L"\\\\WarVK\\\\settings.ini"', MODULE)
        self.assertNotIn('warvk.ini', MODULE)
        self.assertNotIn('WritePrivateProfile', MODULE)
        self.assertIn('MOVEFILE_REPLACE_EXISTING', MODULE)

    def test_save_delay_and_built(self):
        self.assertIn('constexpr uint64_t kSaveDelayMs = 1500u;', HEADER)
        self.assertIn("'war3/platform/war3_player_settings.cpp',", MESON)
        self.assertIn("test('player_settings', player_settings_test", MESON)


if __name__ == '__main__':
    unittest.main()
