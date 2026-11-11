"""Bounded production wiring guards; static evidence is not runtime proof.

The disabled gate prevents extra device-state reads. Fixed-size owned POD
copies still enlarge the records, so these checks do not claim zero overhead.
"""

from functools import lru_cache
from pathlib import Path
import re
import unittest

import native_render_input_contract as contract


ROOT = Path(__file__).resolve().parents[1]
_CPP_TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


@lru_cache(maxsize=32)
def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


@lru_cache(maxsize=32)
def masked(source):
    # Preserve character positions while excluding braces/anchors in comments
    # and string literals from the function and branch boundary scanner.
    return _CPP_TOKENS.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), source)


@lru_cache(maxsize=64)
def code(source):
    return _CPP_TOKENS.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group())
        if m.group().startswith(("//", "/*")) else m.group(), source)


def block_after(source, anchor, start=0):
    clean = masked(source)
    location = clean.find(anchor, start)
    if location < 0:
        raise AssertionError(f"missing code boundary: {anchor}")
    opening = clean.find("{", location + len(anchor))
    if opening < 0:
        raise AssertionError(f"missing body after: {anchor}")
    depth = 1
    for index in range(opening + 1, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index], index + 1
    raise AssertionError(f"unclosed body after: {anchor}")


def bodies_after(source, anchor):
    result = []
    start = 0
    while masked(source).find(anchor, start) >= 0:
        body, end = block_after(source, anchor, start)
        result.append(body)
        start = end
    if not result:
        raise AssertionError(f"missing code boundary: {anchor}")
    return result


def function(source, name):
    # The semantic append has short forwarding overloads. Select its actual
    # implementation body, not a wrapper or an occurrence in a comment.
    return max(bodies_after(source, name + "("), key=len)


def tight(source):
    return re.sub(r"\s+", "", code(source))


class NativeRenderInputWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = read("src/d3d9/d3d9_device.cpp")
        cls.device_header = read("src/d3d9/d3d9_device.h")
        cls.scene = read("src/d3d9/d3d9_war3_scene.h")
        cls.facts = read("src/d3d9/war3/render/war3_native_draw_material_facts.h")
        cls.payload = read("src/d3d9/war3/shadow/war3_shadow_alpha_test_payload.h")
        cls.inputs = read("src/d3d9/war3/tools/war3_frame_inputs.cpp")
        cls.export = read("src/d3d9/war3/tools/war3_native_render_input_export.h")
        cls.core = read("src/d3d9/war3/tools/war3_frame_inputs_core.h")
        cls.capture_helper = function(cls.device, "D3D9DeviceEx::War3CaptureNativeDrawMaterialFacts")
        cls.capture = function(cls.inputs, "InputCapture::capture")
        cls.export_body = function(cls.inputs, "InputCapture::exportNew")
        cls.generic = function(cls.device, "D3D9DeviceEx::War3TryAppendSemanticShadowPacket")
        cls.producer = function(cls.device, "D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer")
        cls.direct = function(cls.device, "D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped")
        cls.legacy = function(cls.device, "D3D9DeviceEx::War3TryCaptureShadowCaster")

    def assert_order(self, source, *steps):
        body = tight(source)
        cursor = 0
        for step in steps:
            token = tight(step)
            position = body.find(token, cursor)
            self.assertGreaterEqual(position, cursor, f"missing or reordered code: {step}")
            cursor = position + len(token)

    def test_device_input_gate_precedes_all_extra_state_reads(self):
        body = tight(self.capture_helper)
        guard = "if(!war3::tools::evidence::InputsEnabled())return{};"
        self.assertLess(body.index(guard), body.index("m_state."))
        self.assert_order(body, guard, "const n::MaterialFactsKey key", "MaterialFactsKeyValid(key)")

    def test_valid_key_gate_precedes_material_and_array_reads(self):
        body = tight(self.capture_helper)
        guard = "if(!n::MaterialFactsKeyValid(key))return{};"
        for first_read in ("m_state.renderStates", "m_state.textureStages", "m_state.samplerStates",
                           "&m_state.material", "m_state.transforms"):
            with self.subTest(read=first_read):
                self.assertLess(body.index(guard), body.index(first_read))
        key, _ = block_after(self.capture_helper, "const n::MaterialFactsKey key")
        self.assert_order(key, "m_war3ShadowPersistentFrameSerial", "m_war3GpuSkinMapEpoch",
                          "m_war3GpuSkinDeviceEpoch", "uintptr_t(renderablePart)", "layerIndex")

    def test_sampler_capture_uses_public_state_ids(self):
        helper = tight(self.capture_helper)
        self.assertIn("sampler[i]=m_state.samplerStates[0][n::SamplerStates[i]];", helper)
        getter = function(self.device, "D3D9DeviceEx::GetSamplerState")
        self.assert_order(getter, "Sampler = RemapSamplerState(Sampler)",
                          "*pValue = m_state.samplerStates[Sampler][Type]")
        state, _ = block_after(self.facts, "SamplerStates[SamplerStateCount] =")
        self.assertEqual(tuple(map(int, re.findall(r"\d+", code(state)))), contract.NATIVE_SAMPLER_STATES)

    def test_stage_capture_matches_existing_remapping(self):
        self.assertIn("stage[i]=m_state.textureStages[0][RemapTextureStageStateType("
                      "D3DTEXTURESTAGESTATETYPE(n::StageStates[i]))];", tight(self.capture_helper))
        getter = function(self.device, "D3D9DeviceEx::GetTextureStageState")
        self.assert_order(getter, "auto dxvkType = RemapTextureStageStateType(Type)",
                          "*pValue = m_state.textureStages[Stage][dxvkType]")
        state, _ = block_after(self.facts, "StageStates[StageStateCount] =")
        self.assertEqual(tuple(map(int, re.findall(r"\d+", code(state)))), contract.NATIVE_STAGE_STATES)

    def test_texture_transform_zero_maps_to_device_storage(self):
        body = tight(self.capture_helper)
        if "texture0Index" in body:
            self.assert_order(body, "texture0Index=GetTransformIndex(D3DTS_TEXTURE0)",
                              "m_state.transforms[texture0Index]")
        else:
            self.assertIn("m_state.transforms[GetTransformIndex(D3DTS_TEXTURE0)]", body)
        self.assert_order(body, "const auto& texture0", "static_assert(sizeof(texture0)==sizeof(transform)",
                          "std::memcpy(transform,&texture0,sizeof(transform))")

    def test_material_storage_assertions_precede_bitwise_copy(self):
        self.assert_order(self.capture_helper,
                          "static_assert(sizeof(D3DMATERIAL9) == sizeof(material)",
                          "static_assert(offsetof(D3DMATERIAL9, Power) == 16 * sizeof(float)",
                          "std::memcpy(material, &m_state.material, sizeof(material))",
                          "return n::CaptureMaterialFacts(true, key, {render, stage, sampler, material, transform})")

    def test_shared_capture_gate_precedes_borrowed_pointer_inspection(self):
        body = function(self.facts, "CaptureMaterialFacts")
        self.assert_order(body, "if (!enabled) return facts", "if (!MaterialFactsKeyValid(key)",
                          "facts.renderFrame = key.renderFrame", "std::memcpy(facts.renderStates",
                          "std::memcpy(facts.textureTransform0", "facts.available = 1")
        for view in ("views.renderStates", "views.textureStage0", "views.sampler0", "views.material", "views.textureTransform0"):
            self.assertLess(tight(body).index("if(!enabled)returnfacts;"), tight(body).index(view))

    def test_all_owned_record_layers_use_value_pod(self):
        for source, anchor in ((self.device_header, "struct War3DrawTimeVBEntry"),
                               (self.scene, "struct War3ShadowCasterDraw"),
                               (self.payload, "struct War3ShadowAlphaTestPayload")):
            with self.subTest(record=anchor):
                record, _ = block_after(source, anchor)
                self.assertRegex(code(record), r"\bNativeDrawMaterialFacts\s+nativeDrawMaterial\s*;")
                self.assertNotRegex(code(record), r"\bNativeDrawMaterialFacts\s*[*&]")
        record, _ = block_after(self.facts, "struct NativeDrawMaterialFacts")
        self.assertNotRegex(code(record), r"\b(?:Rc|vector|shared_ptr|unique_ptr)\s*[<*]|\*")
        self.assertIn("std::is_trivially_copyable<NativeDrawMaterialFacts>::value", code(self.facts))
        self.assertIn("std::is_standard_layout<NativeDrawMaterialFacts>::value", code(self.facts))

    def test_generic_entry_source_travels_with_world_and_geometry(self):
        cache_branch, _ = block_after(self.generic, "if (War3DrawTimeVBCacheRuntime() && geometry->vertexBlendEnabled)")
        entry_branch, _ = block_after(cache_branch, "if (entryFresh)")
        self.assert_order(entry_branch, "const auto& entry = vbIt->second",
                          "draw.positionStorage = entry.positionBuffer", "draw.worldMatrix = entry.capturedWorldMatrix",
                          "draw.nativeDrawMaterial = entry.nativeDrawMaterial")
        self.assertNotIn("War3CaptureNativeDrawMaterialFacts", code(entry_branch))

    def test_exact_producer_entry_copy_precedes_submission(self):
        self.assert_order(self.producer, "draw.positionStorage = entry.positionBuffer",
                          "draw.worldMatrix = entry.capturedWorldMatrix",
                          "draw.nativeDrawMaterial = entry.nativeDrawMaterial",
                          "draw.alphaTestEnabled = entry.alphaTestEnabled",
                          "m_war3Scene.shadowCasters.emplace_back(std::move(draw))")

    def test_direct_fast_entry_copy_stays_inside_fast_lambda(self):
        fast, _ = block_after(self.direct, "auto tryAppendDrawTimeFastEligible =")
        self.assert_order(fast, "auto& entry = vbIt->second", "if (!entryFresh)",
                          "draw.positionStorage = entry.positionBuffer", "draw.worldMatrix = entry.capturedWorldMatrix",
                          "draw.nativeDrawMaterial = entry.nativeDrawMaterial",
                          "draw.alphaTestEnabled = entry.alphaTestEnabled",
                          "m_war3Scene.shadowCasters.emplace_back(std::move(draw))")
        self.assertNotIn("War3CaptureNativeDrawMaterialFacts", code(fast))

    def test_final_alpha_override_changes_fact_source_inside_same_branch(self):
        alpha = next(body for body in bodies_after(self.generic, "if (semanticAlphaPayloadState == 5u)")
                     if "draw.nativeDrawMaterial = semanticAlphaPayload.nativeDrawMaterial" in code(body))
        self.assert_order(alpha, "draw.alphaRef = semanticAlphaPayload.alphaRef",
                          "draw.nativeDrawMaterial = semanticAlphaPayload.nativeDrawMaterial",
                          "draw.uvStorage = semanticAlphaPayload.uvStorage",
                          "draw.diffuseTexture = semanticAlphaPayload.diffuseTexture")
        self.assert_order(self.generic, "draw.nativeDrawMaterial = entry.nativeDrawMaterial",
                          "if (semanticAlphaPayloadState == 5u)", "m_war3Scene.shadowCasters.emplace_back(std::move(draw))")

    def test_metadata_fact_source_matches_exact_payload_identity(self):
        metadata = function(self.device, "D3D9DeviceEx::War3CaptureShadowDrawMetadata")
        self.assert_order(metadata, "metadata.alpha.renderablePart = contract.renderablePart",
                          "metadata.alpha.nativeDrawMaterial = War3CaptureNativeDrawMaterialFacts("
                          "contract.renderablePart, exactQuery.layerIndex)",
                          "if (!metadata.alpha.valid())", "metadataStore.publish(std::move(metadata))")

    def test_live_entry_capture_matches_selected_entry_identity(self):
        for name in ("cached", "entry"):
            with self.subTest(entry=name):
                self.assert_order(self.legacy, f"{name}.capturedWorldMatrix =",
                                  f"{name}.nativeDrawMaterial = War3CaptureNativeDrawMaterialFacts("
                                  f"{name}.renderablePart, {name}.layerIndex)")

    def test_finalized_legacy_draw_does_not_manufacture_missing_identity(self):
        for name in ("draw", "compatDraw"):
            with self.subTest(draw=name):
                self.assert_order(self.legacy, f"if (!finalizeShadowDrawCommon({name}))",
                                  f"{name}.nativeDrawMaterial = War3CaptureNativeDrawMaterialFacts("
                                  f"{name}.shadowRenderablePart, {name}.shadowLayerIndex)",
                                  "shadowCapturePostTiming.enter(War3ShadowCapturePostPhase::Submit)")
        self.assertNotRegex(code(self.capture_helper), r"\brenderablePart\s*=(?!=)")

    def test_frame_input_metadata_owns_native_and_final_values(self):
        draw, _ = block_after(self.inputs, "struct Draw")
        self.assertRegex(code(draw), r"\bNativeDrawMaterialFacts\s+nativeMaterial\s*;")
        self.assertRegex(code(draw), r"\bFinalShadowMaterialFacts\s+finalMaterial\s*;")
        self.assertNotRegex(code(draw), r"\b(?:NativeDrawMaterialFacts|FinalShadowMaterialFacts)\s*[*&]")
        self.assertIn("std::is_trivially_copyable<FinalShadowMaterialFacts>::value", code(self.export))
        self.assertIn("std::is_standard_layout<FinalShadowMaterialFacts>::value", code(self.export))

    def test_capture_identity_gate_precedes_fact_copy_and_records_rejection(self):
        branch, _ = block_after(self.capture, "if (d.nativeDrawMaterial.available)")
        self.assert_order(branch,
                          "if (d.mapEpoch==s.key.mapEpoch && d.deviceEpoch==s.key.deviceEpoch &&",
                          "war3::render::native_input::MaterialFactsSameDrawIdentity("
                          "d.nativeDrawMaterial, d.mapEpoch, d.deviceEpoch, r.part, d.shadowLayerIndex))",
                          "r.nativeMaterial=d.nativeDrawMaterial", "else ++s.nativeMaterialIdentityRejected")
        self.assert_order(self.capture, "Draw r{}", "r.part=uint64_t(reinterpret_cast<uintptr_t>(d.shadowRenderablePart))",
                          "if (d.nativeDrawMaterial.available)", "auto& material=r.finalMaterial", "r.words=")

    def test_identity_filter_preserves_raw_bad_values_and_separate_frame_domain(self):
        identity = function(self.facts, "MaterialFactsSameDrawIdentity")
        self.assert_order(identity, "MaterialFactsSchemaValid(facts)", "facts.mapEpoch == mapEpoch",
                          "facts.deviceEpoch == deviceEpoch", "facts.renderablePart == part", "facts.layerIndex == layer")
        self.assertNotIn("MaterialFactsFinite", code(identity))
        self.assertNotRegex(code(identity), r"facts\.renderFrame\s*[=!<>]")
        self.assertNotRegex(code(self.capture), r"\b(?:nativeMaterial|nativeDrawMaterial)\.renderFrame\s*=(?!=)")

    def test_final_material_is_copied_from_selected_replay_draw(self):
        material = self.capture.split("auto& material=r.finalMaterial;", 1)[1].split("r.words=", 1)[0]
        assignments = {
            "alphaRefBits": "war3::render::native_input::FloatBits(d.alphaRef)",
            "alphaTest": "d.alphaTestEnabled", "alphaBlend": "d.alphaBlendEnabled",
            "depthTest": "d.depthTestEnabled", "depthWrite": "d.depthWriteEnabled",
            "additive": "d.additiveBlend", "uvBinding": "d.uvBinding", "uvStride": "d.uvStride",
            "uvOffset": "d.uvOffset", "uvFormat": "uint32_t(d.uvFormat)", "samplerIndex": "d.diffuseSamplerIndex",
        }
        for field, value in assignments.items():
            with self.subTest(field=field):
                self.assertIn(tight(f"material.{field}={value};"), tight(material))
        self.assert_order(material, "if(const auto* descriptor=d.CurrentTextureDescriptor())",
                          "material.textureView=uint64_t(descriptor->legacy.image.imageView)",
                          "if(d.diffuseSampler!=nullptr)",
                          "material.sampler=uint64_t(d.diffuseSampler->getDescriptor().samplerObject)")
        self.assertNotIn("m_state.", code(material))

    def test_real_exporter_uses_shared_encoder_per_draw_after_schema3(self):
        result, _ = block_after(self.export_body, "json result=")
        self.assertIn('{"schema",3}', tight("{" + result + "}"))
        self.assertIn('{"rootCauseReady",false}', tight("{" + result + "}"))
        draw_loop, _ = block_after(self.export_body, "for(const auto& r:s.draws)")
        self.assert_order(draw_loop, 'batch["draws"].push_back(',
                          '{"renderInputEvidence",EncodeNativeRenderInputEvidence(r.nativeMaterial,r.finalMaterial)}',
                          '{"part",std::to_string(r.part)}')
        self.assertNotIn("MaterialFactsMatches", code(draw_loop))

    def test_encoder_preserves_original_frame_bits_and_unknown_zero(self):
        encoder = function(self.export, "EncodeNativeRenderInputEvidence")
        self.assert_order(encoder, "NativeDrawMaterialFacts unknown", "const bool available = native.available == 1",
                          "const auto& observed = available ? native : unknown", "json observation =")
        observation, _ = block_after(encoder, "json observation =")
        for key, value in (("frameDomain", '"ShadowProducerSerial"'),
                           ("sourceFrame", "std::to_string(observed.renderFrame)"),
                           ("materialBits", "observed.material"),
                           ("textureTransform0Bits", "observed.textureTransform0")):
            self.assertIn(tight('{"' + key + '",' + value + '}'), tight(observation))
        self.assertNotRegex(code(encoder), r"observed\.[A-Za-z_]+\s*=(?!=)")
        self.assertNotIn("FloatBits(", code(encoder))
        self.assertNotIn("MaterialFactsFinite", code(encoder))

    def test_encoder_retains_fixed_missing_shared_input_list(self):
        encoder = function(self.export, "EncodeNativeRenderInputEvidence")
        missing = re.search(r'"requiredSharedFieldsMissing"\s*,\s*json::array\(\{([^}]+)\}\)', code(encoder))
        self.assertIsNotNone(missing)
        self.assertEqual(tuple(re.findall(r'"([^"\\]+)"', missing.group(1))),
                         contract.NATIVE_REQUIRED_SHARED_FIELDS_MISSING)
        self.assertNotIn('"sharedRendererReady",true', tight(encoder))

    def test_stage13_retained_replay_keeps_observation_frame(self):
        stage13, _ = block_after(self.device, "if (War3Stage13StaticRetentionRuntime())")
        self.assert_order(stage13, "War3ShadowCasterDraw replay = retained.draw",
                          "replay.positionStorage = replayStorage", "m_war3Scene.shadowCasters.emplace_back(std::move(replay))")
        self.assertNotIn("War3CaptureNativeDrawMaterialFacts", code(stage13))
        self.assertNotRegex(code(stage13), r"\bnativeDrawMaterial(?:\.renderFrame)?\s*=(?!=)")
        retention = next(body for body in bodies_after(self.legacy, "if (stage13CpuRetentionPath)")
                         if "War3ShadowCasterDraw retainedDraw = draw" in code(body))
        self.assert_order(retention, "War3ShadowCasterDraw retainedDraw = draw",
                          "retained.draw = std::move(retainedDraw)")

    def test_s1_early_replay_keeps_observation_frame(self):
        store = function(self.device, "D3D9DeviceEx::War3StoreS1TerrainEarlyCacheEntry")
        self.assert_order(store, "entry.draw = draw", "entry.lastSeenFrame = m_war3ShadowPersistentFrameSerial")
        reuse, _ = block_after(self.legacy, "if (backingValid &&")
        self.assert_order(reuse, "War3ShadowCasterDraw hitDraw = earlyIt->second.draw", "hitDraw.worldMatrix = s1World",
                          "m_war3Scene.shadowCasters.emplace_back(std::move(hitDraw))")
        self.assertNotIn("War3CaptureNativeDrawMaterialFacts", code(reuse))
        self.assertNotRegex(code(reuse + store), r"\bnativeDrawMaterial(?:\.renderFrame)?\s*=(?!=)")
        self.assertNotRegex(code(self.device), r"\bnativeDrawMaterial\s*\.\s*renderFrame\s*=(?!=)")

    def test_new_fact_path_adds_no_env_or_gpu_readback(self):
        new_scope = (self.capture_helper, function(self.facts, "CaptureMaterialFacts"),
                     function(self.export, "EncodeNativeRenderInputEvidence"))
        phase = self.capture.split("Draw r{};", 1)[1].split("r.words=", 1)[0]
        for index, body in enumerate((*new_scope, phase)):
            with self.subTest(scope=index):
                self.assertNotRegex(code(body), r"\b(?:getenv|getEnvVar|GetEnvironmentVariable\w*|SafeRead\w*|ReadProcessMemory)\s*\(")
                self.assertNotRegex(code(body), r"\bvk[A-Z]\w*\s*\(|\b(?:waitIdle|waitFor|mapPtr|createBuffer)\s*\(|ctx\s*->")

    def test_budget_cap_and_live_size_assert_cover_owned_metadata(self):
        self.assertRegex(code(self.core), r"constexpr\s+uint32_t\s+MaxDrawMetadataBytes\s*=\s*1472\s*;")
        budget = function(self.core, "HostPayloadBudgetForSlots")
        self.assertIn("returnuint64_t(slots)*(BytesPerSlot+uint64_t(DrawsPerSlot)*MaxDrawMetadataBytes);", tight(budget))
        _, end = block_after(self.inputs, "struct Draw")
        after_draw = code(self.inputs[end:]).split("json spanJson", 1)[0]
        self.assertIn("static_assert(sizeof(Draw)<=MaxDrawMetadataBytes", tight(after_draw))
        evidence = read("src/d3d9/war3/tools/war3_frame_evidence.cpp")
        control = function(evidence, "Control")
        self.assertIn('if(action=="arm")', tight(control))
        arm, _ = block_after(control, 'if(action==')
        self.assert_order(arm, "InputsEnabled()?inputs::HostPayloadBudgetForSlots(profile.inputSlots):0",
                          "const auto additional=bytes+inputBudget", "RecorderMemoryAdmission(memory,additional,bytes+65536)")

    def test_boundary_scanner_ignores_comment_and_string_decoys(self):
        sample = '// selected() { fake(); }\nconst char* label="selected() { fake(); }";\nvoid selected() { if (ok) { actual(); } }'
        selected = function(sample, "selected")
        self.assertEqual(tight(selected), "if(ok){actual();}")
        branch, _ = block_after(selected, "if (ok)")
        self.assertEqual(tight(branch), "actual();")


if __name__ == "__main__":
    unittest.main()
