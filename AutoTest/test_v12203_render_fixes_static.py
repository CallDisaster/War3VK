"""Source contracts for the 1.22.03 render fixes.

Pixels are proven separately in the isolated sandbox; these tests keep the
wiring from silently coming apart:
  - sun/ambient rewrites are scoped to the main world scene, so the unit
    portrait keeps the game's own lighting;
  - the BeforeUi passes are inserted at the end of the world scene, which a
    paused frame also has, and the frame-end fallback stays out once
    something was drawn after the world;
  - the volumetric shafts sample a finer shadow map and guide.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8").replace("\r\n", "\n")


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin + len(start))]


DEVICE = read("src/d3d9/d3d9_device.cpp")
DEVICE_H = read("src/d3d9/d3d9_device.h")
HOOK = read("src/d3d9/war3/hooks/war3_hook_render.cpp")
SCOPE = read("src/d3d9/war3/render/war3_world_lighting_scope.h")
BOUNDARY = read("src/d3d9/war3/render/war3_before_ui_boundary.h")
GUIDE = read("src/d3d9/war3/render/war3_volumetric_guide_divisor.h")
STATE = read("src/d3d9/war3/render/war3_render_state.cpp")
STATE_H = read("src/d3d9/war3/render/war3_render_state.h")
SETTINGS = read("src/d3d9/d3d9_war3_settings.h")
SHADOW = read("src/d3d9/d3d9_war3_shadow.cpp")
VOLUMETRIC = read("src/d3d9/d3d9_war3_volumetric_light.cpp")
MESON = read("src/d3d9/meson.build")


class WorldLightingScope(unittest.TestCase):
    def test_scope_contract(self):
        self.assertIn("Unscoped = 0", SCOPE)
        self.assertIn("return scope != War3WorldLightingScope::Outside;", SCOPE)
        self.assertIn(
            "return War3WorldLightingActive(from) != War3WorldLightingActive(to);",
            SCOPE)

    def test_hook_brackets_the_world_scene(self):
        scene = between(HOOK, "int __fastcall Hook_WorldRenderScene(",
                        "int __fastcall Hook_SceneSubmitBatch(")
        enter = scene.index("native_light::BeginWorld();")
        on = scene.index("SetDeviceWorldLightingScope(true);")
        native = scene.index("result = g_trampolineWorldRenderScene(thisPtr, edx);")
        end = scene.index("native_light::EndWorld();")
        off = scene.index("SetDeviceWorldLightingScope(false);")
        self.assertLess(enter, on)
        self.assertLess(on, native)
        self.assertLess(native, end)
        self.assertLess(end, off)
        # The pass-through build (boundary hooks compiled out) must not touch
        # the scope: the device then stays Unscoped and rewrites as before.
        legacy = scene[:scene.index("War3RenderState::OnWorldRenderSceneEnter();")]
        self.assertNotIn("SetDeviceWorldLightingScope", legacy)
        self.assertEqual(scene.count("SetDeviceWorldLightingScope("), 2)
        helper = between(HOOK, "static void SetDeviceWorldLightingScope(bool inWorld)",
                         "int __fastcall Hook_WorldRenderScene(")
        self.assertIn("RunWithActiveDevice", helper)
        self.assertIn("device.War3SetWorldLightingScope(inWorld);", helper)

    def test_material_keeps_the_game_value(self):
        material = between(DEVICE, "D3D9DeviceEx::SetMaterial(const D3DMATERIAL9 *pMaterial)",
                           "D3DMATERIAL9 D3D9DeviceEx::War3EffectiveMaterial(")
        self.assertIn("m_war3PureMaterial = *pMaterial;", material)
        self.assertIn("War3EffectiveMaterial(*pMaterial, War3WorldLightingActive())", material)
        self.assertNotIn("Ambient.r = 1.0f", material)
        effective = between(DEVICE, "D3DMATERIAL9 D3D9DeviceEx::War3EffectiveMaterial(",
                            "D3DLIGHT9 D3D9DeviceEx::War3EffectiveLight0(")
        self.assertIn("if (m_war3Pipeline && worldLighting) {", effective)
        self.assertEqual(DEVICE.count("finalMat.Ambient.r = 1.0f;"), 1)

    def test_light0_is_rewritten_only_in_world_scope(self):
        effective = between(DEVICE, "D3DLIGHT9 D3D9DeviceEx::War3EffectiveLight0(",
                            "bool D3D9DeviceEx::War3EffectiveAmbient(")
        guard = effective.index("if (!m_war3Pipeline || !worldLighting)\n    return finalLight;")
        self.assertLess(guard, effective.index("m_war3Pipeline->GetSettings().sun"))
        light = between(DEVICE, "D3D9DeviceEx::SetLight(DWORD Index,", "D3D9DeviceEx::GetLight(")
        self.assertIn("m_war3PureLight0 = *pLight;", light)
        self.assertIn("m_war3PureLight0Valid = true;", light)
        # The sun rewrite exists exactly once, in the helper.
        self.assertEqual(DEVICE.count("finalLight.Direction = {sun.direction.x"), 1)
        self.assertNotIn("sun.direction", light)

    def test_ambient_and_its_source_are_restored(self):
        ambient = between(DEVICE, "if (unlikely(State == D3DRS_AMBIENT)) {",
                          "const DWORD oldValue = states[State];")
        self.assertIn("m_pureGameAmbient = Value;", ambient)
        self.assertIn("War3EffectiveAmbient(Value, War3WorldLightingActive())", ambient)
        self.assertIn("War3ApplyAmbientSourceForWorldLighting(overrideActive);", ambient)
        self.assertIn("m_war3PureAmbientSource = Value;", ambient)
        source = between(DEVICE, "void D3D9DeviceEx::War3ApplyAmbientSourceForWorldLighting(",
                         "void D3D9DeviceEx::War3ReapplyWorldLighting()")
        self.assertIn("m_war3AmbientSourceForced = true;", source)
        self.assertIn("source = m_war3PureAmbientSource;", source)
        effective = between(DEVICE, "bool D3D9DeviceEx::War3EffectiveAmbient(",
                            "void D3D9DeviceEx::War3ApplyAmbientSourceForWorldLighting(")
        self.assertIn("!m_war3Pipeline || !worldLighting ||", effective)

    def test_scope_switch_reapplies_from_pure_values(self):
        switch = between(DEVICE, "void D3D9DeviceEx::War3SetWorldLightingScope(bool inWorld)",
                         "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaterial(")
        self.assertLess(switch.index("D3D9DeviceLock lock = LockDevice();"),
                        switch.index("m_war3LightingScope = next;"))
        self.assertIn("if (previous == next)\n    return;", switch)
        self.assertIn("War3WorldLightingScopeNeedsReapply(previous, next)", switch)
        self.assertIn("War3ReapplyWorldLighting();", switch)
        reapply = between(DEVICE, "void D3D9DeviceEx::War3ReapplyWorldLighting()",
                          "void D3D9DeviceEx::War3SetWorldLightingScope(bool inWorld)")
        for token in (
            "War3EffectiveMaterial(m_war3PureMaterial, worldLighting)",
            "War3EffectiveLight0(m_war3PureLight0, worldLighting)",
            "DWORD ambient = m_pureGameAmbient;",
            "m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);",
        ):
            self.assertIn(token, reapply)

    def test_reset_forgets_pure_values(self):
        reset = between(DEVICE, "rs[D3DRS_AMBIENT] = 0;", "rs[D3DRS_FOGENABLE] = FALSE;")
        for token in (
            "m_pureGameAmbient = 0;",
            "m_war3PureAmbientValid = false;",
            "m_war3PureAmbientSource = D3DMCS_MATERIAL;",
            "m_war3AmbientSourceForced = false;",
            "m_war3PureMaterialValid = false;",
            "m_war3PureLight0Valid = false;",
        ):
            self.assertIn(token, reset)
        self.assertIn(
            "m_war3LightingScope =\n      war3::render::War3WorldLightingScope::Unscoped;",
            DEVICE_H)


class BeforeUiBoundary(unittest.TestCase):
    def test_contract_header(self):
        self.assertIn("kWar3BeforeUiWorldSceneEndTier = 10u", BOUNDARY)
        self.assertIn(
            "return in.sceneCompleted && !in.sceneActive && in.inWorldThisFrame &&\n"
            "         in.haveWorldInput && !in.alreadyInserted;", BOUNDARY)
        self.assertIn(
            "return legacyOverride || !sceneCompleted || drawsAfterWorldScene == 0u;",
            BOUNDARY)

    def test_scene_end_is_published_by_the_hook(self):
        self.assertIn(
            "g_worldRenderSceneCompletedThisFrame.store(true, std::memory_order_relaxed);",
            between(STATE, "void War3RenderState::OnWorldRenderSceneExit() {", "\n}\n"))
        enter = between(STATE, "void War3RenderState::OnWorldRenderSceneEnter() {", "\n}\n")
        self.assertIn(
            "g_worldRenderSceneCompletedThisFrame.store(false, std::memory_order_relaxed);",
            enter)
        frame = between(STATE, "void War3RenderState::OnFrameStart() {", "\n}\n")
        self.assertIn(
            "g_worldRenderSceneCompletedThisFrame.store(false, std::memory_order_relaxed);",
            frame)

    def test_tier10_outranks_heuristics_and_commits_at_once(self):
        insert = between(DEVICE, "void D3D9DeviceEx::War3MaybeInsertBeforeUi(bool forceFrameEnd) {",
                         "War3PipelineInput input;")
        boundary = insert.index("const bool worldSceneEndBoundary =")
        tiers = insert.index("uiBoundaryTier = 7u;")
        tier10 = insert.index(
            "if (worldSceneEndBoundary)\n"
            "    uiBoundaryTier = war3::render::kWar3BeforeUiWorldSceneEndTier;")
        tail = insert.index("bool semanticSceneTailBoundary = false;")
        frame_end = insert.index("uiBoundaryTier = 9u;")
        self.assertLess(boundary, tiers)
        self.assertLess(tiers, tier10)
        self.assertLess(tier10, tail)
        self.assertLess(tail, frame_end)
        decl = insert[boundary:insert.index(";", insert.index("HasInsertedBeforeUi()", boundary))]
        for token in (
            "!forceFrameEnd &&",
            "war3::render::War3IsWorldSceneEndBoundary(",
            "worldSceneCompleted, War3RenderState::IsWorldRenderSceneActive()",
            "inWorldThisFrame, haveWorldInput",
        ):
            self.assertIn(token, decl)
        self.assertIn(
            "const bool worldSceneCompleted =\n"
            "      War3RenderState::HasCompletedWorldRenderSceneThisFrame();", insert)
        commit = insert[insert.index("const bool shouldInsertBeforeUi ="):]
        self.assertIn(
            "(worldSceneEndBoundary\n"
            "                 ? m_war3Pipeline->ForceBeforeUiInsertion()\n"
            "                 : m_war3Pipeline->NotifyDraw(layer, cat, tag,", commit)

    def test_frame_end_fallback_stays_out_after_later_draws(self):
        insert = between(DEVICE, "void D3D9DeviceEx::War3MaybeInsertBeforeUi(bool forceFrameEnd) {",
                         "War3PipelineInput input;")
        guard = between(insert, "if (forceFrameEnd && isUiBoundaryDraw) {",
                        "const bool shouldInsertBeforeUi =")
        self.assertIn(
            "!war3::render::War3FrameEndFallbackAllowed(\n"
            "        worldSceneCompleted, m_war3DrawsAfterWorldSceneThisFrame,\n"
            "        s_allowFrameEndAfterUi);", guard)
        self.assertIn('env::getEnvVar("DXVK_WAR3_BEFOREUI_FRAME_END_AFTER_UI") == "1"', guard)
        self.assertLess(guard.index("if (refuse) {"), guard.index("return;"))
        count = insert[insert.index("if (!shouldInsertBeforeUi) {"):]
        self.assertIn("++m_war3DrawsAfterWorldSceneThisFrame;", count)
        self.assertEqual(DEVICE.count("m_war3DrawsAfterWorldSceneThisFrame = 0u;"), 1)
        reset = DEVICE.index("m_war3DrawsAfterWorldSceneThisFrame = 0u;")
        self.assertLess(DEVICE.index("m_war3UiDrawSeenThisFrame = false;"), reset)

    def test_diagnostics_are_bounded_or_opt_in(self):
        self.assertIn('env::getEnvVar("DXVK_WAR3_FRAME_SEQUENCE_TRACE") == "1"', STATE)
        note = between(STATE, "void War3RenderState::NoteFrameEvent(uint16_t code) {", "\n}\n")
        self.assertLess(note.index("if (!IsFrameSequenceTraceEnabled())\n    return;"),
                        note.index("g_frameSequenceTrace"))
        self.assertIn("trace.logs < 48u", STATE)
        self.assertIn("static constexpr size_t kCapacity = 96;", STATE)
        trace = between(DEVICE, "void D3D9DeviceEx::War3FinishBeforeUiFrameTrace() {",
                        "void D3D9DeviceEx::War3MaybeInsertBeforeUi(bool forceFrameEnd) {")
        self.assertIn("if (n <= 12u || (n % 4096u) == 0u) {", trace)
        self.assertEqual(DEVICE.count("War3FinishBeforeUiFrameTrace();"), 1)
        self.assertIn("FrameEventBeforeUiCommit = 220", STATE_H)


class VolumetricPrecision(unittest.TestCase):
    def test_volume_sun_shadow_defaults(self):
        defaults = re.search(
            r"struct War3VolumetricLightSettings \{(.*?)\n\};", SETTINGS, re.DOTALL).group(1)
        self.assertIn("uint32_t volumeSunResolution = 2048;", defaults)
        self.assertIn("float volumeSunNearRadiusScale = 0.30f;", defaults)
        self.assertIn("float volumeSunOrthoRadius = 10000.0f;", defaults)
        # The producer clamps and falls back to the same values.
        self.assertIn("std::min<uint32_t>(vol.volumeSunResolution, 2048u), 256u);", SHADOW)
        near = between(SHADOW, "const float nearScale = std::clamp(", "const float radiusNear")
        self.assertIn(": 0.30f,", near)
        self.assertIn("0.20f, 0.85f);", near)
        # World units per shadow texel in the near layer: 2 * radius / texels.
        before = 2.0 * 10000.0 * 0.42 / 1536
        after = 2.0 * 10000.0 * 0.30 / 2048
        self.assertGreater(before / after, 1.8)
        self.assertLess(after, 3.0)
        # The far layer got finer too, so nothing outside the near radius lost.
        self.assertLess(2.0 * 10000.0 / 2048, 2.0 * 10000.0 / 1536)

    def test_guide_divisor_is_the_finest_that_fits(self):
        self.assertIn("for (uint32_t divisor = targetDivisor; divisor < effectDivisor; ++divisor)",
                      GUIDE)
        self.assertIn("guideWidth * guideHeight <= cellBudget / uint64_t(depth)", GUIDE)
        self.assertIn(
            "war3::render::War3SelectDirectionalGuideDivisor(\n"
            "            extent.width, extent.height, guideDepth,\n"
            "            kVolumetricDirectionalGuideTargetDivisor, resolutionDivisor,\n"
            "            kVolumetricDirectionalGuideCellBudget);", VOLUMETRIC)
        # Budgets themselves are unchanged.
        self.assertIn("kVolumetricDirectionalGuideCellBudget = 100'000'000ull", VOLUMETRIC)
        self.assertIn("kVolumetricDirectionalGuideTraversalBudget =\n    350'000'000ull",
                      VOLUMETRIC)
        self.assertIn("kVolumetricFroxelTraversalWorkBudget = 350'000'000ull", VOLUMETRIC)

        def select(width, height, depth, target, effect, budget):
            for divisor in range(target, effect):
                cells = -(-width // divisor) * -(-height // divisor) * depth
                if cells <= budget:
                    return divisor
            return effect

        self.assertEqual(select(1920, 1080, 128, 2, 4, 100_000_000), 2)
        self.assertEqual(select(2560, 1440, 128, 2, 8, 100_000_000), 3)
        self.assertEqual(select(3840, 2160, 128, 2, 8, 100_000_000), 4)


class Registration(unittest.TestCase):
    def test_host_test_is_registered(self):
        self.assertIn("'../../AutoTest/test_render_scope_contracts.cpp'", MESON)
        self.assertIn("test('render_scope_contracts', render_scope_contracts_test, timeout : 30)",
                      MESON)


if __name__ == "__main__":
    unittest.main()
