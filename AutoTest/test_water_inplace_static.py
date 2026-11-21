"""Source contracts for the x64 water draw and its original-draw fallback."""

import math
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tools/render_host/dxvk_core"
FX = ROOT / "subprojects/war3fx/shaders"


def source(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)


def body(text, signature):
    start = text.index("{", text.index(signature)) + 1
    depth = 1
    for end in range(start, len(text)):
        depth += (text[end] == "{") - (text[end] == "}")
        if not depth:
            return text[start:end]
    raise AssertionError(signature)


MAIN = source(HOST / "d3d9_host_main.cpp")
BACKEND = source(HOST / "d3d9_backend.cpp")
WATER = source(HOST / "water_in_place.cpp")
HEADER = source(HOST / "water_in_place.h")
DIAG = source(ROOT / "src/d3d9/war3/platform/war3_proxy_diag_block.h")
VS = source(FX / "war3_water_in_place_vert.vert.in")
FS = source(FX / "war3_water_in_place.frag.in")


def tuning(name):
    return float(re.search(rf"\b{name} = ([\d.]+)", FS)[1])


def tuning_vector(name):
    values = re.search(rf"\b{name} = vec[234]\(([^)]+)\)", FS)[1]
    return tuple(float(value) for value in values.split(","))


class InPlaceWater(unittest.TestCase):
    def test_all_message_shapes_bind_water_before_recording_and_deliver_after(self):
        inspect = body(MAIN, "bool inspectAnnotations(")
        self.assertIn("sceneTracker.active && gpu->waterSceneEnabled()", inspect)
        self.assertRegex(inspect, r"inspectWater && schema == uint32_t\(d9::AnnotationSchema::ShadowCaster\) &&\s*"
                                  r"d9::scene::DecodeShadowCaster\(data, bytes, annotatedCasters\[index\]\)")
        self.assertRegex(inspect, r"if \(d9::scene::IsWaterSurfaceRecord\(annotatedCasters\[index\]\)\)"
                                  r"\s*waterMask \|= 1u << index;")
        apply = body(MAIN, "HRESULT apply(")
        self.assertLess(apply.index("!pendingAnnotations.binds(h)"), apply.index("!inspectAnnotations(waterMask)"))
        for op, masked in (("Draw", "drawBatch(&s,1"), ("DrawFixed", "drawFixedBatch(&state,&fixed,1"),
                           ("DrawBatch", "drawBatch(states.data(),count"),
                           ("DrawFixedDeltaBatch", "drawFixedBatch(fixedBatchStates.data(),fixedBatchFixed.data(),count")):
            with self.subTest(op=op):
                dispatch = body(apply, f"case d9::Op::{op}:")
                draw = f"if (waterMask) gpu->{masked},casterSpans.data(),waterMask);"
                self.assertLess(dispatch.index(draw), dispatch.index("if(annotated)deliverAnnotations("))
                self.assertIn("else gpu->draw", dispatch)
        self.assertIn("case d9::Op::DrawFixedBatch:case d9::Op::DrawFixedDeltaBatch:", apply)

    def test_split_is_after_whole_batch_validation_in_stream_flight_and_settled_profiles(self):
        for name, loops in (("drawFixedBatch", 2), ("drawBatch", 3)):
            with self.subTest(batch=name):
                batch = body(BACKEND, f"void D3D9Backend::{name}(")
                self.assertLess(batch.index("prepareDraw("), batch.index("recordDraw("))
                records = re.findall(r"recordDraw\(states\[i\],\s*prepared\[i\],([^;]+)\);", batch)
                self.assertEqual(len(records), loops)
                self.assertTrue(all(call.endswith("waterMask & (1u << i)") for call in records))
                self.assertIn("spans ? spans + i : nullptr", batch)

    def test_first_mark_freezes_both_targets_even_if_that_draw_falls_back(self):
        bind = body(WATER, "WaterInPlace::Reason WaterInPlace::bind(")
        first = body(bind, "if (m_first)")
        self.assertLess(first.index("m_first = false;"), first.index("snapshot(device, ctx, color, depth)"))
        self.assertIn("m_snapshotResult = Reason::Snapshot;", first)
        self.assertIn("catch (...) { return m_snapshotResult; }", first)
        self.assertLess(bind.index("if (m_snapshotResult != Reason::None) return m_snapshotResult;"),
                        bind.index("if (!fixed) return Reason::DrawState;"))
        copy = body(WATER, "WaterInPlace::Reason WaterInPlace::snapshot(")
        self.assertIn("c.sampleCount != VK_SAMPLE_COUNT_1_BIT || d.sampleCount != VK_SAMPLE_COUNT_1_BIT", copy)
        self.assertIn("c.format != VK_FORMAT_B8G8R8A8_UNORM || d.format != VK_FORMAT_D32_SFLOAT", copy)
        self.assertEqual(copy.count("ctx->copyImage("), 2)
        for dst, aspect, src in (("m_color", "rgb", "color"), ("m_depth", "z", "depth")):
            self.assertIn(f"ctx->copyImage({dst}->image(), {aspect}, {{}}, {src}, {aspect}, {{}},", copy)
        view = body(WATER, "Rc<DxvkImageView> snapshotView(")
        self.assertIn("info.format = source.format;", view)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT", view)

    def test_rejected_pipeline_restores_game_shaders_and_issues_the_same_geometry_once(self):
        draw = body(BACKEND, "void D3D9Backend::recordDraw(")
        geometry = body(draw, "const auto drawGeometry =")
        self.assertIn("prepared.indexCount, 1, state.startIndex, state.baseVertexIndex, 0", geometry)
        self.assertIn("owner.context->drawIndexed(1, &command)", geometry)
        self.assertIn("owner.context->draw(1,&command)", geometry)
        water = body(draw, "if (waterDraw)")
        self.assertIn("owner.water->referenceStyle() && prepared.depth.depthWrite()", water)
        self.assertIn("&& !referenceWritesDepth", water)
        self.assertLess(water.index("readOnly.setDepthWrite(false)"), water.index("try { drawGeometry(); }"))
        self.assertLess(water.index("try { drawGeometry(); }"), water.index("setDepthStencilState(prepared.depth)"))
        self.assertLess(water.index("setDepthStencilState(prepared.depth)"), water.index("finishDraw(reason"))
        self.assertLess(water.index("owner.water->bind("), water.index("try { drawGeometry(); }"))
        self.assertIn("if (counters.getCtr(DxvkStatCounter::CmdDrawCalls) == before) reason = WaterFallback::Pipeline;", water)
        self.assertLess(water.index("finishDraw(reason"), water.index("if (reason == WaterFallback::None) return;"))
        self.assertTrue(water.rstrip().endswith("bindGameShaders();"))
        restore = body(draw, "const auto bindGameShaders =")
        for stage, stock in (("VERTEX", "fixedVertexShader"), ("FRAGMENT", "fixedPixelShader")):
            self.assertRegex(restore, rf"bindShader<VK_SHADER_STAGE_{stage}_BIT>\([^;]+{stock}->GetShader\(\)")
        self.assertTrue(draw.rstrip().endswith("drawGeometry();"))
        self.assertEqual(draw.count("drawGeometry();"), 2)
        original = source(ROOT / "src/dxvk/dxvk_context.cpp")
        generic = body(original, "void DxvkContext::drawGeneric(")
        single = body(generic, "if (count == 1u)")
        self.assertIn("if (this->commitGraphicsState<Indexed, false>())", generic)
        self.assertGreater(single.index("addStatCtr(DxvkStatCounter::CmdDrawCalls, 1u)"),
                           max(single.index("cmdDrawIndexed("), single.index("cmdDraw(")))

    def test_style_zero_keeps_the_original_fixed_function_output_and_blend(self):
        build = source(HOST / "meson.build")
        self.assertIn("ff_stage.replace('void main()', 'void gameFixedFunction()')", build)
        self.assertIn("water_glsl.process(water_stages)", build)
        for shader in (VS, FS):
            self.assertTrue(shader.startswith("@FIXED_FUNCTION@"))
            self.assertRegex(body(shader, "void main()"), r"^\s*gameFixedFunction\(\);")
        self.assertRegex(body(FS, "void main()"), r"^\s*gameFixedFunction\(\);\s*vec4 game = out_Color0;"
                                                 r"\s*if \(water.controls.x == 0.0\)\s*return;")
        self.assertIn("premultiplied(mix(game, ours, water.controls.x))", FS)
        draw = body(BACKEND, "void D3D9Backend::recordDraw(")
        self.assertLess(draw.index("setBlendMode(0, prepared.blend)"), draw.index("if (waterDraw)"))
        water = body(draw, "if (waterDraw)")
        self.assertIn("const bool premultiply = !owner.water->referenceStyle();", water)
        self.assertIn("(!premultiply || sourceOver)", water)
        self.assertIn("setBlendMode(0, blend)", body(water, "if (premultiply)"))
        self.assertIn("if (premultiply) owner.context->setBlendMode(0, prepared.blend);", water)

    def test_depth_foam_and_refraction_use_the_draw_and_foreground_depth(self):
        self.assertIn("in_Position0 * water.world", VS)
        self.assertIn("u.invViewProj = inverse(u.viewProj)", WATER)
        self.assertIn("Matrix4(p.m) * u.view", WATER)
        self.assertIn("vec4(ndc, z, 1) * water.invViewProj", FS)
        self.assertIn("sampleTexture(0u, in_Texcoord0, vec4(0)).rgb * waterVertexColor.rgb", FS)
        self.assertIn("max(0.0, 0.68 * thickness + 0.32 * vertical - ClearWater)", FS)
        self.assertTrue(0 <= tuning("ClearWater") <= 10)
        self.assertIn("vec3(0.021, 0.0080, 0.0042)", FS)
        self.assertIn("exp(-sigma * water.controls.y * opticalDepth)", FS)
        # Foam: soft froth that hugs the waterline, thinning outward. Its reach
        # is some units of column or some pixels on screen, whichever is
        # nearer, so a gentle beach does not widen it into a broad band; no
        # wave lines of its own (the game draws its surf there).
        water_body = body(FS, "vec4 waterBody(")
        # By vertical depth: the slant column keeps a fraction of a pixel's
        # width at the waterline, which on a gentle beach is many pixels out.
        self.assertRegex(water_body, r"if \(water.controls.z > 0.0 && fogMask > 0.0 && vertical < FoamReach \* "
                                     r"\(FoamFade.y \+ FoamWander\)\)")
        # The game's water draws nothing in the last unit or so of depth at a
        # beach (measured 0.7 to 1.3), so the froth rises from there.
        self.assertIn("float pixelsOut = (vertical - FoamEdge) / perPixel;", FS)
        self.assertIn("float reach = max(vertical / FoamReach, pixelsOut / FoamPixels);", FS)
        self.assertTrue(0.5 <= tuning("FoamEdge") <= 1.5)
        self.assertTrue(0 < tuning("FoamReach") <= 16 and 4 <= tuning("FoamPixels") <= 24)
        full, gone = tuning_vector("FoamFade")
        self.assertTrue(0 < full < gone == 1.0)
        # It rises over a few pixels from the waterline whatever the slope.
        self.assertTrue(1 <= tuning("FoamRise") <= tuning("FoamPixels") / 2)
        self.assertTrue(0 < tuning("FoamWander") <= 0.5)
        self.assertIn("smoothstep(0.0, FoamRise * (0.4 + 1.2 * clumps), pixelsOut)", FS)
        self.assertIn("(1.0 - smoothstep(FoamFade.x, FoamFade.y, reach + wander)) * stretch * froth", FS)
        bare, full_stretch = tuning_vector("FoamStretch")
        self.assertTrue(0 < bare < full_stretch < 1)
        self.assertTrue(0 < tuning("FoamBubbles") < 1)
        self.assertIn("mix(FoamBubbles, 1.0, smoothstep(", FS)
        # How fast the water shallows per pixel: the snapshot one pixel over
        # (the water plane is flat), kept inside the viewport.
        taps = body(water_body, "for (int i = 0; i < 2; ++i)")
        self.assertIn("min(pixel + (i == 0 ? ivec2(1, 0) : ivec2(0, 1)), last)", taps)
        self.assertIn("perPixel = max(perPixel, abs(nextBottom.z - bottomWorld.z));", taps)
        self.assertNotIn("dFd", water_body)
        self.assertNotIn("fract(thickness", FS)
        self.assertNotIn("ShoreFoam", FS)
        self.assertEqual(water_body.count("waterNoise(xy *"), 4)
        foam_drifts = re.findall(r"waterNoise\(xy \* [\d.]+ [+-] water.waves.x \* vec2\(([^)]+)\)\)", water_body)
        self.assertEqual(len(foam_drifts), 4)
        for drift in foam_drifts:
            self.assertTrue(all((float(v) * 2).is_integer() for v in drift.split(",")), drift)
        self.assertIn("foam = clamp(water.controls.z * foam, 0.0, 1.0);", FS)
        # A layer over the water: lit froth at its own opacity; open water keeps
        # full refraction coverage while melting cleanly to zero at shoreline alpha.
        self.assertIn("float cover = foam * FoamOpacity * fogMask;", FS)
        self.assertIn("float coverage = mix(waterAlpha, 1.0, cover);", FS)
        self.assertIn("vec4((frothLit * cover + body * waterAlpha * (1.0 - cover)) / max(coverage, 1e-4), coverage)", FS)
        self.assertTrue(0 < tuning("FoamOpacity") <= 0.75)
        self.assertIn("openSurface *= waterAlpha * (1.0 - foam) * smoothstep(0.0, ShoreLight, thickness)", FS)
        self.assertIn("64.0 * water.controls.w", FS)
        self.assertIn("1.0 - exp(-thickness / 48.0)", FS)
        taps = body(body(FS, "vec3 refractedColor("), "for (int i = 0; i < 4; ++i)")
        self.assertRegex(taps, r"(?s)texelFetch\(SNAP\(waterSceneDepth\), tap, 0\).*?"
                               r"if \(!\(along >= max\(0.0, plane\)\)\)\s*return original;")
        self.assertLess(taps.index("return original;"), taps.index("texelFetch(SNAP(waterSceneColor), tap, 0)"))

    def test_scene_target_and_frame_lifetimes_do_not_alias(self):
        self.assertIn("m_first = true;", body(WATER, "void WaterInPlace::beginScene("))
        self.assertIn("m_settings.inPlace == 1 || (m_settings.inPlace == 2 &&", WATER)
        self.assertIn("deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU", WATER)
        self.assertIn("owner.water->resetTargets(owner.context)", body(BACKEND, "void D3D9Backend::reset("))
        self.assertIn("m_enabled = false; m_drewThisFrame = false;", body(HEADER, "void endFrame()"))
        self.assertNotIn("m_drewThisFrame = false", body(WATER, "void WaterInPlace::beginScene("))
        self.assertIn("m_drewThisFrame = true;", body(body(WATER, "void WaterInPlace::finishDraw("),
                                                      "if (reason == Reason::None)"))
        self.assertIn("owner.water && owner.water->drewThisFrame(), m_stats.water.skippedOldPasses",
                      body(BACKEND, "D3D9Backend::BeforeUiOutcome D3D9Backend::runBeforeUi("))

    def test_v4_v5_and_v6_use_only_their_own_water_words_with_one_sequence(self):
        self.assertIn("constexpr uint32_t Version = 6u;", DIAG)
        self.assertLess(DIAG.index("uint32_t water[1 + 8 + 1]"), DIAG.index("alignas(8) uint32_t waterInPlace[8]"))
        self.assertIn("Version4Bytes = offsetof(Block, waterInPlace)", DIAG)
        self.assertIn("constexpr unsigned Version5TailWords = 5;", DIAG)
        self.assertIn("(Version5TailWords * sizeof(uint32_t) + alignof(Block) - 1) / alignof(Block) * alignof(Block)", DIAG)
        mapping = body(DIAG, "inline Block* MapCompatibleBlock(")
        self.assertIn("version == 4u ? Version4Bytes : version == 5u ? Version5Bytes :", mapping)
        self.assertIn("version == Version ? sizeof(Block) : 0", mapping)
        self.assertLess(mapping.index("header[2] == bytes && header[3] == pid"),
                        mapping.index("MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes)"))
        self.assertIn("b->version != version || b->bytes != bytes || b->gamePid != pid", mapping)
        opening = body(DIAG, "void open(DWORD pid)")
        self.assertIn("block = MapCompatibleBlock(mapping, pid)", opening)
        self.assertIn("block->version == 4u ? 0u : block->version == 5u ? Version5TailWords : WaterTailWords", opening)
        self.assertIn("diag::MapCompatibleBlock(mapping, m_gamePid)", source(HOST / "evidence_recorder.cpp"))
        read = body(DIAG, "WaterWords readWater()")
        write = body(DIAG, "void publishWater(")
        for region in (read, write):
            self.assertIn("block->waterInPlace", body(region, "if (waterTailWords)"))
            self.assertLess(region.index("p + LegacyWaterWords"), region.index("block->waterInPlace"))
            self.assertGreater(region.rindex("p + LegacyWaterWords"), region.index("block->waterInPlace"))
        self.assertIn("else next[0] &= ~InPlaceWaterWordsPresent", read)
        self.assertIn("lastWater = next", read)
        codec = body(DIAG, "inline void DecodeWater(")
        self.assertLess(codec.index("s = Water{}"), codec.index("if (w[0] & InPlaceWaterWordsPresent)"))
        fields = ("inPlace", "style", "depthColor", "foam", "refraction", "caustics")
        for i, field in enumerate(fields, 9):
            self.assertRegex(body(DIAG, "inline WaterWords EncodeWater("), rf"w\[{i}\] = \w+\(s\.{field}\);")
            self.assertRegex(body(codec, "if (w[0] & InPlaceWaterWordsPresent)"), rf"s\.{field} = .*?w\[{i}\]")
        self.assertIn("constexpr unsigned WaterTailSettings = 6;", DIAG)
        self.assertIn("constexpr unsigned WaterTailWords = 8;", DIAG)
        self.assertRegex(write, r"i < waterTailWords; \+\+i\)\s*tail\[i\] = i < WaterTailSettings \? LONG\(words\[LegacyWaterWords \+ i\]\) : 0;")
        self.assertLess(read.index("next[14] = 0x7fc00000u"), read.index("i < std::min(waterTailWords, WaterTailSettings)"))
        self.assertIn("s.caustics = value(w[14], 0.0f, 2.0f, s.caustics)", codec)
        self.assertIn("std::isfinite(v) && v >= lo && v <= hi ? v : fallback", codec)
        self.assertNotRegex(codec, r"w\[(15|16)\]")

    def test_draw_light_camera_and_optics_reach_the_fragment(self):
        fill = body(WATER, "bool WaterInPlace::fillUniform(")
        self.assertIn("fixed.lights[0].Type == D3DLIGHT_DIRECTIONAL", fill)
        self.assertIn("fixed.lightCount &&", fill)
        self.assertIn("const Vector4 toSun(-d.x, -d.y, -d.z, 0)", fill)
        self.assertIn("u.toSun = toSun / len", fill)
        self.assertIn("Vector4(light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, 0)", fill)
        self.assertIn("FixedColor(fixed.render[d9::FFIndex(d9::FFRenderStates, D3DRS_AMBIENT)])", fill)
        self.assertIn("Matrix4 cameraToWorld = transpose(stockInverseView)", fill)
        self.assertIn("cameraToWorld[2] * std::copysign(1.0f, a < b ? zb : za)", fill)
        self.assertIn("u.camera = eye / eye.w", fill)
        self.assertIn("Vector4(-forward.x, -forward.y, -forward.z, 0.0f)", fill)
        self.assertIn("u.forwardX = bearing.x;", fill)
        self.assertIn("u.forwardY = bearing.y;", fill)
        self.assertIn("Vector4(m_settings.reflection, m_settings.specular, m_settings.caustics, 0)", fill)
        self.assertIn("owner.fixedTransforms.inverseView, stockVS, stockFS", BACKEND)
        layout = body(FS, "uniform WaterFragment")
        names = re.findall(r"\b(?:mat4|vec[24])\s+([^;]+);", layout)
        self.assertEqual(", ".join(names).replace(" ", "").split(","),
                         "world invViewProj view viewProj viewport depthRange controls waves forwardXY camera toSun sunColor ambient lightControls".split())
        for name, offset in (("world", 0), ("invViewProj", 64), ("view", 128), ("viewProj", 192),
                             ("viewport", 256), ("depthRange", 272), ("controls", 288), ("time", 304),
                             ("forwardX", 312), ("camera", 320), ("toSun", 336), ("sunColor", 352),
                             ("ambient", 368), ("lightControls", 384)):
            self.assertIn(f"offsetof(Uniform, {name}) == {offset}", HEADER)
        self.assertIn("sizeof(Uniform) == 400", HEADER)

    def test_warped_wave_spectrum_and_continuous_ggx_glints_replace_dot_grid(self):
        main = body(FS, "void main()")
        waves = body(FS, "WaveField evaluateWaves(")
        glint = body(FS, "vec3 surfaceGlints(")
        glint_dir = body(FS, "vec3 glintLightDir(")
        self.assertIn("water.camera.xyz - waterPosition * water.camera.w", main)
        self.assertIn("sunlight * smoothstep(SunRise.x, SunRise.y, water.toSun.z)", main)
        # Hybrid directional swells + wind-advected rotated C2 gradient-noise
        # ripples: integer 256s swell speeds, half-cell/s ripple drifts, and
        # zero crawling domain warps or basket-weave sine lattices.
        self.assertIn("exp(sin(x) - 1.0)", waves)
        self.assertIn("p += d * (-dw * drags[i]);", waves)
        self.assertIn("advectedRippleLayer(", waves)
        self.assertIn("tileableGradNoise(", FS)
        for viscous in ("macroWarp", "eddyWarp", "waterNoiseGrad"):
            self.assertNotIn(viscous, FS)
        # Gradients from an integer hash and a normalize: no sine hash (vendor
        # dependent at large arguments) and no cos/sin per corner.
        grad = body(FS, "vec3 tileableGradNoise(")
        self.assertNotIn("sin(", grad)
        self.assertNotIn("cos(", grad)
        self.assertEqual(grad.count("1664525u"), 6)
        self.assertIn("inversesqrt(max(gx * gx + gy * gy, 1e-4))", grad)
        # Unexplored water (black mask) skips the wave field and the caustics.
        self.assertIn("evaluateWaves(waterPosition.xy, phase, footprint, fogMask > 0.0, glinting)", main)
        # The capillary layers feed only the sparkle: skipped where no glint is drawn.
        self.assertIn("bool glinting = water.lightControls.y > 0.0 && fogGlint > 0.0 && "
                      "any(greaterThan(directLight, vec3(0)));", main)
        self.assertIn("if (glinting && openSurface > 0.0)\n        glint = ", main.replace("\r\n", "\n"))
        self.assertIn("if (glinting && capResolved > 0.0)", waves)
        self.assertNotIn("mod(", grad)
        self.assertIn("& 127", grad)
        self.assertIn("if (!visible || water.waves.y <= 0.0)", waves)
        self.assertRegex(body(FS, "vec4 waterBody("), r"bottomLight = fogOptical > 0.0\s*\? shallowCaustics\(")
        speeds = re.search(r"speeds\[4\]\s*=\s*float\[4\]\(([^)]+)\)", waves)[1]
        for v in speeds.split(","):
            val = float(v)
            self.assertEqual(val, round(val), v)
            self.assertGreater(val, 0.0, v)
        drifts = re.search(r"rDrifts\[4\]\s*=\s*float\[4\]\(([^)]+)\)", waves)[1]
        for v in drifts.split(","):
            val = float(v)
            self.assertEqual(val * 2.0, round(val * 2.0), v)
            self.assertGreater(val, 0.0, v)
        # Fog-of-War / Black-Mask visibility derived from waterVertexColor.rgb
        # keeps unexplored Black Mask pure black and dims explored Fog of War.
        self.assertIn("float vPeak = max(waterVertexColor.r, max(waterVertexColor.g, waterVertexColor.b));", main)
        self.assertIn("float fogMask = smoothstep(0.004, 0.055, vPeak);", main)
        self.assertIn("float fogOptical = fogVis * fogVis;", main)
        # No periodic polka-dot cell sparkle grid.
        for forbidden in ("SparkCell", "SparkRadius", "dotShape", "sparkHash"):
            self.assertNotIn(forbidden, FS)
        # Continuous multi-lobe GGX glints driven by wave normals & Toksvig AA.
        self.assertIn("water.forwardXY", glint_dir)
        self.assertIn("ggxNdf(ndothMacro, aSheen)", glint)
        self.assertIn("ggxNdf(ndothDetail, aRibbon)", glint)
        self.assertIn("ggxNdf(ndothCap, aSpark)", glint)
        self.assertIn("sqrt(wf.microVar)", glint)
        self.assertIn("surfaceGlints(V, Nmacro, Ndetail, wf, Lglint, footprint, directLight, water.lightControls.y)", main)

    def test_schlick_floor_sky_and_negligible_weight_guard(self):
        main = body(FS, "void main()")
        reflected = body(main, "if (slider > 0.0 && fogOptical > 0.0)")
        self.assertIn("floorF = slider * FresnelFloorMax", reflected)
        self.assertIn("floorF + (1.0 - floorF) * pow(1.0 - clamp(dot(Nrefl, V), 0.0, 1.0), FresnelPower)", reflected)
        floor_max = tuning("FresnelFloorMax")
        floor = 0.6 * floor_max
        center = floor + (1 - floor) * (1 - math.sin(math.radians(56))) ** tuning("FresnelPower")
        self.assertTrue(0.05 <= center <= 0.12, center)
        march = body(reflected, "if (weight > MinReflection)")
        self.assertIn("traceWaterReflection(R, sky)", march)
        self.assertEqual(main.count("traceWaterReflection("), 1)
        self.assertIn("mix(vec3(1.0), mapTint, MapTintShare)", march)
        sky = body(FS, "vec3 waterSky(")
        self.assertIn("SkyDayHorizon", sky)
        self.assertIn("SkyDayZenith", sky)
        self.assertIn("SkyDuskHorizon", sky)
        self.assertIn("SkyDuskZenith", sky)
        self.assertIn("smoothstep(SkyAltitude.x, SkyAltitude.y, altitude)", sky)

    def test_march_refines_and_validates_the_exact_colour_texel(self):
        trace = body(FS, "vec3 traceWaterReflection(")
        side = body(FS, "int reflectionSide(")
        self.assertIn("if (ray.z <= 0.0) return sky", trace)
        self.assertIn("vec4(ray, 0) * water.viewProj", trace)
        self.assertIn("i <= ReflectionSteps; ++i", trace)
        self.assertIn("hi = RayReach * f * f", trace)
        self.assertTrue(12 <= tuning("ReflectionSteps") <= 20)
        self.assertTrue(3 <= tuning("ReflectionBisections") <= 5)
        self.assertTrue(0 < tuning("RayFadeStart") < tuning("RayReach") <= 768)
        self.assertIn("if (!(clip.w > 1e-6)) return -1", side)
        self.assertLess(side.index("greaterThanEqual(tap"), side.index("texelFetch(SNAP(waterSceneDepth), tap, 0)"))
        self.assertIn("(ndc.z - sceneZ) * water.depthRange.w >= 0.0", side)
        self.assertIn("if (side == 0) { lo = hi; continue; }", trace)
        refine = body(trace, "j < ReflectionBisections;")
        self.assertIn("hi = mid;", refine)
        self.assertIn("hitTap = tap;", refine)
        self.assertIn("hitRaw = raw;", refine)
        self.assertIn("else lo = mid;", refine)
        self.assertIn("!snapshotWorld(hitTap, hitRaw, hit) || hit.z <= waterPosition.z", trace)
        self.assertIn("dot(hit - origin, ray) <= 0.0", trace)
        for stop in ("HitGap.x, HitGap.y, gap", "RayFadeStart, RayReach, hi", "0.0, ScreenEdge, min(edge.x, edge.y)"):
            self.assertIn(stop, trace)
        self.assertRegex(trace, r"if \(confidence > 0.0\)\s*return mix\(sky, texelFetch\(SNAP\(waterSceneColor\), hitTap, 0\).rgb, confidence\)")
        self.assertTrue(trace.rstrip().endswith("return sky;"))

    def test_caustic_dapples_brighten_the_bottom_before_absorption(self):
        # Soft dapples, no lines: a net of thin bright lines read as a spider
        # web from the game's camera (the user, 11-20).
        layers = body(FS, "vec3 shallowCaustics(")
        self.assertIn("smoothstep(CausticDepth.x, CausticDepth.y, vertical)", layers)
        self.assertIn("1.0 - smoothstep(CausticDepth.z, CausticDepth.w, vertical)", layers)
        self.assertIn("water.lightControls.z * depth", layers)
        self.assertIn("bottomWorld.xy + slope * (vertical * CausticWobble)", layers)
        self.assertIn("directLight * (amount * CausticGain * tint)", layers)
        pool = body(FS, "float causticPool(")
        warp = body(FS, "vec2 causticDapples(")
        dapples = warp + pool
        self.assertIn("smoothstep(CausticBand.x, CausticBand.y, pooled)", pool)
        self.assertEqual(warp.count("waterNoise("), 2)  # two warp axes
        self.assertEqual(pool.count("waterNoise("), 2)  # two octaves
        # The warm fringe's second sample shares the warp: one call per pixel.
        self.assertIn("return vec2(causticPool(q, t), causticPool(q + shift * (1.0 / CausticScale), t));", warp)
        self.assertEqual(layers.count("causticDapples("), 1)
        # Every drift is a multiple of half a noise cell per clock second, so the
        # 256-second clock wraps by whole 128-cell noise periods.
        drifts = re.findall(r"t \* vec2\(([^)]*)\)", dapples)
        self.assertEqual(len(drifts), 4)
        for drift in drifts:
            for v in drift.split(","):
                self.assertEqual(float(v) * 2, round(float(v) * 2), drift)
        self.assertNotIn("causticEdge", FS)
        self.assertTrue(32 <= tuning("CausticScale") <= 96)
        start, full, fade, gone = tuning_vector("CausticDepth")
        self.assertTrue(0 < start < full <= 10 and 40 <= fade < gone <= 128)
        self.assertTrue(0 < tuning("CausticGain") <= 1)
        water = body(FS, "vec4 waterBody(")
        self.assertIn("shallowCaustics(bottomWorld, vertical, wf.macroSlope, directLight)", water)
        # A factor on the bottom's own colour: dark stones stay dark, no milky haze.
        self.assertIn("refracted *= 1.0 + bottomLight", water)
        self.assertLess(water.index("refracted *= 1.0 + bottomLight"), water.index("refracted * transmission + deep * (1.0 - transmission) + sss"))
        self.assertIn("(1.0 - base.a) * openSurface * (original * bottomLight * transmission)", water)

    def test_light_blends_over_the_live_destination_without_game_alpha(self):
        draw = body(body(BACKEND, "void D3D9Backend::recordDraw("), "if (waterDraw)")
        self.assertIn("prepared.blend.colorSrcFactor() == VK_BLEND_FACTOR_SRC_ALPHA", draw)
        self.assertIn("prepared.blend.colorDstFactor() == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA", draw)
        blend = body(draw, "if (premultiply)")
        self.assertIn("auto blend = prepared.blend;", blend)
        self.assertIn("blend.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD)", blend)
        self.assertLess(draw.index("try { drawGeometry(); }"), draw.index("if (premultiply) owner.context->setBlendMode(0, prepared.blend);"))
        self.assertLess(draw.index("if (premultiply) owner.context->setBlendMode(0, prepared.blend);"), draw.index("finishDraw(reason"))
        self.assertIn("vec4(color.rgb * color.a, color.a)", body(FS, "vec4 premultiplied("))
        main = body(FS, "void main()")
        self.assertIn("base = mix(base, vec4(reflected, 1.0), weight);", main)
        self.assertIn("glint = openSurface * fogGlint * surfaceGlints(", main)
        # Glints are computed before the reflection march (register pressure:
        # 72 -> 64 on NVIDIA) and added after it, so the sum is unchanged.
        self.assertLess(main.index("glint = openSurface * fogGlint * surfaceGlints("),
                        main.index("traceWaterReflection(R, sky)"))
        self.assertLess(main.index("base = mix(base, vec4(reflected, 1.0), weight);"), main.index("base.rgb += glint;"))
        self.assertIn("if (any(isnan(base)) || any(isinf(base)))", main)
        self.assertTrue(main.rstrip().endswith("out_Color0 = base;"))

    def test_session_timing_counts_complete_gpu_samples_without_waiting(self):
        self.assertIn("m_waterScenes++ % 32", WATER)
        self.assertIn("m_stampCount == MaxTimedDraws", WATER)
        collect = body(WATER, "void WaterInPlace::collectTiming(")
        self.assertLess(collect.index("completed < m_submission"), collect.index("->vkGetQueryPoolResults("))
        self.assertNotIn("VK_QUERY_RESULT_WAIT_BIT", collect)
        self.assertIn("list->track(m_timestamps)", WATER)
        self.assertIn("m_stampCount && !m_partial", collect)
        self.assertIn("(stamps[i + 1] - stamps[i]) & m_tickMask", collect)
        report = body(MAIN, "void reportWater(")
        self.assertIn('w.gpuSamples ? "sampled" : "unavailable"', report)
        for reason in ("Msaa", "Format", "Snapshot", "DrawState", "Projection", "Resources", "Pipeline"):
            self.assertIn(f"n(R::{reason})", report)


if __name__ == "__main__":
    unittest.main()
