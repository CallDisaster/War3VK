#!/usr/bin/env python3
"""Source rules of the shadow pass at the x64 seam (x64 migration phase 4c).

The shadow pass is not rewritten as an executor: War3ShadowReceiverPass, the
same sources, is compiled into the x64 render host. What the host cannot
compile (the device, the game, diagnostics) it gets from one services file;
what the pass needs from the x86 data layer arrives as records
(war3_shadow_scene.h). Both ends build the pass's input from those records
with the same functions (war3_shadow_scene_input.h). These rules keep it that
way. The pixel and account comparison of the two modes is
run_x64_shadow_gate.py.

Phase 4c-3, the lifecycle: the four facts of the x86 data layer the pass asks
for reach it from the ShadowFrame record in both modes and stay between
records; a session ends at a Present and a device at its Reset, in process by
the device's own transitions; every run is accounted for, and so is every
image and buffer the pass owns.

Phase 4c-2, more casters: what a draw read from a dynamic buffer is copied at
the draw, by the shared function, into a pool whose chunks live no longer than
the frame's casters. An alpha-tested caster is a ShadowCasterAlpha record on
the caster's draw; both ends take the texture its stage bound and the
coordinates its TEXCOORD element names, and bind the pass's own sampler. The
frame's point lights are a PointLights record that both ends put into the
light manager the pass reads, before it runs.

Phase 4d, the unit outline: an Outline record carries its settings and the
handles of the objects to outline; both ends put the handles where the pass
asks for them and key them as the x86 data layer does.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EFFECTS = ROOT / "src/d3d9/war3/effects"
D3D9 = ROOT / "src/d3d9"
HOST = ROOT / "tools/render_host"
CORE = HOST / "dxvk_core"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def code(text: str) -> str:
    """The text without comments (rules are about code, not prose)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def includes(text: str) -> list[str]:
    return re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M)


def body(text: str, start: str) -> str:
    """A function from its signature to the closing brace in column 0."""
    text = text[text.index(start):]
    return text[:text.index("\n}\n")]


class ShadowSeam(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scene = read(EFFECTS / "war3_shadow_scene.h")
        cls.input = read(EFFECTS / "war3_shadow_scene_input.h")
        cls.wire = read(HOST / "d3d9_annotation_wire.h")
        cls.host_main = read(CORE / "d3d9_host_main.cpp")
        cls.backend = read(CORE / "d3d9_backend.cpp")
        cls.before_ui = read(CORE / "shadow_before_ui.cpp")
        cls.services = read(CORE / "shadow_host_services.cpp")
        cls.device = read(D3D9 / "d3d9_device.cpp")
        cls.fixture = read(D3D9 / "d3d9_war3_scene_effects_fixture.cpp")

    # --- records ---------------------------------------------------------------

    def test_host_exact_bounds_belong_to_the_frozen_draw(self):
        geometry = body(self.input, "inline ShadowCasterRefusal ShadowCasterReplayGeometry(")
        self.assertIn("if (pack)", geometry)
        self.assertIn("g.vertexData + vertexStart + g.positionOffset", geometry)
        self.assertIn("g.indexData + indexStart", geometry)
        self.assertIn("pool.bounds.get(vertexKey, indexKey", geometry)
        self.assertIn("bounds = {};", body(self.input, "void endFrame()"))
        append = body(self.input, "inline ShadowCasterRefusal AppendShadowCaster(")
        self.assertIn("draw.proxyCaster = g.compact", append)
        self.assertLess(geometry.index("CasterWorldBounds(localBounds, record.world, world.box)"),
                        geometry.index("pool.reserve("))
        self.assertIn("draw.proxyWorldBounds = world.box", append)
        self.assertIn("draw.proxyBoundsFrame = frameSerial", append)
        self.assertIn("geometry.compact = true", self.before_ui)

    def test_exact_cull_only_filters_the_surface_cascade_mask(self):
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        render = body(shadow, "bool War3ShadowReceiverPass::renderShadowMap(")
        self.assertLess(render.index("validateShadowReplayDraws("), render.index("CasterOutsideCascade("))
        self.assertIn("draw.proxyCaster && !m_volumeSunRenderPathActive", render)
        self.assertIn("draw.proxyBoundsFrame == input.frameSerial", render)
        self.assertIn("m_csmData.cascades[c].lightViewProj", render)
        self.assertIn("reconciliation.proxyCsmRemoved = proxyRemoved", render)
        self.assertIn("exactCull && draw.proxyCaster && !actualMask", render)
        self.assertIn("drawnUnique -= proxyOutsideEveryCascade", render)
        self.assertIn("depthClampEnable = VK_TRUE", body(shadow, "War3ShadowReceiverPass::createShadowCasterPipeline("))
        self.assertIn("j < 2", body(self.scene, "inline bool CasterOutsideCascade("))
        self.assertIn("setProxyExactCull(enabled)", self.before_ui)
        self.assertIn('GetEnvironmentVariableA("WARVK_D3D9_SHADOW_EXACT_CULL"', self.before_ui)
        self.assertIn("nullptr,nullptr,&startup.StartupInfo", read(HOST / "owned_child.cpp"))

    def test_partial_publication_checks_the_seal_and_both_render_loops(self):
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        run = body(shadow, "void War3ShadowReceiverPass::Run(")
        for proof in ("seal.sealed && !seal.counterOverflow", "seal.sealFrameSerial == input.frameSerial",
                      "seal.mapEpoch == input.mapEpoch", "seal.deviceEpoch == input.deviceEpoch",
                      "seal.arenaAdmissionCount == boxes.size()", "replayBindingsResolvedForFrame",
                      "seal.requiredCasterOmissionCount, accounted"):
            self.assertIn(proof, run)
        render = body(shadow, "bool War3ShadowReceiverPass::renderShadowMap(")
        self.assertIn("!m_volumeSunRenderPathActive && m_proxySubsetProven", render)
        self.assertEqual(render.count("!(m_proxyWriteMask & (1u << c))"), 3)
        self.assertLess(render.index("validateShadowReplayDraws("), render.index("VK_ATTACHMENT_LOAD_OP_CLEAR"))
        for proof in ("m_proxyLayers.settle(", "oldLayersValid, proxyLayerIdentity()",
                      "m_shadowMapResourceGeneration, shadowLifecycleTombstoneSerial, shadowStagePolicyRevision",
                      "m_csmData.cascades[c] = csmDataBeforeShadowCandidate.cascades[c]",
                      "!hostOmission && mutableSettings.postFx.enabled", "if (outlineEnabled)",
                      "directionalMapResolvedForFrame && !hostOmission"):
            self.assertIn(proof, run)
        self.assertIn("m_shadowTaaHistoryContractValid = false", run[run.index("previous != m_proxyAvailable"):])
        self.assertIn("m_proxyLayers = {};", shadow[shadow.index("::InvalidateMapEpoch("):])
        self.assertIn("const bool outlineEnabled = settings->occludedOutline.enabled;", run)

    def test_admission_counts_only_misses_before_allocation(self):
        reserve = self.input.split("bool reserve(scene::CasterCopySizes& writes", 1)[1].split("\n  }\n", 1)[0]
        for hit in ("if (vertexHit) writes.vertex = 0;", "if (indexHit) writes.index = 0;"):
            self.assertLess(reserve.index(hit), reserve.index("admission->allows("))
        self.assertLess(reserve.index("admission->allows("), reserve.index("reserveSlice("))
        self.assertLess(reserve.rindex("reserveSlice("), reserve.index("admission->accepted("))
        add = body(self.before_ui, "void ShadowBeforeUi::addCaster(")
        self.assertEqual(add.count("fx::AppendShadowCaster("), 1)
        self.assertIn("if (s.pool.budgetMiss) s.omit(world, cls, part);", add)
        self.assertIn("s.hintSerial + 1 != frameSerial || s.hintMap != mapEpoch || s.hintDevice != deviceEpoch", add)
        self.assertIn("s.scene.proxyRecordFrame = true;", self.before_ui)

    def test_draw_cost_is_independent_of_the_caster_count(self):
        admission = self.scene.split("class CasterAdmission {", 1)[1].split("// Frame, producer", 1)[0]
        allows = admission.split("bool allows(", 1)[1].split("\n  }\n", 1)[0]
        self.assertNotRegex(code(allows), r"\b(for|while|new)\b")
        self.assertLess(allows.index("if (!bytes)"), allows.index("requests[count++]"))
        self.assertLess(allows.index("if (!reserving)"), allows.index("footprints.priority("))
        self.assertLess(allows.index("tiers[tier] += bytes;"), allows.index("if (!fits)"))
        self.assertIn("t < reserved.size()", admission.split("void accepted(", 1)[1].split("\n  }\n", 1)[0])
        self.assertIn("std::array<Request, CasterFrameRecordLimit> requests", admission)
        finish = admission.split("void finish(", 1)[1].split("\n  }\n", 1)[0]
        self.assertLess(finish.index("if (overflow && final.count)"), finish.index("i < count"))
        self.assertIn("count = 0;", finish)
        for vector in ("shadowCasters", "shadowInstances", "proxyOmittedBounds"):
            self.assertIn(f"scene.{vector}.reserve(fx::scene::CasterFrameRecordLimit);", self.before_ui)
            self.assertIn(f"empty.{vector}.swap(scene.{vector});", self.before_ui)
        for old in ("makeRoom", "CasterVictim", "std::memmove", "pendingVertex", "trackChunks"):
            self.assertNotIn(old, self.before_ui + admission + self.input)

    def test_bounds_evidence_uses_the_recorded_picture_slot(self):
        evidence = read(CORE / "evidence_recorder.cpp")
        self.assertIn("outcome.received - outcome.refused - boundsCounts.exact", self.before_ui)
        self.assertIn("boundsCounts.removed = r.proxyCsmRemoved", self.before_ui)
        self.assertIn("owner.evidenceBounds = boundsCounts", self.backend)
        self.assertIn("PublishPresentedBounds(evidenceBounds);\n    evidenceBounds = {};", self.backend)
        self.assertLess(evidence.index("gameCasterAdmittedBytes"), evidence.index(",castersExactBounds,castersUnknownBounds,"))
        self.assertIn("slot.facts = facts;\n        m_package->ringBounds[m_head] = bounds;", evidence)
        self.assertIn("{slot.facts, m_package->ringBounds[index]}", evidence)
        writer = body(evidence, "void EvidenceRecorder::exportReady(")
        self.assertIn("f.bounds.exact, f.bounds.unknown", writer)
        self.assertNotIn("PresentedBounds()", writer)

    def test_record_header_depends_only_on_the_effect_records(self):
        names = includes(self.scene)
        self.assertIn("war3_effect_scene.h", names)
        for name in names:
            if name not in ("war3_effect_scene.h", "xmmintrin.h"):
                self.assertRegex(name, r"^c?[a-z_]+$", name)
        # Values only: no game, device or renderer type, no pointer.
        for forbidden in ("War3", "D3D9", "Rc<", "Dxvk", "void*", "void *"):
            self.assertNotIn(forbidden, code(self.scene).replace("WAR3_S", ""), forbidden)

    def test_record_numbers_and_sizes(self):
        for constant in ("LightingSchema = 9;", "ShadowFrameSchema = 10;",
                         "ShadowCasterSchema = 11;", "ShadowFrameBytes = 14 * 4;",
                         "ShadowCasterBytes = (16 + 4) * 4 + 4 + 3 * 4 + 4 + 4 * 4 + 2 * 8 + 4;",
                         "LightingBytes = LightingWords * 4;"):
            self.assertIn(constant, self.scene)
        self.assertEqual(136, (16 + 4) * 4 + 4 + 3 * 4 + 4 + 4 * 4 + 2 * 8 + 4)
        self.assertIn("Lighting=9, ShadowFrame=10, ShadowCaster=11", self.wire)
        for pair in ("AnnotationSchema::Lighting)==scene::LightingSchema",
                     "AnnotationSchema::ShadowFrame)==scene::ShadowFrameSchema",
                     "AnnotationSchema::ShadowCaster)==scene::ShadowCasterSchema"):
            self.assertIn(pair, self.wire)
        self.assertIn("scene::ShadowCasterBytes<=MaxAnnotationData", self.wire)

    def test_lighting_table_is_the_whole_record(self):
        table = self.scene[self.scene.index("#define WAR3_SCENE_LIGHTING_FIELDS("):]
        table = table[:table.index("\n\n")]
        rows = re.findall(r"^\s*([BUQFEV])\((\w+),\s*([\w.]+),", table, re.M)
        words = sum({"Q": 2, "V": 4}.get(kind, 1) for kind, _, _ in rows)
        self.assertEqual(123, words)
        self.assertIn("static_assert(scene::LightingBytes == 492", self.input)
        names = [name for _, name, _ in rows]
        paths = [path for _, _, path in rows]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(paths), len(set(paths)))
        # The sun, its shadows, the day and the ambient light; nothing else.
        self.assertEqual({"sun", "shadows", "dayNight", "ambient"},
                         {path.split(".")[0] for path in paths})
        # A setting added to one of them stops the build until the table has it.
        self.assertIn("sizeof(War3DirectionalLightSettings) == 40 && sizeof(War3ShadowSettings) == 280",
                      self.input)
        self.assertIn("sizeof(War3DayNightSettings) == 104 && sizeof(War3RenderSettings::ambient) == 20",
                      self.input)
        # Encode, decode, the mirror struct and both conversions come from it.
        self.assertGreaterEqual(self.scene.count("WAR3_SCENE_LIGHTING_FIELDS("), 5)
        self.assertEqual(2, self.input.count("WAR3_SCENE_LIGHTING_FIELDS("))

    def test_epochs_and_frame_numbers_are_not_on_the_wire(self):
        records = code(self.scene)
        for name in ("mapEpoch", "deviceEpoch", "frameSerial", "frameIndex"):
            self.assertNotIn(name, records, name)

    def test_what_the_record_does_not_carry_is_off(self):
        settings = body(self.input, "inline std::shared_ptr<const War3RenderSettings> ShadowPassSettings(")
        self.assertIn("ApplyLighting(lighting, *settings);", settings)
        self.assertIn("settings->postFx.volumetricLight.enabled = false;", settings)
        self.assertIn("settings->occludedOutline.enabled = false;", settings)

    # --- one pass, one way to feed it ---------------------------------------------

    def test_host_compiles_the_device_pass_sources(self):
        x86 = read(D3D9 / "meson.build")
        host = read(CORE / "meson.build")
        shadow = host[host.index("# Phase 4c: the shadow pass itself"):]
        shadow = shadow[:shadow.index(")")]
        sources = re.findall(r"'\.\./\.\./\.\./src/d3d9/([^']+)'", shadow)
        self.assertIn("d3d9_war3_shadow.cpp", sources)
        self.assertIn("d3d9_war3_shadow_resources.cpp", sources)
        for source in sources:
            self.assertIn(f"'{source}'", x86, source)
        self.assertIn("'shadow_host_services.cpp', 'shadow_before_ui.cpp'", shadow)
        # The device and the game stay out of the host.
        self.assertNotIn("d3d9_device.cpp", host)
        core = read(ROOT / "AutoTest/run_render_host_core_gate.py")
        listed = core[core.index("SHADOW_PASS_SOURCES = ("):]
        listed = listed[:listed.index(")")]
        self.assertEqual(sorted(f"src/d3d9/{source}" for source in sources),
                         sorted(re.findall(r"'([^']+)'", listed)))

    def test_host_services_are_values_from_records(self):
        services = code(self.services)
        for forbidden in ("d3d9_device.h", "D3D9DeviceEx", "GetModuleHandle", "Game.dll",
                          "ReadProcessMemory"):
            self.assertNotIn(forbidden, services, forbidden)
        header = read(CORE / "shadow_host_services.h")
        for fact in ("float gameTime", "tombstoneSerial", "stagePolicyRevision",
                     "runtimeDisabledModules", "frameIndex"):
            self.assertIn(fact, header)
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        for fact in ("shadow.frame.timeOfDay", "shadow.frame.tombstoneSerial",
                     "shadow.frame.stagePolicyRevision", "shadow.frame.runtimeDisabledModules"):
            self.assertIn(fact, run)
        self.assertLess(run.index("shadowhost::Publish(facts);"), run.index("m_state->pass->Run("))

    def test_both_ends_build_the_input_with_the_shared_functions(self):
        host = code(self.before_ui[self.before_ui.index("ShadowBeforeUi::ShadowBeforeUi("):])
        entry = code(self.device)
        entry = entry[entry.index("HRESULT D3D9DeviceEx::War3DrawShadowCasterForFixture("):
                      entry.index("void D3D9DeviceEx::War3RecordWorldCamera()")]
        for function in ("AppendShadowCaster(", "SealShadowScene(", "ShadowPipelineInput(",
                         "ShadowRunFactsOf(", "FindShadowPosition("):
            self.assertEqual(1, host.count("fx::" + function), function)
            self.assertEqual(1, entry.count("fx::" + function), function)
        # Neither end fills a caster by hand.
        for name in ("War3ShadowCasterDraw", "War3ShadowInstanceRef", "producerCompleteness"):
            self.assertNotIn(name, host, name)
            self.assertNotIn(name, entry, name)
            self.assertNotIn(name, code(self.fixture), name)
        self.assertIn("LightingDefaultsAgree()", host)
        self.assertIn("fx::LightingDefaultsAgree()", entry)
        self.assertIn("ShadowBeforeUi::layoutAgrees()", self.backend)

    def test_executor_refuses_what_it_cannot_replay(self):
        geometry = body(self.input, "inline ShadowCasterRefusal ShadowCasterReplayGeometry(")
        self.assertIn("return ShadowCasterRefusal::Layout;", geometry)
        self.assertGreaterEqual(geometry.count("return ShadowCasterRefusal::Range;"), 5)
        # Every range is proven before a byte is copied.
        copy = geometry.index("pool.reserve(")
        self.assertLess(geometry.rindex("return ShadowCasterRefusal::Range;", 0, copy), copy)
        self.assertEqual(2, geometry.count("return ShadowCasterRefusal::DynamicBuffer;"))
        append = body(self.input, "inline ShadowCasterRefusal AppendShadowCaster(")
        self.assertLess(append.index("account.note(refusal, copied);"), append.index("if (refusal != ShadowCasterRefusal::None)"))
        self.assertIn("instance.mode = War3ShadowReplayMode::FixedWorld;", append)
        # A refused or missing caster makes the set incomplete; without a
        # ShadowFrame record it is never sealed.
        seal = body(self.input, "inline void SealShadowScene(")
        self.assertLess(seal.index("if (!packet.hasFrame) return;"), seal.index("completeness.seal("))
        self.assertIn("uint64_t(account.refused) + missing", seal)
        self.assertIn("War3RequiredCasterOmissionReason::ArenaAdmission", seal)
        self.assertIn("packet.frame.omittedCasters", seal)

    def test_dynamic_buffers_are_copied_at_the_draw(self):
        geometry = body(self.input, "inline ShadowCasterRefusal ShadowCasterReplayGeometry(")
        # The copy is what the draw reads: vertices from the first it can
        # reference, indices from the first it uses, and the draw is rebased
        # onto the copy.
        self.assertIn("scene::CopyCasterVertices(dst, g.vertexData + vertexStart", geometry)
        self.assertIn("g.vertexOffset + uint64_t(firstVertex) * g.stride", geometry)
        self.assertIn("g.indexData + indexStart, size_t(writes.index)", geometry)
        self.assertIn("g.indexOffset + uint64_t(g.startIndex) * indexSize", geometry)
        self.assertIn("CasterIndexedAddress::from(g.baseVertexIndex, minimum, g.vertexDynamic)", geometry)
        self.assertIn("draw.vertexOffset = rebase ? address.rebased(minimum) : address.replayBase;", geometry)
        self.assertIn("draw.firstVertex = g.vertexDynamic ? 0u : g.startVertex;", geometry)
        # The pool: host-visible chunks of the frame, bounded, never reused.
        pool = self.input[self.input.index("class ShadowCasterPool {"):self.input.index("// ---- casters ----")]
        self.assertIn("CasterCopyFrameBytes = uint64_t(64) << 20;", self.scene)
        self.assertIn("next.append(writes, limit, plan)", pool)
        self.assertIn("uint64_t limit = scene::CasterCopyFrameBytes;", pool)
        self.assertIn("chunkBytes > limit - frameBytes", self.scene)
        self.assertIn("VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT", pool)
        end = pool[pool.index("  void endFrame() {"):]
        self.assertIn("m_chunk = nullptr;", end[:end.index("\n  }\n")])
        # Both ends hand the bytes the draw read: the host's dynamic buffer
        # (written in place by its uploads) and its index bytes, the device's
        # mapped slice.
        add = body(self.backend, "void D3D9Backend::addShadowCaster(")
        self.assertIn("source.vertexData = static_cast<const uint8_t*>(vb.object->mapPtr(0));", add)
        self.assertIn("source.indexData = ib.indexBytes.data();", add)
        entry = body(self.device, "HRESULT D3D9DeviceEx::War3DrawShadowCasterForFixture(")
        self.assertLess(entry.index("DrawIndexedPrimitive(type, baseVertexIndex,"), entry.index("buffer->GetMappedSlice()"))
        self.assertIn("geometry.vertexData = mapped(vertices);", entry)
        self.assertIn("geometry.indexData = mapped(indices);", entry)
        # The host takes the caster before it applies any later upload, and
        # drops the frame's chunks with the frame.
        self.assertIn("pool.endFrame();", body(self.before_ui, "void ShadowBeforeUi::State::clearFrame() {"))
        # Every draw message hands its casters over as soon as its draws are
        # recorded, within the same message.
        for draw in ("gpu->draw(s,annotated?casterSpans.data():nullptr);", "gpu->drawFixed(state,fixed,annotated?casterSpans.data():nullptr);", "gpu->drawBatch(states.data(),count,annotated?casterSpans.data():nullptr);",
                     "gpu->drawFixedBatch(fixedBatchStates.data(),fixedBatchFixed.data(),count,annotated?casterSpans.data():nullptr);"):
            at = self.host_main.index(draw)
            handed = self.host_main.index("if(annotated)deliverAnnotations(", at)
            self.assertNotIn("case d9::Op::", self.host_main[at:handed], draw)

    def test_proxy_demand_runs_after_classification_and_before_the_first_record(self):
        producer = read(D3D9 / "war3/platform/war3_proxy_casters.cpp")
        draw = body(code(producer), "HRESULT CastersDraw(")
        gate = draw.index("if (!c.pool.wants(")
        self.assertLess(draw.index("if (!std::isfinite(value))"), gate)
        self.assertLess(draw.index("CasterSkip::AlphaPayload"), gate)
        self.assertLess(draw.index("CasterSkip::Marker"), gate)
        self.assertNotIn("c.pool.", draw[:gate])
        self.assertNotIn("exhausted", draw[:gate])
        self.assertIn("return S_FALSE;", draw[gate:draw.index("record.category =", gate)])
        self.assertLess(gate, draw.index("AnnotateNextDraw("))
        self.assertIn("c.frameCounts.admittedBytes = uint32_t(std::min<uint64_t>(c.pool.demand.frameBytes, UINT32_MAX))", draw)
        self.assertIn("account.omitted = c.pool.omitted;", producer)
        self.assertIn("if (c.pool.omitted == 1u)", draw)
        self.assertNotIn("WARVK_D3D9_CASTER_BUDGET_TEST_MIB", producer)
        self.assertIn("WARVK_D3D9_CASTER_BUDGET_TEST_MIB", body(self.before_ui, "uint64_t CasterAdmissionLimit()"))
        self.assertIn("c.pool.wants(sizes, scene::CasterFrameRecordLimit)", draw)

    def test_host_trim_and_reuse_have_draw_time_proofs(self):
        prepare = body(self.backend, "D3D9Backend::PreparedDraw D3D9Backend::prepareDraw(")
        self.assertIn("span.include(index);", prepare)
        self.assertIn("ib->indexBytes.data()", prepare)
        self.assertIn("*outputSpan = span;", prepare)
        add = body(self.backend, "void D3D9Backend::addShadowCaster(")
        self.assertIn("if (source.indexDynamic)\n      source.frozenIndices = span;", add)
        self.assertIn("source.minVertexIndex = state.minVertexIndex;", add)
        self.assertIn("source.numVertices = state.numVertices;", add)
        self.assertIn("source.vertexGeneration = vb.contentGeneration;", add)
        self.assertIn("source.indexGeneration = ib.contentGeneration;", add)
        upload = body(self.backend, "void D3D9Backend::uploadBuffer(")
        # Every successful mode passes the same update, after the mapped write,
        # rename/carry, optional CPU copy and IB mirror. Slot reuse gets a new
        # value from this owner-wide, checked cumulative count on first upload.
        generation = upload.index("slot.contentGeneration = m_stats.uploadedBytes;")
        for operation in ("canAdd(m_stats.uploadedBytes, count)", "invalidateBuffer(",
                          "memcpy(slot.vertexBytes.data()", "memcpy(slot.indexBytes.data()",
                          "m_stats.uploadedBytes += count;"):
            self.assertLess(upload.index(operation), generation)
        self.assertIn("m_vertex = {};", self.input)
        self.assertIn("m_index = {};", self.input)
        self.assertIn("copied = writes.vertex + writes.index;", self.input)
        self.assertIn("draw.numVertices = g.numVertices;", self.input)
        self.assertIn("draw.shadowActualIndexMin = rebase ? 0u : g.frozenIndices.min;", self.input)
        self.assertIn("draw.shadowActualIndexMax = rebase ? frozenCount - 1u : g.frozenIndices.max;", self.input)

    def test_diagnostic_columns_follow_all_existing_wait_columns(self):
        evidence = read(CORE / "evidence_recorder.cpp")
        header = evidence.index(",gameCasterDemandCasters,gameCasterDemandBytes,gameCasterAdmittedBytes")
        self.assertLess(evidence.index('m_perfPending += ",waitUs"'), header)
        row = evidence.index("perfRow(facts, waitNames, waitCount);")
        self.assertLess(row, evidence.index("game.demandCasters", row))
        self.assertIn("g.demandCasters, static_cast<unsigned long long>(g.demandBytes), g.admittedBytes", evidence)
        frame = read(D3D9 / "war3/platform/war3_proxy_frame.cpp")
        for field in ("demandCasters", "demandBytes", "admittedBytes"):
            self.assertIn(f"out.{field} = counts.{field};", frame)

    # --- order and lifetime -------------------------------------------------------

    def test_shadows_run_before_the_effects_everywhere(self):
        device = self.device
        # In process: registration order of the passes.
        self.assertLess(device.index('"ShadowReceiver", std::move(shadowPass)'),
                        device.index('"SSAO", std::move(ssaoPass)'))
        entry = body(device, "HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture(")
        self.assertLess(entry.index("cShadow->Run(cmd, *cShadowInput);"),
                        entry.index("cSsao->Run(cmd, cInput);"))
        # Host.
        run = body(self.backend, "D3D9Backend::BeforeUiOutcome D3D9Backend::runBeforeUi(")
        self.assertLess(run.index("owner.shadow->run(list, owner.colorView, owner.depthView, packet, shadow,"),
                        run.index("owner.beforeUi->run(list, owner.colorView, owner.depthView, packet"))
        self.assertLess(run.index("owner.context->beginExternalRendering()"), run.index("owner.shadow->run("))

    def test_host_invalidates_by_epoch_before_every_run(self):
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        self.assertLess(run.index("m_state->pass->InvalidateMapEpoch(mapEpoch, deviceEpoch);"),
                        run.index("m_state->pass->Run(list, input);"))
        # The map epoch is the session generation, the device epoch the target's.
        self.assertIn("gpu->runBeforeUi(scenePacket,shadowPacket,effectFrames+1,sceneTracker.generation+1)",
                      self.host_main)
        self.assertIn("gpu->addShadowCaster(states[casterIndex],caster,hasAlpha?&alpha:nullptr,effectFrames+1,"
                      "sceneTracker.generation+1,casterSpans[casterIndex])", re.sub(r"\s+", "", self.host_main))
        backend = body(self.backend, "D3D9Backend::BeforeUiOutcome D3D9Backend::runBeforeUi(")
        self.assertIn("frameSerial, mapEpoch, owner.targetGeneration, m_stats.presentAttempts, "
                      "owner.evidence && owner.evidence->wantsAudit(), boundsCounts);", " ".join(backend.split()))

    def test_casters_never_outlive_their_frame(self):
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        # The input owns the scene through the passes. Only empty storage
        # returns to the next frame. A frame without a run drops its draws.
        self.assertIn("std::move(m_state->scene)", run)
        self.assertLess(run.index("m_state->pass->Run(list, input);"), run.index("s.admission.finish("))
        self.assertLess(run.index("s.scene = std::move(input.scene);"), run.index("s.clearFrame();"))
        self.assertIn("if (!shadow.work()) { endFrame(); return outcome; }", run)
        self.assertIn("gpu->endShadowFrame();", self.host_main)
        mark = body(self.fixture, "D3D9War3SceneEffectsFixture::MarkStage(")
        self.assertIn("*m_shadow = ShadowArm();", mark)
        self.assertIn("clearScene();", mark)
        # The pass dies with the device that created its objects.
        shutdown = self.backend.index("shadow.reset();")
        self.assertLess(shutdown, self.backend.index("context = nullptr", shutdown))

    def test_caster_records_travel_only_with_their_draw(self):
        # FrameScene carries Lighting and ShadowFrame; a caster is a draw record.
        valid = body(self.scene, "inline bool ValidShadowRecord(")
        self.assertIn("LightingSchema", valid)
        self.assertIn("ShadowFrameSchema", valid)
        self.assertNotIn("ShadowCasterSchema", valid)
        self.assertIn("if(schema==uint32_t(AnnotationSchema::ShadowCaster))return bytes==scene::ShadowCasterBytes;",
                      self.wire)
        self.assertEqual(2, self.wire.count("ValidDrawRecordData(schema,"))
        deliver = self.host_main[self.host_main.index("void deliverAnnotations("):]
        deliver = deliver[:deliver.index("void clearScene()")]
        self.assertIn("d9::scene::DecodeShadowCaster(data,bytes,caster)", deliver)
        # In process the same decode stands in front of the draw.
        draw = body(self.fixture, "D3D9War3SceneEffectsFixture::DrawIndexedAnnotated(")
        self.assertLess(draw.index("scene::DecodeShadowCaster("), draw.index("War3DrawShadowCasterForFixture("))

    def test_receiver_layout_is_bound_whole(self):
        # DXVK reads one descriptor per binding of the layout: every bind with
        # the receiver layout passes all of them (the shadow TAA passes once
        # passed 14 of 17 and read past their array).
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        self.assertIn("constexpr uint32_t kReceiverLayoutBindingCount = 19u;", shadow)
        layout = body(shadow, "const DxvkPipelineLayout *War3ShadowReceiverPass::createPipelineLayout() const {")
        self.assertIn("std::array<DxvkDescriptorSetLayoutBinding, kReceiverLayoutBindingCount> bindings", layout)
        self.assertEqual(19, layout.count("DxvkDescriptorSetLayoutBinding("))
        binds = [m.start() for m in re.finditer(r"ctx->bindResources\(DxvkCmdBuffer::ExecBuffer, m_layout,", shadow)]
        self.assertEqual(2, len(binds))
        for at in binds:
            declared = shadow.rindex("std::array<DxvkDescriptorWrite, ", 0, at)
            self.assertTrue(shadow.startswith("std::array<DxvkDescriptorWrite, kReceiverLayoutBindingCount> descriptors",
                                              declared), shadow[declared:declared+80])
        self.assertEqual(3, shadow.count("std::array<DxvkDescriptorWrite, kReceiverLayoutBindingCount> descriptors"))

    def test_outline_reaches_the_pass_from_its_record(self):
        # A FrameScene record, at the transport and in process alike.
        self.assertIn("PointLights=13, Outline=14,", self.wire)
        self.assertIn("||schema==uint32_t(AnnotationSchema::Outline)\n", self.wire)
        self.assertIn("if (schema == OutlineSchema) { Outline ignored; return DecodeOutline(p, bytes, ignored); }",
                      self.scene)
        set_scene = body(self.fixture, "D3D9War3SceneEffectsFixture::SetFrameScene(")
        self.assertIn("|| schema == scene::OutlineSchema", set_scene)
        # The settings: off without the record, every field of them with it.
        settings = body(self.input, "inline std::shared_ptr<const War3RenderSettings> ShadowPassSettings(")
        self.assertLess(settings.index("settings->occludedOutline.enabled = false;"),
                        settings.index("ApplyOutline(*outline, settings->occludedOutline);"))
        self.assertIn("ShadowPassSettings(shadow.lighting, shadow.hasOutline ? &shadow.outline : nullptr,", self.input)
        self.assertIn("static_assert(sizeof(War3OccludedOutlineSettings) == 28,", self.input)
        apply = body(self.input, "inline void ApplyOutline(")
        for field in ("enabled", "mode", "useScreenSpace", "widthPx", "showVisible", "showOccluded",
                      "colorR", "colorG", "colorB", "colorA"):
            self.assertIn(f"s.{field} = ", apply, field)
        # The handles, where the pass asks for them, and only when they changed.
        handles = body(self.input, "inline void ApplyOutlineHandles(")
        self.assertLess(handles.index("if (same)\n    return;"), handles.index("War3RenderState::ClearOutlineHandles();"))
        self.assertIn("War3RenderState::AddOutlineHandle(", handles)
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        self.assertLess(run.index("fx::ApplyOutlineHandles(shadow);"), run.index("m_state->pass->Run(list, input);"))
        mark = body(self.fixture, "D3D9War3SceneEffectsFixture::MarkStage(")
        self.assertLess(mark.index("war3::effects::ApplyOutlineHandles(m_shadowPacket);"),
                        mark.index("m_device->War3RunSceneEffectsForFixture("))
        # The host keys a handle as the x86 data layer does.
        data_layer = read(D3D9 / "war3/render/war3_render_state.cpp")
        x86_key = body(data_layer, "uint32_t NormalizeHandle(")
        host_key = self.services[self.services.index("uint32_t OutlineKey("):]
        host_key = host_key[:host_key.index("\n}\n")]
        for rule in ("handle & 0x0FFFFFu", "handleId | 0x100000u"):
            self.assertIn(rule, x86_key, rule)
            self.assertIn(rule, host_key, rule)
        # The pass matches each caster's batchHandle, nothing else.
        outline = read(D3D9 / "d3d9_war3_shadow_outline.cpp")
        self.assertEqual(2, outline.count("War3RenderState::IsOutlineHandle(caster.batchHandle)"))

    def test_outline_vulkan_use_is_what_the_validation_layer_accepts(self):
        # Phase 4d ran the outline under the validation layer for the first
        # time; each rule here is one of the errors it reported.
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        outline = read(D3D9 / "d3d9_war3_shadow_outline.cpp")
        mask = body(shadow, "War3ShadowReceiverPass::createOutlineMaskPipeline(")
        unit = body(outline, "VkPipeline War3ShadowReceiverPass::getOrCreateUnitOutlinePipeline(")
        # Every declared vertex input has a source (Input-07904).
        for pipeline in (mask, unit):
            self.assertLess(pipeline.index("War3AliasUnusedShadowCasterInputs(attributes, attributeCount);"),
                            pipeline.index("viState.vertexAttributeDescriptionCount = attributeCount;"))
        # The hand-built mask pipeline: counts set with the state, descriptor
        # buffers where DXVK binds them (03379/03380, 08117/08600).
        self.assertIn("vpInfo.viewportCount = 0;", mask)
        self.assertIn("vpInfo.scissorCount = 0;", mask)
        self.assertIn("if (m_device->canUseDescriptorBuffer())\n"
                      "    pipelineFlags.flags |= VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT;", mask)
        self.assertIn("renderingInfo.pNext = &pipelineFlags;", mask)
        # No draw names strides its pipeline fixed (08608).
        binds = re.findall(r"cmdBindVertexBuffers\([^;]*;", code(outline))
        self.assertEqual(4, len(binds))
        for bind in binds:
            self.assertTrue(bind.replace(" ", "").replace("\n", "").endswith(",nullptr);"), bind)
        # Attachments name the layout their image is in (09588/09592).
        self.assertEqual(3, outline.count("->image()->queryLayout("))
        # The unit outline's pipelines end with the pass (05137).
        destructor = body(shadow, "War3ShadowReceiverPass::~War3ShadowReceiverPass(")
        self.assertIn("for (auto &kv : m_outlinePipelines) {", destructor)
        # The depth copy's restore lets the outline load the depth it wrote
        # (SYNC-HAZARD-READ-AFTER-WRITE).
        copy_depth = body(read(D3D9 / "d3d9_war3_shadow_resources.cpp"), "void War3ShadowReceiverPass::copyDepth(")
        restore = copy_depth[copy_depth.index("ctx->cmdCopyImage("):]
        restore = restore[:restore.index("barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;")]
        self.assertIn("barriers[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |", restore)

    def test_volumetric_light_runs_after_the_shadow_pass_from_its_record(self):
        # A FrameScene record, at the transport and in process alike.
        self.assertIn("Outline=14,\n  VolumetricLight=15,", self.wire)
        self.assertIn("||schema==uint32_t(AnnotationSchema::VolumetricLight)||", self.wire)
        self.assertIn("if (schema == VolumetricLightSchema) { VolumetricLight ignored; "
                      "return DecodeVolumetricLight(p, bytes, ignored); }", self.scene)
        set_scene = body(self.fixture, "D3D9War3SceneEffectsFixture::SetFrameScene(")
        self.assertIn("|| schema == scene::VolumetricLightSchema", set_scene)
        # Every setting of the pass is a row of the table, and the table's
        # defaults are the settings' defaults at both ends.
        self.assertIn("static_assert(sizeof(War3VolumetricLightSettings) == 148,", self.input)
        self.assertIn("static_assert(scene::VolumetricLightBytes == 164,", self.input)
        table = self.scene[self.scene.index("#define WAR3_SCENE_VOLUMETRIC_FIELDS("):]
        table = table[:table.index("\n\n")]
        rows = re.findall(r"^\s*([BUIFE])\((\w+),\s*([\w.]+),", table, re.M)
        self.assertEqual(41, len(rows))
        self.assertEqual(len(rows), len({name for _, name, _ in rows}))
        self.assertIn("fx::VolumetricDefaultsAgree()", code(self.before_ui))
        self.assertIn("!fx::VolumetricDefaultsAgree()", code(self.device))
        # The settings: off without the record, every field of it with it.
        settings = body(self.input, "inline std::shared_ptr<const War3RenderSettings> ShadowPassSettings(")
        self.assertLess(settings.index("settings->postFx.volumetricLight.enabled = false;"),
                        settings.index("ApplyVolumetric(*volumetric, settings->postFx.volumetricLight);"))
        self.assertIn("shadow.hasOutline ? &shadow.outline : nullptr,\n"
                      "      shadow.hasVolumetric ? &shadow.volumetric : nullptr, classes, sunStyle, water);", self.input)
        # Both ends run it after the shadow pass, with the shadow pass's input
        # (the sun the shadow pass resolved, the maps it rendered).
        run = code(body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run("))
        self.assertLess(run.index("m_state->pass->Run(list, input);"), run.index("m_state->volumetric->Run(list, input);"))
        self.assertIn("std::make_unique<dxvk::War3VolumetricLightPass>(m_state->device, m_state->pass.get())", run)
        self.assertLess(run.index("fx::ShadowRunFactsOf(*m_state->pass);"),
                        run.index("fx::AddVolumetricRunFacts(*m_state->volumetric, run);"))
        entry = code(self.device)
        entry = entry[entry.index("HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture("):]
        entry = entry[:entry.index("\n}\n")]
        self.assertIn("const bool runVolumetric = runShadow && fx::VolumetricRuns(packet, *shadow);", entry)
        self.assertIn("const bool volumetric = fx::VolumetricRuns(packet, shadow);", run)
        self.assertLess(entry.index("cShadow->Run(cmd, *cShadowInput);"),
                        entry.index("cVolumetric->Run(cmd, *cShadowInput);"))
        self.assertLess(entry.index("cVolumetric->Run(cmd, *cShadowInput);"), entry.index("cSsao->Run(cmd, cInput);"))
        self.assertLess(entry.index("SynchronizeCsThread(DxvkCsThread::SynchronizeAll);"),
                        entry.index("fx::AddVolumetricRunFacts(*m_volumetricPass, *shadowFacts);"))
        # The pass takes the shadow pass from the device in x86 and as given in
        # the host; it reads nothing else of the device.
        volumetric = read(D3D9 / "d3d9_war3_volumetric_light.cpp")
        source = body(volumetric, "War3ShadowReceiverPass* War3VolumetricLightPass::shadowSource()")
        self.assertIn("return m_parent ? m_parent->GetWar3ShadowReceiverPass() : m_shadowSource;", source)
        self.assertNotIn("m_parent->GetWar3ShadowReceiverPass()", volumetric.replace(source, ""))
        header = " ".join(code(read(D3D9 / "d3d9_war3_volumetric_light.h")).split())
        self.assertIn("War3VolumetricLightPass(const Rc<DxvkDevice>& device, War3ShadowReceiverPass* shadow,", header)
        # Its composite names the layout the frame's target is in (09592).
        self.assertIn("colorImage->queryLayout(input.colorView->imageSubresources()) ==", volumetric)
        # The host compiles it as the x86 device does, and nothing else of it.
        host = read(CORE / "meson.build")
        block = host[host.index("# Phase 4d: the volumetric light pass"):]
        block = block[:block.index(")")]
        sources = re.findall(r"'\.\./\.\./\.\./src/d3d9/([^']+)'", block)
        self.assertEqual(["d3d9_war3_volumetric_light.cpp", "war3/render/war3_volumetric_shader_work_admission.cpp"],
                         sources)
        x86 = read(D3D9 / "meson.build")
        for source in sources:
            self.assertIn(f"'{source}'", x86, source)
        core = read(ROOT / "AutoTest/run_render_host_core_gate.py")
        listed = core[core.index("VOLUMETRIC_PASS_SOURCES = ("):]
        listed = listed[:listed.index(")")]
        self.assertEqual(sorted(f"src/d3d9/{source}" for source in sources),
                         sorted(re.findall(r"'([^']+)'", listed)))
        shaders = read(ROOT / "subprojects/war3fx/meson.build")
        shaders = shaders[shaders.index("war3fx_executor_shaders = files(["):]
        shaders = shaders[:shaders.index("])")]
        for shader in ("war3_volumetric_light.frag", "war3_volumetric_composite.frag",
                       "war3_volumetric_froxel_inject.comp", "war3_volumetric_froxel_temporal.comp",
                       "war3_volumetric_froxel_integrate.comp"):
            self.assertIn(shader, shaders, shader)

    def test_volumetric_media_reach_the_pass_as_the_data_layer_holds_them(self):
        # Fog volumes: a FrameScene record, at the transport and in process.
        self.assertIn("VolumetricLight=15, FogVolumes=16 };", self.wire)
        self.assertIn("||schema==uint32_t(AnnotationSchema::FogVolumes))", self.wire)
        self.assertIn("if (schema == FogVolumesSchema) { FogVolumes ignored; return DecodeFogVolumes(p, bytes, ignored); }",
                      self.scene)
        set_scene = body(self.fixture, "D3D9War3SceneEffectsFixture::SetFrameScene(")
        self.assertIn("|| schema == scene::FogVolumesSchema) {", set_scene)
        # The record's ranges are the manager's.
        manager = read(D3D9 / "war3/state/war3_fog_volume.cpp")
        for limit in ("kMaximumCoordinate = 1'000'000.0f", "kMinimumHalfExtent = 0.5f",
                      "kMaximumHalfExtent = 100'000.0f", "kMaximumDensity = 2.0f",
                      "kMaximumRotationMagnitude = 360'000.0f"):
            self.assertIn(limit, manager, limit)
        decode = body(self.scene, "inline bool DecodeFogVolumes(")
        for limit in ("real(-1000000.0f, 1000000.0f)", "real(0.5f, 100000.0f)", "real(-360000.0f, 360000.0f)",
                      "real(0.0f, 2.0f)", "real(0.0f, 1.0f)"):
            self.assertIn(limit, decode, limit)
        # Built through the manager's own interface, only when the record or
        # the manager changed, at both ends before the run.
        apply = body(self.input, "inline void ApplyFogVolumes(")
        self.assertLess(apply.index("if (same)\n    return;"), apply.index("manager.Clear();"))
        self.assertIn("built.generation == manager.GetGeneration()", apply)
        for call in ("manager.AddSphere(", "manager.AddBox(", "manager.AddCylinder(", "manager.SetRotationDegrees(",
                     "manager.SetActive(id, false);"):
            self.assertIn(call, apply, call)
        run = code(body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run("))
        self.assertLess(run.index("fx::ApplyFogVolumes(shadow);"), run.index("m_state->pass->Run(list, input);"))
        mark = body(self.fixture, "D3D9War3SceneEffectsFixture::MarkStage(")
        self.assertLess(mark.index("war3::effects::ApplyFogVolumes(m_shadowPacket);"),
                        mark.index("m_device->War3RunSceneEffectsForFixture("))
        # The host builds the manager the x86 data layer builds.
        self.assertIn("'../../../src/d3d9/war3/state/war3_fog_volume.cpp'", read(CORE / "meson.build"))
        # The weather medium: the Snow record, converted once, into the shadow
        # pass's input in its own frame and epochs; the device's weather pass
        # takes the same conversion.
        pipeline = body(self.input, "inline War3PipelineInput ShadowPipelineInput(")
        self.assertIn("input.weather = WeatherFromSnow(packet.snow, frameSerial, mapEpoch, deviceEpoch);", pipeline)
        entry = code(self.device)
        entry = entry[entry.index("HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture("):]
        entry = entry[:entry.index("\n}\n")]
        self.assertIn("input.weather = fx::WeatherFromSnow(packet.snow, frameSerial, kFixtureEpoch, kFixtureEpoch);",
                      entry)
        self.assertNotIn("weather.params.haze =", entry)
        runs = body(self.input, "inline bool VolumetricRuns(")
        self.assertIn("return shadow.hasVolumetric || WeatherMedium(packet);", runs)
        # The pass accounts its weather medium's storage and last run.
        header = " ".join(code(read(D3D9 / "d3d9_war3_volumetric_light.h")).split())
        self.assertIn("return resourceBytes() + (m_weatherOwner ? m_weatherOwner->resourceBytes() : 0u);", header)
        self.assertIn("return m_weatherOwner ? &m_weatherOwner->m_lastRun : nullptr;", header)
        facts = body(self.input, "inline void AddVolumetricRunFacts(")
        self.assertIn("weather->frameSerial == last.frameSerial", facts)

    def test_point_lights_reach_the_pass_through_the_light_manager(self):
        # A FrameScene record, at the transport and in process alike.
        self.assertIn("||schema==uint32_t(AnnotationSchema::PointLights)||", self.wire)
        self.assertIn("if (schema == PointLightsSchema) { PointLights ignored; return DecodePointLights(p, bytes, ignored); }",
                      self.scene)
        set_scene = body(self.fixture, "D3D9War3SceneEffectsFixture::SetFrameScene(")
        self.assertIn("|| schema == scene::PointLightsSchema", set_scene)
        # One function puts them where the pass reads them, and only when
        # they changed.
        apply = body(self.input, "inline void ApplyPointLights(")
        self.assertIn("auto& manager = War3LightManager::Instance();", apply)
        self.assertLess(apply.index("if (same)\n    return;"), apply.index("manager.ClearLights();"))
        self.assertIn("manager.AddPointLight(", apply)
        # Both ends, before the pass runs.
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        self.assertLess(run.index("fx::ApplyPointLights(shadow);"), run.index("m_state->pass->Run(list, input);"))
        mark = body(self.fixture, "D3D9War3SceneEffectsFixture::MarkStage(")
        self.assertLess(mark.index("war3::effects::ApplyPointLights(m_shadowPacket);"),
                        mark.index("m_device->War3RunSceneEffectsForFixture("))
        # The pass takes them from the manager, nowhere else.
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        self.assertIn("pointLightSnapshot = War3LightManager::Instance().GetFrameSnapshot(", shadow)

    def test_alpha_record_is_the_companion_of_its_caster(self):
        # On the wire it follows the caster record of the same draw, at the
        # proxy and at the decoder alike.
        self.assertIn("ShadowCaster=11, ShadowCasterAlpha=12,", self.wire)
        follows = body(self.wire, "inline bool CompanionFollowsCaster(")
        self.assertIn("previousSchema==uint32_t(AnnotationSchema::ShadowCaster)", follows)
        self.assertIn("if(!CompanionFollowsCaster(schema,count&&(previous>>16)==index?(previous&0xffff):0))return false;",
                      self.wire)
        self.assertIn("||!d9::CompanionFollowsCaster(schema,nextRecordCount?nextLastSchema:0))return D3DERR_INVALIDCALL;",
                      read(HOST / "d3d9_connection.h"))
        # The host hands a caster over with the companion of its own draw.
        deliver = self.host_main[self.host_main.index("void deliverAnnotations("):]
        deliver = deliver[:deliver.index("void clearScene()")]
        self.assertIn("if(hasCaster&&casterIndex!=index)handCaster();", deliver)
        self.assertIn("ShadowCasterAlpha)&&hasCaster&&casterIndex==index) {", deliver)
        self.assertGreater(deliver.rindex("handCaster();"), deliver.index("pendingAnnotations.forEach("))
        # In process the companion is taken by the next caster draw, refused or not.
        draw = body(self.fixture, "D3D9War3SceneEffectsFixture::DrawIndexedAnnotated(")
        self.assertLess(draw.index("m_hasNextAlpha = false;"), draw.index("scene::DecodeShadowCaster("))
        annotate = body(self.fixture, "D3D9War3SceneEffectsFixture::AnnotateNextDraw(")
        self.assertIn("scene::DecodeShadowCasterAlpha(", annotate)
        self.assertIn("if (m_hasNextAlpha)\n    return D3DERR_INVALIDCALL;", annotate)

    def test_alpha_casters_sample_the_draws_own_texture(self):
        # The texture of the record's stage, as each end binds it.
        add = body(self.backend, "void D3D9Backend::addShadowCaster(")
        self.assertIn("alpha->stage ? state.additionalTextures[alpha->stage - 1] : state.texture;", add)
        self.assertIn("source.alphaTexture = owner.at(owner.textures, texture).view;", add)
        entry = body(self.device, "HRESULT D3D9DeviceEx::War3DrawShadowCasterForFixture(")
        self.assertIn("GetCommonTexture(m_state.textures[alpha->stage])", entry)
        self.assertIn("geometry.alphaTexture = texture->GetSampleView(false);", entry)
        # Both find the coordinates with the shared function.
        self.assertIn("fx::FindShadowTexcoord(", entry)
        self.assertIn("fx::FindShadowTexcoord(", self.before_ui)
        # The executor refuses what the pass would not draw alpha-tested, before
        # anything is copied, and replays the coordinates from the position stream.
        geometry = body(self.input, "inline ShadowCasterRefusal ShadowCasterReplayGeometry(")
        refuse = geometry.index("if (g.alphaTest && (g.alphaTexture == nullptr || !g.uvFloat2")
        self.assertLess(refuse, geometry.index("pool.reserve("))
        self.assertIn("draw.positionStride = quantized ? 8u : pack ? scene::CasterVertexStride(g.alphaTest) : g.stride;", geometry)
        self.assertIn("draw.uvStride = draw.positionStride;", geometry)
        self.assertIn("draw.uvOffset = pack ? 12u : g.uvOffset;", geometry)
        self.assertIn("draw.uvBinding = 0;", geometry)
        # The pass's sampler, with the mip settings of the Lighting record, on
        # both ends after the pass exists and before it runs.
        bind = body(self.input, "inline void BindShadowAlphaSamplers(")
        self.assertIn("pass.getFallbackSampler(shadows.alphaShadowUseMip, shadows.alphaShadowMipLodBias)", bind)
        run = body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run(")
        self.assertLess(run.index("m_state->pass = std::make_unique"), run.index("fx::BindShadowAlphaSamplers(input, *m_state->pass);"))
        self.assertLess(run.index("fx::BindShadowAlphaSamplers("), run.index("m_state->pass->Run(list, input);"))
        effects = body(self.device, "HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture(")
        self.assertLess(effects.index("fx::BindShadowAlphaSamplers(*shadowInput, *m_shadowReceiverPass);"),
                        effects.index("cShadow->Run(cmd, *cShadowInput);"))
        # The run's account says how many casters were prepared alpha-tested.
        self.assertIn("facts.alphaPrepared = r.shadowMapAlphaTestPreparedCount;",
                      body(self.input, "inline ShadowRunFacts ShadowRunFactsOf("))

    # --- lifecycle (4c-3) -----------------------------------------------------------

    def test_pass_asks_the_data_layer_for_four_facts(self):
        gate = read(ROOT / "AutoTest/run_render_host_core_gate.py")
        listed = gate[gate.index("SHADOW_PASS_SOURCES = ("):]
        sources = re.findall(r"'([^']+)'", listed[:listed.index(")")])
        passes = "".join(code(read(ROOT / source)) for source in sources)
        for function in ("War3RenderState::GetGameTime()", "CurrentShadowCasterTombstoneSerial()",
                         "CurrentShadowStagePolicyRevision()"):
            self.assertIn(function, passes, function)
        # Of the runtime modules it asks about shadow TAA only: the one bit of
        # a record's module word that reaches it.
        asked = re.findall(r"IsWar3RuntimeModule(?:Enabled|Disabled)\(\s*([\w:]+)", passes)
        self.assertTrue(asked)
        self.assertEqual({"war3::runtime::War3RuntimeModule::ShadowTaa"}, set(asked))
        self.assertNotIn("IsAnyWar3RuntimeModuleEnabled", passes)
        self.assertNotIn("GetWar3RuntimeConfig", passes)
        self.assertIn("inline constexpr uint32_t ShadowPassModules =\n"
                      "    1u << uint32_t(war3::runtime::War3RuntimeModule::ShadowTaa);", self.input)
        # The run's account reads the facts where the pass does.
        facts = body(self.input, "inline ShadowRunFacts ShadowRunFactsOf(")
        for line in ("facts.gameTime = War3RenderState::GetGameTime();",
                     "facts.tombstoneSerial = war3::render::CurrentShadowCasterTombstoneSerial();",
                     "facts.stagePolicyRevision = war3::render::CurrentShadowStagePolicyRevision();",
                     "facts.taaModule = r.shadowTaaRuntimeModuleEnabled;",
                     "facts.resourceBytes = pass.ResourceBytes();"):
            self.assertIn(line, facts)

    def test_host_facts_start_as_the_data_layer_and_stay_between_records(self):
        header = read(CORE / "shadow_host_services.h")
        self.assertIn("float gameTime = -1.0f;", header)
        self.assertIn("uint64_t tombstoneSerial = 0, stagePolicyRevision = 1;", header)
        self.assertIn("std::atomic<float> g_gameTime = -1.0f;", read(D3D9 / "war3/render/war3_render_state.cpp"))
        lifecycle = read(D3D9 / "war3/render/war3_shadow_lifecycle.cpp")
        self.assertIn("std::atomic<uint64_t> g_eventSerial{0u};", lifecycle)
        self.assertIn("std::atomic<uint64_t> g_stagePolicyRevision{1u};", lifecycle)
        # Neither counter is ever set back.
        self.assertEqual([], re.findall(r"g_(?:eventSerial|stagePolicyRevision)\.store\(", lifecycle))
        run = code(body(self.before_ui, "ShadowOutcome ShadowBeforeUi::run("))
        start = run.index("shadowhost::DataLayerFacts facts = shadowhost::Current();")
        record = run.index("if (shadow.hasFrame) {", start)
        publish = run.index("shadowhost::Publish(facts);")
        self.assertLess(start, record)
        self.assertLess(record, publish)
        # Nothing of the four is written outside the record's branch.
        outside = run[:record] + run[run.index("}", record):]
        for fact in ("facts.gameTime", "facts.tombstoneSerial", "facts.stagePolicyRevision",
                     "facts.runtimeDisabledModules"):
            self.assertNotIn(fact, outside, fact)

    def test_in_process_facts_go_through_the_data_layer(self):
        fixture = self.fixture
        apply = body(fixture, "void ApplyShadowFacts(")
        self.assertIn("War3RenderState::SetGameTime(frame.timeOfDay);", apply)
        self.assertIn("war3::render::PublishShadowCasterTombstone(tombstone)", apply)
        self.assertIn("war3::render::AdvanceShadowStagePolicyRevision();", apply)
        reachable = body(fixture, "bool ShadowFactsReachable(")
        self.assertIn("war3::effects::ShadowPassModules", reachable)
        self.assertIn("GetWar3RuntimeConfig().effectiveDisabledMask", reachable)
        mark = body(fixture, "D3D9War3SceneEffectsFixture::MarkStage(")
        refuse = mark.index("if (facts && !ShadowFactsReachable(m_shadowPacket.frame))\n    return D3DERR_INVALIDCALL;")
        self.assertLess(refuse, mark.index("m_tracker.mark(stage, 0)"))
        self.assertLess(mark.index("ApplyShadowFacts(m_shadowPacket.frame);"),
                        mark.index("m_device->War3RunSceneEffectsForFixture("))
        # The run's account is read with the CS thread idle.
        entry = body(self.device, "HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture(")
        self.assertLess(entry.index("SynchronizeCsThread(DxvkCsThread::SynchronizeAll);"),
                        entry.index("fx::ShadowRunFactsOf(*m_shadowReceiverPass)"))

    def test_a_session_ends_at_a_present_in_both_modes(self):
        # x64: the proxy holds the request and sends it ahead of the Present;
        # the host then takes the new generation, which is its map epoch.
        connection = read(HOST / "d3d9_connection.h")
        self.assertIn("if(h.op==d9::Op::Present&&sessionResetPending.load(std::memory_order_acquire)) {", connection)
        reset = self.host_main[self.host_main.index("case d9::Op::SessionReset:{"):]
        reset = reset[:reset.index("case d9::Op::ContentUpload:")]
        self.assertIn("if(!sceneTracker.reset(generation))return D3DERR_INVALIDCALL;", reset)
        self.assertIn("clearScene();", reset)
        # In process: the request is the game's own map unload, and the device
        # applies it in its Present.
        request = body(self.fixture, "D3D9War3SceneEffectsFixture::RequestSessionReset(")
        self.assertLess(request.index("if (!m_tracker.reset(generation))"), request.index("clearScene();"))
        self.assertLess(request.index("clearScene();"), request.index("m_device->War3RequestShadowMapEpochReset();"))
        self.assertIn("device->War3RequestShadowMapEpochReset();", read(D3D9 / "war3/war3.cpp"))
        device = self.device
        present = device.index('War3PresentFrameTransitionScope("ShadowMapEpochTransition")')
        self.assertLess(present, device.index("War3ApplyShadowMapEpochResetAtPresent(retiringFrameSerial);", present))
        self.assertLess(present, device.index("War3ApplyShadowDeviceEpochTransitionAtPresent(", present))
        apply = body(device, "bool D3D9DeviceEx::War3ApplyShadowMapEpochResetAtPresent(")
        self.assertIn("War3InvalidateShadowReceiverEpochOnCs(\n      m_war3GpuSkinMapEpoch, deviceEpoch);", apply)
        # Both ends give the pass the epochs of the frame they run.
        entry = body(device, "HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture(")
        self.assertEqual(2, entry.count("m_war3GpuSkinMapEpoch, m_war3GpuSkinDeviceEpoch)"))

    def test_a_device_reset_is_a_new_device_epoch_in_both_modes(self):
        # x64: new targets, a new generation.
        targets = body(self.backend, "  void createTargets() {")
        self.assertIn("++targetGeneration;", targets)
        reset = body(self.backend, "void D3D9Backend::reset(")
        self.assertIn("owner.createTargets();", reset)
        self.assertNotIn("shadow.reset()", reset)
        # In process: Reset and ResetEx both start the device epoch
        # transition, which the device applies in its Present.
        self.assertEqual(2, len(re.findall(r"^  War3ResetGpuSkinDeviceEpoch\(\);$", self.device, re.M)))
        transition = body(self.device, "bool D3D9DeviceEx::War3ApplyShadowDeviceEpochTransitionAtPresent(")
        self.assertIn("War3InvalidateShadowReceiverEpochOnCs(\n      m_war3GpuSkinMapEpoch, requested);", transition)

    def test_resource_outlet_names_everything_the_pass_owns(self):
        header = code(read(D3D9 / "d3d9_war3_shadow.h"))
        owned = re.findall(r"(?:^|\s)(?:Rc<Dxvk(?:Image|Buffer)>|std::array<Rc<Dxvk(?:Image|Buffer)>,[^>]*>)\s+(m_\w+)",
                           header)
        self.assertGreaterEqual(len(owned), 17)
        outlet = body(read(D3D9 / "d3d9_war3_shadow_resources.cpp"), "uint64_t War3ShadowReceiverPass::ResourceBytes() const {")
        for member in owned:
            self.assertIn(member, outlet, member)
        # The upload slots hold buffers of their own.
        self.assertIn("Rc<DxvkBuffer> buffer;", header)
        for member in ("m_shadowMatrixUploadSlots", "m_pointRayHiZVisibilityView", "m_pointRayHiZView"):
            self.assertIn(member, outlet, member)
        self.assertIn("storage->getMemoryInfo().size", outlet)
        # The hybrid ray tracer keeps its images and uniforms from one frame to
        # the next, while the pass clears its two views at every run (phase
        # 4d-3 found them uncounted after the frame that made them): each is
        # counted where it is owned.
        tracer = code(read(D3D9 / "war3/render/war3_hybrid_ray_tracing.h"))
        kept = re.findall(r"Rc<Dxvk(Image|Buffer)>\s+m_(\w+);", tracer)
        self.assertEqual({("Image", "hizImage"), ("Image", "visibilityImage"), ("Buffer", "uniformBuffer")}, set(kept))
        for kind, member in kept:
            accessor = member[0].upper() + member[1:]
            self.assertIn(f"const Rc<Dxvk{kind}>& {accessor}() const {{ return m_{member}; }}", tracer)
            self.assertIn(f"m_hybridRayTracing->{accessor}()", outlet, accessor)
        self.assertIn("m_pointRayHiZVisibilityView = nullptr;\n  m_pointRayHiZView = nullptr;",
                      read(D3D9 / "d3d9_war3_shadow.cpp"))

    def test_every_run_is_accounted_for_in_both_modes(self):
        fixture = read(ROOT / "AutoTest/test_render_host_d3d9_shadow.cpp")
        for key in ("casters", "replayDraws", "drawnCasters", "cascades", "cascadeDrawn", "mapExecuted",
                    "receiverExecuted", "earlyReturn", "inputReject", "replayRejected", "completeMap", "taaModule",
                    "taaMode", "gameTimeBits", "tombstoneSerial", "stagePolicyRevision", "resourceBytes"):
            self.assertIn(f'\\"{key}\\":', self.host_main, key)
            self.assertIn(f'\\"{key}\\":', fixture, key)
        self.assertIn("if(shadowRunLog.size()<ShadowRunLogLimit) {\n"
                      "              shadowRunLog.push_back(shadow);\n"
                      "              shadowDeviceLog.push_back({outcome.deviceUsedBytes,outcome.deviceAllocatedBytes});",
                      self.host_main)
        self.assertIn('\\"runLog\\":[', self.host_main)
        self.assertIn('\\"runLog\\":[', fixture)
        # The fixture's mirror of the in-process account is the header's.
        header = read(D3D9 / "d3d9_war3_scene_effects_fixture.h")
        mirror = fixture[fixture.index("struct ShadowRun {"):fixture.index("struct ISceneFixture")]
        real = header[header.index("struct WarVKShadowFixtureRun {"):header.index("// Same contract as the x64 proxy")]

        def fields(text):
            return re.findall(r"\b(\w+)\s*(?:\[\d+\])?\s*=", code(text))
        self.assertEqual(fields(real), fields(mirror))
        gate = read(ROOT / "AutoTest/run_x64_shadow_gate.py")
        for message in ("'shadow pass runs: '", "'shadow pass storage: '", "'the shadow pass runs differ'",
                        "'session resets reached the host'", "'fixture lifecycle coverage'"):
            self.assertIn(message, gate)

    def test_host_accounts_for_its_device_after_every_run(self):
        # The pass names what it owns; the device's allocator says what the
        # host holds altogether, so what a map or a device that ended left
        # behind outside the pass shows too.
        run = body(self.backend, "D3D9Backend::BeforeUiOutcome D3D9Backend::runBeforeUi(")
        submit = run.index("if (!owner.streamMode) owner.submit(m_stats, true);")
        sample = run.index("const auto stats = owner.device->getMemoryStats(heap);")
        self.assertLess(run.index("shadowOutcome = owner.shadow->run("), submit)
        self.assertLess(submit, sample)
        self.assertIn("heap < memory.memoryHeapCount", run)
        self.assertIn("outcome.deviceUsedBytes += stats.memoryUsed;", run)
        self.assertIn("outcome.deviceAllocatedBytes += stats.memoryAllocated;", run)
        self.assertIn('\\"deviceMemoryLog\\":[', self.host_main)
        gate = read(ROOT / "AutoTest/run_x64_shadow_gate.py")
        self.assertIn("SETTLED = 'shadow-return'", gate)
        self.assertIn("set(used[first:]) == {used[first]} and max(allocated[first:]) <= allocated[first]", gate)
        self.assertIn("'host device memory: '", gate)

    def test_directional_class_policy_precedes_admission_and_preserves_terrain_bias(self):
        # The caster path (the water surface has its own admission, before it).
        front = body(read(D3D9 / "war3/platform/war3_proxy_casters.cpp"),
                     "HRESULT CastersDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {")
        self.assertLess(front.index("scene::ClassifyCaster("), front.index("c.pool.wants("))
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        self.assertIn("!m_volumeSunRenderPathActive && !m_casterTransforms.empty()", shadow)
        self.assertIn("m_proxyAvailable = m_proxyHeld = 0", shadow)
        self.assertIn("scene::ExtendCasterBox(boxes[p.boxIndex]", shadow)
        self.assertEqual(shadow.count("DirectionalTerrainCasterDepthBias(m_shadowMapResolution)"), 2)
        run = shadow[shadow.index("const auto& classes = settings->casterClasses;"):]
        self.assertLess(run.index("m_objectBases.add("), run.index("renderShadowMap(ctx, input"))

    def test_gpu_timestamps_are_nonblocking_and_bracket_receiver_draws(self):
        timers = self.before_ui.split("struct ShadowGpuTimers {", 1)[1].split("struct ShadowBeforeUi::State", 1)[0]
        self.assertIn("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT", timers)
        for word in ("VK_QUERY_RESULT_WAIT_BIT", "vkWait", "vkQueueSubmit", "flush("):
            self.assertNotIn(word, timers)
        self.assertIn("slot->retired()", timers)
        self.assertIn("list->track(", timers)
        shadow = read(D3D9 / "d3d9_war3_shadow.cpp")
        for begin, end in ((2, 3), (4, 5)):
            work = shadow.split(f"proxyTimestamp(ctx, {begin});", 1)[1].split(f"proxyTimestamp(ctx, {end});", 1)[0]
            self.assertEqual(re.findall(r"ctx->(cmd\w+)\(", work), ["cmdDraw"])
        work = shadow.split("proxyTimestamp(ctx, 0);", 1)[1].split("proxyTimestamp(ctx, 1);", 1)[0]
        self.assertIn("if (reuseLastShadowMap)", work)
        self.assertIn("renderShadowMap(ctx, input", work)
        self.assertNotIn("renderVolumeSunShadowMap(", work)

    def test_caster_copy_formats_and_evidence_columns(self):
        geometry = body(self.input, "inline ShadowCasterRefusal ShadowCasterReplayGeometry(")
        self.assertIn("pool.quantize && !g.alphaTest && world.known", geometry)
        self.assertIn("quantized ? VK_FORMAT_R16G16B16A16_UNORM", geometry)
        self.assertIn("quantized ? quant.world : record.world", geometry)
        # The table compares against its own copy, never the write-combined map.
        self.assertIn("std::memcmp(bytesCopy.data() + e.offset, bytes, size)", self.scene)
        recorder = read(CORE / "evidence_recorder.cpp")
        names = "shadowMapGpuUs,shadowReceiverGpuUs,casterVertexBytes,casterIndexBytes,casterIndexReusedBytes"
        self.assertIn(names + r"\r\n", recorder)
        for name in names.split(","):
            self.assertIn('\\"' + name + '\\"', recorder)

    # --- gate ---------------------------------------------------------------------

    def test_gate_compares_pictures_and_accounts(self):
        gate = read(ROOT / "AutoTest/run_x64_shadow_gate.py")
        for name in ("war3_shadow_scene.h", "war3_shadow_scene_input.h", "test_render_host_d3d9_shadow.cpp"):
            self.assertIn(name, gate)
        self.assertIn("'in-process and x64 host images differ: '", gate)
        self.assertIn("'the shadow pass accounts differ: '", gate)
        self.assertIn("IsolatedRenderDesktop(restricted_medium=True)", gate)
        # The host prints, and the in-process fixture returns, the same account.
        for key in ("runs", "mapRenders", "receiverDraws", "castersUsed", "castersRefused",
                    "refusedDynamic", "refusedLayout", "refusedRange", "castersCopied", "copiedBytes",
                    "castersDrawn", "replayRejected"):
            self.assertIn(f'\\"{key}\\":', self.host_main, key)
            self.assertIn(f'\\"{key}\\":', read(ROOT / "AutoTest/test_render_host_d3d9_shadow.cpp"), key)


if __name__ == "__main__":
    unittest.main()
