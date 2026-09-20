import unittest
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def source(p): return (ROOT/p).read_text(encoding='utf-8')
DEVICE = source('src/d3d9/d3d9_native_light.cpp')
MAIN = source('src/d3d9/d3d9_device.cpp')
FF = source('src/d3d9/d3d9_fixed_function.cpp')
BRIDGE = source('src/d3d9/war3/model/war3_native_light_bridge.cpp')
SHADOW = source('src/d3d9/d3d9_war3_shadow.cpp')

class TransactionalNativeLight(unittest.TestCase):
    def test_primary_and_native_calls_retained(self):
        self.assertIn('originalFlushLight(device, index, value, enabled)', BRIDGE)
        self.assertIn('originalEgress(index, light)', BRIDGE)
        self.assertNotIn('AddPointLight(', DEVICE)
        self.assertNotIn('LightEnable(', DEVICE)
        self.assertIn('m_module.opStore(m_ps.out.COLOR, primary)', FF)
        self.assertIn('m_module.opStore(m_vs.out.COLOR[0], finalColor0)', FF)
    def test_identity_precedes_value_comparison(self):
        claim = BRIDGE.split('bool ClaimDrawLight',1)[1].split('bool ValidateLease',1)[0]
        for predicate in ('binding.owner.generation != slots[index].generation',
                          'binding.owner.light != slots[index].light',
                          'std::memcmp(&value, &binding.value, sizeof(value))',
                          'worldSerial != state.frame', 'Read(binding.owner.light, current)',
                          'std::memcmp(&current, &binding.owner.copied, 36)'):
            self.assertIn(predicate, claim)
        self.assertIn('sizeof(D3DLIGHT9) == 0x68', BRIDGE)
        self.assertIn('{0xef2d0, (void*)&FlushLight', BRIDGE)
    def test_lease_is_lifetime_and_frame_bound(self):
        validate = BRIDGE.split('bool ValidateLease',1)[1].split('void CloneInstance',1)[0]
        for predicate in ('count > 2', 'serial != state.frame', 'sample.epoch != state.epoch',
                          'sample.frame != serial', 'owner->second.generation != sample.generation',
                          'std::memcmp(&current, &sample.value, 36)'):
            self.assertIn(predicate, validate)
    def test_second_color_has_same_geometry_and_original_alpha(self):
        self.assertIn('compilePSColor(m_ps.in.COLOR[0], m_ps.in.COLOR[1])', FF)
        self.assertIn('compilePSColor(m_ps.alternateColor[endpoint][0], m_ps.alternateColor[endpoint][1])', FF)
        wrapper = FF.split('void D3D9FFShaderCompiler::compilePS()',1)[1].split('uint32_t D3D9FFShaderCompiler::compilePSColor',1)[0]
        self.assertEqual(wrapper.count('alphaTestPS();'), 1)
        self.assertIn('alternate, primary, swizzle.size()', wrapper)
        self.assertIn('D3DTOP_DOTPRODUCT3', DEVICE)
        self.assertIn('semantic.usageIndex < 8', FF)
        self.assertNotIn('ctx->draw', DEVICE)
    def test_ambient_not_removed(self):
        self.assertIn('mat_ambient, ambientValue, alternate', FF)
        self.assertIn('mat_diffuse, alternateDiffuse[endpoint], alternate', FF)
        self.assertIn('mat_specular, alternateSpecular[endpoint]', FF)
        self.assertNotIn('alternateAmbient', FF)
        self.assertIn('Direction.w = (m_nativeColor.mask', MAIN)
    def test_draw_admission(self):
        for predicate in ('UseProgrammableVS()', 'UseProgrammablePS()',
                          'War3ShouldOverrideWorldMaterial()', 'War3ShouldOverridePostProcessMaterial()',
                          'm_textureSlotTracking.hazardRT', 'm_textureSlotTracking.hazardDS',
                          'VK_SAMPLE_COUNT_1_BIT', 'application-mrt', 'rgb-dependent-alpha',
                          'animated-within-frame', 'world-identity'):
            self.assertIn(predicate, DEVICE)
        for reason in ('stretch-rect', 'color-fill', 'clear-after-first-lease', 'gpu-skin-vs-interface'):
            self.assertIn('War3AbortNativeColor("'+reason+'")', MAIN)
    def test_shader_pair_not_ubershader(self):
        self.assertIn('m_d3d9Options.ffUbershaderVS && !m_nativeLightSplit', MAIN)
        self.assertIn('m_d3d9Options.ffUbershaderFS && !m_nativeLightSplit', MAIN)
        self.assertIn('key.Data.Contents.NativeLightSplit = m_nativeLightSplit', MAIN)
        self.assertIn('stage0.NativeLightSplit = m_nativeLightSplit', MAIN)
    def test_attachment_blend_and_fenced_ownership(self):
        self.assertIn('attachments.color[i+1].view = m_nativeColor.alternate[i]', MAIN)
        self.assertIn('(writeMasks & 0xfu) << (4*i)', MAIN)
        self.assertIn('(alphaMasks & 1u) << i', MAIN)
        self.assertIn('src = Rc<DxvkImage>(original->image())', DEVICE)
        self.assertIn('ctx->copyImage(dst, layers, {0,0,0}, src, layers, {0,0,0}, extent)', DEVICE)
    def test_no_commit_with_stale_or_missing_cube(self):
        gate = SHADOW.split('bool nativeCommit =',1)[1].split('VkExtent3D extent =',1)[0]
        for predicate in ('pointShadowPublishedStateMatchesCurrentPlan()',
                          'm_pointShadowPublishedFrameSerial == input.frameSerial',
                          'm_pointShadowPublishedLightGeneration == pointLightSnapshot.generation',
                          'm_pointShadowFaceValidMask[i] == 0x3fu', 'dropped == 0',
                          'freezePointUniform(author)',
                          'm_pointLightsEnabled = input.settings && input.settings->shadows.pointLightsEnabled',
                          'renderPointShadow(ctx, input, author, &replayDraws, input.settings.get())'):
            self.assertIn(predicate, gate)
        self.assertIn('copyColor(ctx, input.colorView)', SHADOW)
        self.assertIn('nativeCommit ? input.nativeLightBaselines : std::array<Rc<DxvkImageView>,3>()', SHADOW)
        self.assertIn('nativeSlot, nativeCommit ? input.nativeLightCount : 0u', SHADOW)
    def test_manual_channel_keeps_priority_and_no_pointer_publication(self):
        self.assertIn('author.count + tx.count > 16 || authorShadow + tx.count > 4', DEVICE)
        self.assertLess(DEVICE.index('i<authorShadow'), DEVICE.index('i<tx.count'))
        self.assertIn('pointShadowTemporalReuse = false', DEVICE)
        self.assertIn('frame.shadowCount = frame.count', DEVICE)
        publish = DEVICE.split('void D3D9DeviceEx::War3PublishNativeColor',1)[1]
        self.assertNotIn('sample.nativeLight', publish)
        self.assertNotIn('source.range =', publish)
    def test_failure_reuses_frozen_author_values_and_original_settings(self):
        self.assertIn('author.generation = input.nativeLightAuthorGeneration', SHADOW)
        self.assertIn('pointLightSnapshot.lights[i].id > 0', SHADOW)
        self.assertIn('(canonicalFallbackSettings || input.nativeLightCount)', SHADOW)
        self.assertIn('? War3PointShadowPersistentMode::Off : PointShadowPersistentMode()', SHADOW)
        self.assertIn('!canonicalFallbackSettings && m_pointShadowCpuPlan.ready', SHADOW)
        self.assertIn('syncInput.settings = *canonicalFallbackSettings', SHADOW)
    def test_initial_clear_is_before_snapshot_not_an_invalid_draw(self):
        self.assertIn('if (m_nativeColor.active) War3AbortNativeColor("clear-after-first-lease")', MAIN)
        self.assertIn('!m_nativeColor.active && !war3::native_light::CurrentWorldSerial()', DEVICE)
    def test_post_world_mirrors_but_cannot_claim_lights(self):
        self.assertIn('world ? world : m_nativeColor.worldSerial', DEVICE)
        self.assertIn('if (!world && !m_nativeColor.active) return', DEVICE)
        self.assertIn('std::array<uint32_t, 2> mask = {};', DEVICE)
        self.assertIn('if (!world) ++m_nativeColor.tailDraws;', DEVICE)
        self.assertIn('if (lighting && world)', DEVICE)
        self.assertIn('std::array<uint32_t, 4> components = {keep, keep, keep, keep}', FF)
    def test_previous_census_is_priority_not_value_authority(self):
        self.assertIn('previous.serial == world - 1', DEVICE)
        self.assertIn('m_war3Scene.worldCamera.viewProj', DEVICE)
        self.assertIn('sample.generation) == m_nativeColor.preferred.end()', DEVICE)
        self.assertIn('ClaimDrawLight(slot, m_state.lights[slot].value(), sample)', DEVICE)
        self.assertIn('ValidateLease(tx.lights.data(), tx.count, tx.worldSerial)', DEVICE)
    def test_listener_admission_is_identity_not_name(self):
        modules = source('src/d3d9/war3/platform/war3_module_api.cpp')
        example = source('src/d3d9/war3/war3_user_example.cpp')
        query = modules.split('bool HasNativeColorWriteModules()',1)[1].split('void InitializeModules',1)[0]
        self.assertIn('module.callbacks.onRenderEvent', query)
        self.assertIn('!war3example::IsNativeColorSafeCallback', query)
        self.assertNotIn('info.name', query)
        self.assertIn('return callback == &OnRenderEvent', example)
        callback = example.split('void OnRenderEvent(',1)[1].split('bool RegisterUserExampleModuleInternal',1)[0]
        self.assertIn('eventId == war3shader::RenderEventID::POST_PROCESS_BEGIN', callback)
        self.assertNotIn('Draw', callback)
        self.assertNotIn('Vulkan', callback)
        self.assertIn('input.lighting->nativeColorExternalWriteHazard', SHADOW)
    def test_exact_native_contribution_not_relighting_dark_color(self):
        shader = source('subprojects/war3fx/shaders/war3_shadow_receiver.frag')
        self.assertIn('!tx.count || tx.count > 2', DEVICE)
        self.assertIn('std::array<war3::native_light::Sample, 2>', source('src/d3d9/d3d9_device.h'))
        self.assertIn('mix(withoutA, col.rgb, nativeVisibility.x)', shader)
        self.assertIn('mix(withoutBoth, withoutB, nativeVisibility.x)', shader)
        self.assertIn('mix(noB, withB, nativeVisibility.y)', shader)
        self.assertIn('accumLight += (resolved - col.rgb) * mul', shader)
        self.assertIn('if (nativeValid && i >= nativeSlot && i-nativeSlot < nativeCount) continue', shader)
        self.assertIn('nativeCount <= lightUbo.count-nativeSlot', SHADOW)
        self.assertIn('std::array<DxvkDescriptorWrite, 17>', SHADOW)
        layout = SHADOW.split('War3ShadowReceiverPass::createPipelineLayout() const',1)[1].split('War3ShadowReceiverPass::',1)[0]
        self.assertIn('std::array<DxvkDescriptorSetLayoutBinding, 17>', layout)
        self.assertEqual(layout.count('DxvkDescriptorSetLayoutBinding('),17)
        self.assertIn('barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL', SHADOW)
        self.assertIn('ctx->track(nativeBaseline->image(), DxvkAccess::Read)', SHADOW)
    def test_emitter_exclusion_is_owner_bound_and_only_own_light(self):
        self.assertIn('tx.lights[0].source.policyIndex == 8u', DEVICE)
        self.assertIn('owner->second.model != sample.nativeModel', BRIDGE)
        self.assertIn('snapshot.nativeLightEmitterGeneration == tx.lights[0].generation', DEVICE)
        self.assertIn('shadowCasters[i].nativeLightEmitterGeneration == tx.lights[0].generation', DEVICE)
        self.assertEqual(MAIN.count('nativeLightEmitterGeneration = war3::native_light::EmitterGenerationForModel('),3)
        self.assertIn('input.nativeEmitterFallbackIndices.push_back(i)', DEVICE)
        self.assertIn('const auto* emitter = &input.scene.shadowFallbacks[index].snapshot', SHADOW)
        self.assertIn('if (light.id == -1 && std::find(input.nativeEmitterReplayIndices.begin()', SHADOW)
        self.assertIn('workerInput.nativeEmitterReplayIndices = NativeEmitterReplayIndices(input, replayDraws)', SHADOW)
        self.assertIn('syncInput.nativeEmitterReplayIndices = NativeEmitterReplayIndices(input, replayDrawsOverride)', SHADOW)
    def test_visibility_endpoints_and_convex_color_model(self):
        # Algebra model only; actual GPU/FFP equality is a separate image gate.
        randomizer = random.Random(122)
        for _ in range(1000):
            original, without, visibility = (randomizer.random() for _ in range(3))
            result = original - (original-without)*(1-visibility)
            self.assertAlmostEqual(original - (original-without)*0, original)
            self.assertAlmostEqual(original - (original-without)*1, without)
            self.assertGreaterEqual(result + 1e-12, min(original, without))
            self.assertLessEqual(result - 1e-12, max(original, without))

if __name__ == '__main__': unittest.main()
