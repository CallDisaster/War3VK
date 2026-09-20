#include "d3d9_device.h"
#include "war3_shader_api.h"
#include "../util/util_env.h"
#include <cstring>

namespace dxvk {

void D3D9DeviceEx::War3SetNativeColorSplit(bool enabled) {
  if (m_nativeLightSplit == enabled) return;
  m_nativeLightSplit = enabled;
  m_dirty.set(D3D9DeviceDirtyFlag::Framebuffer);
  m_dirty.set(D3D9DeviceDirtyFlag::BlendState);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexShader);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelShader);
  m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  m_dirty.set(D3D9DeviceDirtyFlag::FFPixelData);
  if (!enabled) {
    if (!UseProgrammableVS() && m_d3d9Options.ffUbershaderVS)
      BindFFUbershader<DxsoProgramType::VertexShader>();
    if (!UseProgrammablePS() && m_d3d9Options.ffUbershaderFS)
      BindFFUbershader<DxsoProgramType::PixelShader>();
  }
}

void D3D9DeviceEx::War3AbortNativeColor(const char* reason) {
  if (m_nativeColor.sealed) return; // later screenshot copies cannot revoke an already queued commit
  if (!m_nativeColor.active && !war3::native_light::CurrentWorldSerial()) return;
  if (!m_nativeColor.worldSerial)
    m_nativeColor.worldSerial = war3::native_light::CurrentWorldSerial();
  if (!m_nativeColor.invalid && m_nativeColor.worldSerial % 120 == 1)
    Logger::info(str::format("NativeModelLights color fallback frame=",
      m_nativeColor.worldSerial, " reason=", reason));
  m_nativeColor.invalid = true;
  War3SetNativeColorSplit(false);
}

void D3D9DeviceEx::War3ResetNativeColor() {
  War3SetNativeColorSplit(false);
  // In-flight commands retain Rc owners. Reuse storage only in queue order.
  auto storage = m_nativeColor.alternate;
  m_nativeColor = {};
  m_nativeColor.alternate = std::move(storage);
}

void D3D9DeviceEx::War3PrepareNativeColor() {
  using namespace war3::native_light;
  if (!Enabled()) return;
  // Deferred by the author: retained only as an explicit research opt-in.
  static const bool consumer = env::getEnvVar("DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER") == "1";
  if (!consumer || m_nativeColor.sealed || m_nativeColor.invalid) return;
  const uint64_t world = CurrentWorldSerial();
  // The native world call returns before the established BeforeUi boundary.
  // Keep mirroring intervening draws, but never claim new lights outside it.
  // Final publication still validates the original frame/generation lease.
  const uint64_t serial = world ? world : m_nativeColor.worldSerial;
  if (!world && !m_nativeColor.active) return;
  if (m_nativeColor.active && serial != m_nativeColor.worldSerial) {
    War3AbortNativeColor("world-identity"); return;
  }
  if (!m_war3Pipeline || !m_war3Pipeline->WantsBeforeUiInsertion() ||
      !m_war3Pipeline->IsPassEnabled("ShadowReceiver") ||
      !m_war3Pipeline->GetSettings().shadows.enabled ||
      war3shader::internal::HasNativeColorWriteListeners()) {
    if (m_nativeColor.worldSerial == 0 && serial % 120 == 1)
      Logger::info(str::format("NativeModelLights pipeline admission serial=", serial,
        " pipeline=", m_war3Pipeline != nullptr,
        " wants=", m_war3Pipeline && m_war3Pipeline->WantsBeforeUiInsertion(),
        " receiver=", m_war3Pipeline && m_war3Pipeline->IsPassEnabled("ShadowReceiver"),
        " shadow=", m_war3Pipeline && m_war3Pipeline->GetSettings().shadows.enabled,
        " listeners=", war3shader::internal::HasNativeColorWriteListeners()));
    War3AbortNativeColor("pipeline"); return;
  }
  if (UseProgrammableVS() || UseProgrammablePS() || IsSWVP() ||
      War3ShouldOverrideWorldMaterial() || War3ShouldOverridePostProcessMaterial() ||
      m_textureSlotTracking.hazardRT || m_textureSlotTracking.hazardDS ||
      m_state.renderStates[D3DRS_SRGBWRITEENABLE] ||
      m_state.renderTargets[0] == nullptr || m_state.depthStencil == nullptr) {
    if (serial % 120 == 1)
      Logger::info(str::format("NativeModelLights draw admission serial=", serial,
        " pvs=", UseProgrammableVS(), " pps=", UseProgrammablePS(), " swvp=", IsSWVP(),
        " worldOverride=", War3ShouldOverrideWorldMaterial(),
        " postOverride=", War3ShouldOverridePostProcessMaterial(),
        " hazardRT=", m_textureSlotTracking.hazardRT,
        " hazardDS=", m_textureSlotTracking.hazardDS,
        " srgb=", m_state.renderStates[D3DRS_SRGBWRITEENABLE],
        " rt=", m_state.renderTargets[0] != nullptr, " ds=", m_state.depthStencil != nullptr));
    War3AbortNativeColor("draw-state"); return;
  }
  for (uint32_t i = 1; i < m_state.renderTargets.size(); ++i)
    if (m_state.renderTargets[i] != nullptr) { War3AbortNativeColor("application-mrt"); return; }
  for (uint32_t i=0; i<caps::TextureStageCount; ++i) {
    const auto& stage = m_state.textureStages[i];
    if (stage[DXVK_TSS_COLOROP] == D3DTOP_DISABLE) break;
    if (stage[DXVK_TSS_COLOROP] == D3DTOP_DOTPRODUCT3 ||
        stage[DXVK_TSS_ALPHAOP] == D3DTOP_DOTPRODUCT3) {
      War3AbortNativeColor("rgb-dependent-alpha"); return;
    }
  }
  auto original = m_state.renderTargets[0]->GetRenderTargetView(false);
  const auto& info = original->image()->info();
  const auto sub = original->subresources();
  if (info.sampleCount != VK_SAMPLE_COUNT_1_BIT || info.numLayers != 1 ||
      info.mipLevels != 1 || sub.baseMipLevel || sub.baseArrayLayer ||
      !(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !(info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_R8G8B8A8_UNORM) ||
      (m_nativeColor.active && original != m_nativeColor.original)) {
    War3AbortNativeColor("color-resource"); return;
  }
  if (!m_nativeColor.planned) {
    m_nativeColor.planned = true;
    // Previous census is a priority hint only, never value/lifetime authority.
    // Pick the light nearest the screen center rather than whichever a
    // terrain batch happened to bind first. Every actual claim is still fresh.
    const auto previous = Snapshot();
    std::array<float, 2> scores = {INFINITY, INFINITY};
    std::array<bool, 2> explicitRules = {};
    if (world && previous.complete && previous.serial == world - 1 &&
        m_war3Scene.worldCamera.valid) {
      for (uint32_t i=0; i<previous.count; ++i) {
        const auto& value = previous.lights[i].value;
        const auto clip = m_war3Scene.worldCamera.viewProj *
          Vector4(value.position[0], value.position[1], value.position[2], 1.0f);
        if (!std::isfinite(clip.w) || clip.w <= 0.001f) continue;
        const float x = clip.x / clip.w, y = clip.y / clip.w;
        const float score = x*x + y*y;
        if (!std::isfinite(score)) continue;
        const bool explicitRule = previous.lights[i].explicitRegistration;
        for (uint32_t j=0; j<scores.size(); ++j) if (
            (explicitRule && !explicitRules[j]) ||
            (explicitRule == explicitRules[j] && score < scores[j])) {
          for (uint32_t k=uint32_t(scores.size())-1; k>j; --k) {
            scores[k] = scores[k-1]; m_nativeColor.preferred[k] = m_nativeColor.preferred[k-1];
            explicitRules[k] = explicitRules[k-1];
          }
          scores[j] = score; m_nativeColor.preferred[j] = previous.lights[i].generation;
          explicitRules[j] = explicitRule;
          break;
        }
      }
    }
  }
  std::array<uint32_t, 2> mask = {};
  const bool lighting = m_state.renderStates[D3DRS_LIGHTING] && m_state.vertexDecl != nullptr &&
    !m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  // After the world scope, never claim/remove another light. Admitted FFP
  // draws still evaluate identical source colors in all endpoints (zero mask),
  // preserving each endpoint's own destination blend, alpha, depth and coverage.
  // All shader/resource/writer guards above remain mandatory, including RT0
  // identity. Merely having D3DRS_LIGHTING on is not a write-identity hazard.
  if (lighting && world) for (uint32_t i = 0; i < caps::MaxEnabledLights; ++i) {
    const auto slot = m_state.enabledLightIndices[i];
    if (slot == UINT32_MAX || slot >= m_state.lights.size() || !m_state.lights[slot]) continue;
    Sample sample;
    if (!ClaimDrawLight(slot, m_state.lights[slot].value(), sample)) continue;
    if (std::find(m_nativeColor.preferred.begin(), m_nativeColor.preferred.end(),
          sample.generation) == m_nativeColor.preferred.end()) continue;
    uint32_t j = 0;
    for (; j < m_nativeColor.count; ++j)
      if (m_nativeColor.lights[j].generation == sample.generation) break;
    if (j == m_nativeColor.count) {
      if (j == m_nativeColor.lights.size()) continue; // unclaimed native lighting remains
      m_nativeColor.lights[m_nativeColor.count++] = sample;
    } else if (std::memcmp(&sample.value, &m_nativeColor.lights[j].value, 36)) {
      War3AbortNativeColor("animated-within-frame"); return;
    }
    mask[j] |= 1u << i;
  }
  if (!m_nativeColor.active && !mask[0] && !mask[1]) return;
  if (!m_nativeColor.active) {
    try {
      const auto extent = info.extent;
      for (auto& alternate : m_nativeColor.alternate) {
      if (!alternate || alternate->image()->info().format != info.format ||
          alternate->mipLevelExtent(0) != extent) {
        DxvkImageCreateInfo ci;
        ci.format = info.format; ci.extent = extent;
        ci.sampleCount = VK_SAMPLE_COUNT_1_BIT; ci.numLayers = 1; ci.mipLevels = 1;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        ci.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        ci.layout = VK_IMAGE_LAYOUT_GENERAL;
        ci.debugName = "WarVK native-light transactional color";
        auto image = m_dxvkDevice->createImage(ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        DxvkImageViewKey key;
        key.viewType = VK_IMAGE_VIEW_TYPE_2D; key.format = ci.format;
        key.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        key.aspects = VK_IMAGE_ASPECT_COLOR_BIT; key.mipCount = 1; key.layerCount = 1;
        alternate = image->createView(key);
      }
      EmitCs([src = Rc<DxvkImage>(original->image()),
              dst = Rc<DxvkImage>(alternate->image()), extent](DxvkContext* ctx) {
        const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        ctx->copyImage(dst, layers, {0,0,0}, src, layers, {0,0,0}, extent);
      });
      }
      m_nativeColor.original = original; m_nativeColor.worldSerial = serial;
      m_nativeColor.active = true;
      War3SetNativeColorSplit(true);
    } catch (const std::exception&) {
      m_nativeColor.invalid = true; return;
    } catch (const DxvkError&) {
      m_nativeColor.invalid = true; return;
    }
  }
  if (mask != m_nativeColor.mask) m_dirty.set(D3D9DeviceDirtyFlag::FFVertexData);
  m_nativeColor.mask = mask;
  ++m_nativeColor.draws;
  if (!world) ++m_nativeColor.tailDraws;
}

void D3D9DeviceEx::War3PublishNativeColor(War3PipelineInput& input) {
  auto& tx = m_nativeColor;
  if (tx.sealed) return;
  tx.sealed = true;
  War3SetNativeColorSplit(false);
  if (!tx.active || tx.invalid || !tx.count || tx.count > 2 || !input.settings || !input.scene.worldCamera.valid ||
      input.colorView != tx.original || !tx.draws ||
      !war3::native_light::ValidateLease(tx.lights.data(), tx.count, tx.worldSerial)) return;
  War3PointLightFrameSnapshot author;
  if (input.settings->shadows.pointLightsEnabled) {
    const auto camera = inverse(input.scene.worldCamera.view);
    author = War3LightManager::Instance().GetFrameSnapshot(input.frameSerial,
      Vector4(camera[3].x, camera[3].y, camera[3].z, 1.0f));
  }
  const auto authorShadow = input.settings->shadows.pointShadowEnabled
    ? std::min(author.shadowCount, input.settings->shadows.pointShadowMaxLights) : 0u;
  if (author.count + tx.count > 16 || authorShadow + tx.count > 4) return;
  // Stock TorchHuman's light pivot lies inside its opaque lamp cup. Native
  // art was authored without self-shadowing. Exclude only the exact emitter
  // instance from its own cube, never by distance/path or for another light.
  // Only the independently audited 98D1 TorchHuman variant has this policy.
  // In particular, never exclude an entire wall merely because it owns a lamp.
  if (tx.lights[0].source.policyIndex == 8u) {
    for (uint32_t i=0; i<input.scene.shadowFallbacks.size(); ++i)
      if (input.scene.shadowFallbacks[i].snapshot.nativeLightEmitterGeneration == tx.lights[0].generation)
        input.nativeEmitterFallbackIndices.push_back(i);
    for (uint32_t i=0; i<input.scene.shadowCasters.size(); ++i)
      if (input.scene.shadowCasters[i].nativeLightEmitterGeneration == tx.lights[0].generation)
        input.nativeEmitterCasterIndices.push_back(i);
  }
  auto& frame = input.nativeLightSnapshot;
  frame.frameSerial = input.frameSerial;
  // No temporal reuse for this transaction; nevertheless preserve a nonzero
  // monotonic identity for existing publication seals and worker validation.
  frame.generation = input.frameSerial;
  input.nativeLightAuthorGeneration = author.generation;
  for (uint32_t i=0; i<authorShadow; ++i) frame.lights[frame.count++] = author.lights[i];
  for (uint32_t i=0; i<tx.count; ++i) {
    const auto& sample = tx.lights[i];
    const auto& value = sample.value;
    War3PointLight light;
    light.position = Vector4(value.position[0], value.position[1], value.position[2], sample.source.range);
    // Native colors are packed 0xAARRGGBB. The original ambient term stays in FFP.
    light.color = Vector4(float((value.directColor>>16)&255)/255.0f,
      float((value.directColor>>8)&255)/255.0f, float(value.directColor&255)/255.0f, value.directIntensity);
    // Diagnostic only: keep cube publication and color takeover, bypass only
    // automatic-light visibility. Never changes the authored channel.
    static const bool diagnosticUnshadowed = env::getEnvVar(
      "DXVK_WAR3_NATIVE_MODEL_LIGHT_DIAGNOSTIC_UNSHADOWED") == "1";
    light.params = Vector4(diagnosticUnshadowed ? 0.0f : 1.0f,0,0,0);
    light.id = -int32_t(i+1);
    frame.lights[frame.count++] = light;
    if (tx.worldSerial % 120 == 1)
      Logger::info(str::format("NativeModelLights selected frame=", tx.worldSerial,
        " policy=", sample.source.policyIndex, " generation=", sample.generation,
        " xyz=", value.position[0], ",", value.position[1], ",", value.position[2],
        " radius=", sample.source.range, " intensity=", value.directIntensity));
  }
  frame.shadowCount = frame.count;
  for (uint32_t i=authorShadow; i<author.count; ++i) frame.lights[frame.count++] = author.lights[i];
  frame.hasAny = frame.count != 0;
  auto derived = std::make_shared<War3RenderSettings>(*input.settings);
  derived->shadows.pointLightsEnabled = true;
  derived->shadows.pointShadowEnabled = true;
  derived->shadows.pointShadowMaxLights = frame.shadowCount;
  derived->shadows.pointShadowTemporalReuse = false;
  derived->shadows.pointShadowUpdatePeriod = 1;
  input.nativeLightSettings = std::move(derived);
  input.nativeLightBaselines = tx.alternate; input.nativeLightCount = tx.count;
  if (tx.worldSerial % 120 == 1)
    Logger::info(str::format("NativeModelLights color lease frame=", tx.worldSerial,
      " lights=", tx.count, " draws=", tx.draws, " tailDraws=", tx.tailDraws, " author=", author.count,
      " emitterFallbacks=", input.nativeEmitterFallbackIndices.size(),
      " emitterCasters=", input.nativeEmitterCasterIndices.size()));
}
} // namespace dxvk
