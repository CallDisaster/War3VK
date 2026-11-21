"""Wiring checks for the water under the shadow receiver (x64 proxy mode).

The game draws its water without writing depth, so the receiver used to shadow
the bed under it. The x86 producer sends the water draws as ShadowCaster
records; the host replays them for the surface depth; the receiver fades the
ground's shadow with the water's depth and writes what the water pass reads.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D3D9 = ROOT / "src/d3d9"


def read(path):
    return Path(path).read_text(encoding="utf-8")


SCENE = read(D3D9 / "war3/effects/war3_shadow_scene.h")
INPUT = read(D3D9 / "war3/effects/war3_shadow_scene_input.h")
CASTERS = read(D3D9 / "war3/platform/war3_proxy_casters.cpp")
SHADOW = read(D3D9 / "d3d9_war3_shadow.cpp")
SHADOW_H = read(D3D9 / "d3d9_war3_shadow.h")
FRAG = read(ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag")
BLOCK = read(D3D9 / "war3/platform/war3_proxy_diag_block.h")
HOST = read(ROOT / "tools/render_host/dxvk_core/shadow_before_ui.cpp")


class FrozenFrontAcceptsTheRecord(unittest.TestCase):
    def test_no_flag_bit_beyond_the_front_mask(self):
        # The f8 front decodes every record with the mask it was built with;
        # one more bit and it refuses the record and the producer stops.
        self.assertIn("constexpr uint32_t All = (1u << 11) - 1;", SCENE)
        self.assertNotIn("WaterSurface = 1u <<", SCENE)

    def test_water_marked_by_identity_words(self):
        self.assertIn('constexpr uint32_t WaterSurfaceRawcode = 0x7E574154u; // "~WAT"', SCENE)
        self.assertIn("constexpr uint32_t WaterSurfacePart = 0xFFFFFFFFu;", SCENE)
        send = CASTERS[CASTERS.index("HRESULT SendWater("):]
        send = send[:send.index("\n}\n")]
        self.assertIn("record.rawcode = scene::WaterSurfaceRawcode;", send)
        self.assertIn("record.part = scene::WaterSurfacePart;", send)
        self.assertIn("record.stage = -1;", send)
        self.assertIn("record.batchTag = -1;", send)
        self.assertIn("record.category = uint32_t(Category::Unknown);", send)
        # Counted like every record, so the host's completeness check holds.
        self.assertIn("++c.sent;", send)

    def test_host_diverts_before_the_casters(self):
        append = INPUT[INPUT.index("inline ShadowCasterRefusal AppendShadowCaster("):]
        self.assertLess(append.index("if (scene::IsWaterSurfaceRecord(record)) {"),
                        append.index("War3ShadowInstanceRef instance = {};"))

    def test_water_before_the_caster_gates(self):
        draw = CASTERS[CASTERS.index("HRESULT CastersDraw("):]
        self.assertLess(draw.index("return SendWater(device, facts);"),
                        draw.index("render::ShadowProducerPolicyAllows("))
        # Native CTerrain::SubmitWaterDraw (0x737960) scope must gate SendWater so
        # transparent billboard particle emitters (stage == -1, SRCALPHA/INVSRCALPHA,
        # 2 triangles per 4 vertices, identity world) are never hijacked as water.
        self.assertIn("War3RenderState::IsWaterRendering() && IsWaterDraw(device, facts)", draw)
        render_hook = read(D3D9 / "war3/hooks/war3_hook_render.cpp")
        self.assertIn("Hook_Water_SubmitDraw", render_hook)
        self.assertIn("War3RenderState::OnWaterEnter()", render_hook)
        self.assertIn("War3RenderState::OnWaterExit()", render_hook)
        book = read(D3D9 / "war3/hooks/war3_hook_address_book.cpp")
        self.assertIn("0x737960, // waterSubmitDraw", book)


class ReceiverFade(unittest.TestCase):
    def test_bindings_and_views(self):
        self.assertIn("layout(set = 1, binding = 17) uniform texture2D s_waterDepth;", FRAG)
        self.assertIn("layout(set = 1, binding = 18, rg16f) uniform image2D s_waterInfoWrite;", FRAG)
        # DXVK encodes a view's descriptor from its usage: one view per kind.
        self.assertIn("descriptors[18].descriptor = water ? m_waterInfoStorageView->getDescriptor() : nullptr;",
                      SHADOW)
        self.assertIn("frame.waterInfo = m_waterInfoView;", SHADOW)
        self.assertIn("Rc<DxvkImageView> m_waterInfoStorageView;", SHADOW_H)

    def test_fade_keeps_shallow_and_drops_deep(self):
        self.assertIn("waterKeep = 1.0 - smoothstep(fadeStart, fadeEnd, waterColumn);", FRAG)
        self.assertIn("vis = mix(1.0, vis, mix(deepShadow, 1.0, waterKeep));", FRAG)

    def test_one_shadow_evaluation(self):
        # A second call for the surface doubled the shader; the driver's
        # compile of the receiver then ran past the host's 5 s reply deadline
        # (shadow gate i7). The point is lifted instead; dry pixels keep theirs.
        self.assertEqual(2, FRAG.count("computeShadowVisibility("))  # the definition and one call
        self.assertIn("shadowPos = mix(worldPos, surfacePos, 1.0 - waterKeep);", FRAG)
        self.assertIn("if (waterPixel && waterKeep < 1.0) {", FRAG)
        self.assertIn("vec3 shadowPos = worldPos;", FRAG)

    def test_settings_reach_the_host(self):
        self.assertIn("constexpr uint32_t Version = 6u;", BLOCK)
        self.assertIn("diag::DecodeWater(water, s.classView.readWater());", HOST)

    def test_water_pass_between_shadows_and_volumetric(self):
        run = HOST[HOST.index("ShadowOutcome ShadowBeforeUi::run("):]
        self.assertLess(run.index("m_state->pass->Run(list, input);"), run.index("m_state->water->Run(list, input);"))
        self.assertLess(run.index("m_state->water->Run(list, input);"), run.index("m_state->volumetric->Run(list, input);"))



class HostBuildBoundary(unittest.TestCase):
    def test_gate_allows_the_water_pass_and_nothing_more(self):
        core = read(ROOT / "AutoTest/run_render_host_core_gate.py")
        self.assertIn("WATER_PASS_SOURCES = ('src/d3d9/d3d9_war3_water.cpp', 'tools/render_host/dxvk_core/water_in_place.cpp')", core)
        self.assertEqual(2, core.count("SHADOW_PASS_SOURCES + VOLUMETRIC_PASS_SOURCES + WATER_PASS_SOURCES"))
        host = read(ROOT / "tools/render_host/dxvk_core/meson.build")
        self.assertEqual(1, host.count("'../../../src/d3d9/d3d9_war3_water.cpp'"))
        self.assertEqual(1, host.count("'water_in_place.cpp'"))

    def test_same_checked_caster_reaches_the_receiver_with_its_original_span(self):
        main = read(ROOT / "tools/render_host/dxvk_core/d3d9_host_main.cpp")
        self.assertIn("d9::scene::DecodeShadowCaster(data, bytes, annotatedCasters[index])", main)
        self.assertIn("d9::scene::IsWaterSurfaceRecord(annotatedCasters[index])", main)
        deliver = main.split("void deliverAnnotations(", 1)[1].split("void clearScene()", 1)[0]
        self.assertIn("caster=annotatedCasters[index];hasCaster=true;", deliver)
        self.assertIn("gpu->addShadowCaster(states[casterIndex],caster,hasAlpha?&alpha:nullptr", deliver)
        self.assertIn("sceneTracker.generation+1,casterSpans[casterIndex]", deliver)
        self.assertNotIn("IsWaterSurfaceRecord", deliver)


class EmissiveLayersKeepTheirAmbient(unittest.TestCase):
    # The Infernal's glow (material D=0 A=0 E=(0.8,1,0)) turned pale yellow
    # under the world lighting's white material ambient.
    FRAME = read(D3D9 / "war3/platform/war3_proxy_frame.cpp")

    def test_exemption_per_draw_before_the_front_sends_it(self):
        helper = self.FRAME[self.FRAME.index("static void EmissiveOnlyDraw(IDirect3DDevice9* device) {"):]
        helper = helper[:helper.index("\n}\n")]
        self.assertIn("m.Diffuse.r == 0.0f && m.Diffuse.g == 0.0f && m.Diffuse.b == 0.0f", helper)
        self.assertIn("const bool hasGameAmbient = m.Ambient.r > 0.0f || m.Ambient.g > 0.0f || m.Ambient.b > 0.0f;", helper)
        self.assertIn("want = (zeroDiffuse || hasGameAmbient) ? 0u : 1u;", helper)
        self.assertIn("D3DRS_AMBIENTMATERIALSOURCE", helper)
        self.assertIn("War3RenderState::IsWaterRendering()", helper)
        self.assertIn("lighting.materialAmbientWhite = want;", helper)
        self.assertIn("lighting.ambientOverride = wantOverride;", helper)
        draw = self.FRAME[self.FRAME.index("void FrameDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {"):]
        self.assertLess(draw.index("WorldDrawLighting(device);"), draw.index("CastersDraw(device, facts)"))
        world = self.FRAME[self.FRAME.index("static void WorldDrawLighting(IDirect3DDevice9* device) {"):]
        world = world[:world.index("\n}\n")]
        self.assertTrue(world.rstrip().endswith("EmissiveOnlyDraw(device);"))

    def test_held_state_follows_what_was_sent(self):
        send = self.FRAME[self.FRAME.index("void SendWorldLighting(bool active) {"):]
        send = send[:send.index("\n}\n")]
        self.assertIn("g.worldLightingHeld = false;", send)
        self.assertIn("g.worldLightingHeld = hr == S_OK;", send)
        self.assertIn("g.ambientWhiteHeld = lighting.materialAmbientWhite;", send)
        self.assertIn("g.ambientOverrideHeld = lighting.ambientOverride;", send)
        # A scene starts and ends with the world lighting itself, not set aside.
        self.assertLess(send.index("g.overlayHeld = false;"), send.index("if (!active) {"))


class CameraSpaceOverlaysKeepTheGameLighting(unittest.TestCase):
    # The cinematic filter is a lit quad drawn in the world scene in camera
    # space, its ambient from its vertex colour: forced to the white material
    # ambient, a fade to black went to our ambient's grey or blue (campaign
    # cinematics, the start of a map), the shadows drawn over it.
    FRAME = read(D3D9 / "war3/platform/war3_proxy_frame.cpp")

    def test_identity_view_is_an_overlay(self):
        helper = self.FRAME[self.FRAME.index("static bool CameraSpaceDraw(IDirect3DDevice9* device) {"):]
        helper = helper[:helper.index("\n}\n")]
        self.assertIn("device->GetTransform(D3DTS_VIEW, &view)", helper)
        self.assertIn("if (view.m[r][c] != (r == c ? 1.0f : 0.0f))", helper)

    def test_overlay_gets_no_world_lighting_and_the_rest_gets_it_back(self):
        world = self.FRAME[self.FRAME.index("static void WorldDrawLighting(IDirect3DDevice9* device) {"):]
        world = world[:world.index("\n}\n")]
        self.assertIn("if (!g.worldLightingHeld)\n    return;", world)
        self.assertIn("if (!g.overlayHeld && SetWorldLighting(nullptr) == S_OK) {", world)
        # Back to the world lighting as it was sent, white ambient included.
        self.assertIn("WarVKD3D9WorldLighting lighting = g.worldLighting;", world)
        self.assertIn("lighting.materialAmbientWhite = g.ambientWhiteHeld;", world)
        self.assertLess(world.index("CameraSpaceDraw(device)"), world.index("EmissiveOnlyDraw(device);"))

    def test_no_diagnostic_left_in_the_draw_path(self):
        self.assertNotIn("WARVK_D3D9_DRAW_PROBE", self.FRAME)


if __name__ == "__main__":
    unittest.main()
