#include "d3d9_war3_scene_effects_fixture.h"
#include "d3d9_device.h"
#include "war3/effects/war3_shadow_scene_input.h"
#include "war3/render/war3_post_process.h"

#include "../util/util_env.h"

namespace dxvk {

namespace scene = war3::effects::scene;

namespace {

// How far one frame's record may move a counter of the data layer: every step
// is an event published to it.
constexpr uint64_t kMaxFactSteps = 64u;
// The object the fixture's tombstones name: no caster of any frame.
constexpr uint32_t kFactTombstoneHandle = 0x7fffff01u;

bool FactReachable(uint64_t current, uint64_t wanted) {
  return wanted >= current && wanted - current <= kMaxFactSteps;
}

// Whether this process's data layer can be made to say what the record says.
bool ShadowFactsReachable(const scene::ShadowFrame& frame) {
  const uint32_t modules = war3::runtime::GetWar3RuntimeConfig().effectiveDisabledMask;
  return FactReachable(war3::render::CurrentShadowCasterTombstoneSerial(), frame.tombstoneSerial)
      && FactReachable(war3::render::CurrentShadowStagePolicyRevision(), frame.stagePolicyRevision)
      && ((frame.runtimeDisabledModules ^ modules) & war3::effects::ShadowPassModules) == 0u;
}

// The record's facts, to where the pass reads them (war3_shadow_scene_input.h):
// the game time as the game's hook sets it, the counters by the events that
// move them in the game.
void ApplyShadowFacts(const scene::ShadowFrame& frame) {
  War3RenderState::SetGameTime(frame.timeOfDay);
  while (war3::render::CurrentShadowCasterTombstoneSerial() < frame.tombstoneSerial) {
    war3::render::ShadowCasterTombstone tombstone;
    tombstone.identity.jHandle = kFactTombstoneHandle;
    tombstone.reason = war3::render::ShadowCasterTombstoneReason::Removed;
    if (!war3::render::PublishShadowCasterTombstone(tombstone))
      break;
  }
  while (war3::render::CurrentShadowStagePolicyRevision() < frame.stagePolicyRevision)
    war3::render::AdvanceShadowStagePolicyRevision();
}

} // namespace

struct D3D9War3SceneEffectsFixture::ShadowArm {
  War3FrameScene scene;
  war3::effects::ShadowCasterAccount account;
  // What the frame's casters read from dynamic buffers; created by the first
  // caster of the frame.
  std::unique_ptr<war3::effects::ShadowCasterPool> pool;
};

D3D9War3SceneEffectsFixture::D3D9War3SceneEffectsFixture(D3D9DeviceEx* device)
: m_device(device), m_shadow(std::make_unique<ShadowArm>()) { }

D3D9War3SceneEffectsFixture::~D3D9War3SceneEffectsFixture() = default;

bool D3D9War3SceneEffectsFixture::Enabled() {
  static const bool enabled = env::getEnvVar("DXVK_WAR3_SCENE_EFFECTS_FIXTURE") == "1";
  return enabled;
}

bool D3D9War3SceneEffectsFixture::LegacyBloom() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM") == "1";
  return enabled;
}

HRESULT D3D9War3SceneEffectsFixture::runLegacyBloom() {
  // The settings the record came from (War3BloomPass::Record): intensity 0 is
  // bloom off, which the D3D9 implementation also composited.
  War3PostFxSettings settings;
  settings.enabled = true;
  settings.bloom.enabled = true;
  settings.bloom.threshold = m_packet.bloom.threshold;
  settings.bloom.softKnee = m_packet.bloom.softKnee;
  settings.bloom.intensity = m_packet.bloom.intensity;
  settings.bloom.acesToneMap = m_packet.bloom.acesToneMap != 0;
  settings.exposure = m_packet.bloom.exposure;
  settings.useSrgb = m_packet.bloom.useSrgb != 0;
  Com<IDirect3DSurface9> target;
  if (FAILED(m_device->GetRenderTarget(0, &target)) || target == nullptr)
    return D3DERR_INVALIDCALL;
  if (!m_legacyBloom)
    m_legacyBloom = std::make_unique<War3PostProcess>(m_device);
  return m_legacyBloom->ApplyFromSurface(target.ptr(), target.ptr(), settings)
      ? D3D_OK : D3DERR_INVALIDCALL;
}

ULONG STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::AddRef() {
  return m_device->AddRef();
}

ULONG STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::Release() {
  return m_device->Release();
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::QueryInterface(REFIID riid, void** ppvObject) {
  return m_device->QueryInterface(riid, ppvObject);
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::SetFrameScene(UINT schema, const void* data, UINT bytes) {
  const auto* payload = static_cast<const uint8_t*>(data);
  // The transport's acceptance rule: any finite camera, or a valid effect record.
  if (schema == scene::CameraSchema) {
    if (!payload || bytes != scene::CameraBytes)
      return D3DERR_INVALIDCALL;
    for (uint32_t word = 0; word < scene::CameraBytes / 4; ++word)
      if (word != 38 && !std::isfinite(scene::detail::GetFloat(payload + 4 * word)))
        return D3DERR_INVALIDCALL;
  } else if (schema == scene::LightingSchema || schema == scene::ShadowFrameSchema
      || schema == scene::PointLightsSchema || schema == scene::OutlineSchema
      || schema == scene::VolumetricLightSchema || schema == scene::FogVolumesSchema) {
    if (!scene::ValidShadowRecord(schema, payload, bytes))
      return D3DERR_INVALIDCALL;
  } else if (!scene::ValidEffectRecord(schema, payload, bytes)) {
    return D3DERR_INVALIDCALL;
  }
  D3D9DeviceLock lock = m_device->LockDevice();
  if (!m_shadowPacket.accept(schema, payload, bytes))
    m_packet.accept(schema, payload, bytes);
  return D3D_OK;
}

void D3D9War3SceneEffectsFixture::clearScene() {
  m_packet.clear();
  m_shadowPacket.clear();
  *m_shadow = ShadowArm();
  m_hasNextAlpha = false;
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::AnnotateNextDraw(UINT schema, const void* data, UINT bytes) {
  scene::ShadowCasterAlpha alpha;
  if (schema != scene::ShadowCasterAlphaSchema ||
      !scene::DecodeShadowCasterAlpha(static_cast<const uint8_t*>(data), bytes, alpha))
    return D3DERR_INVALIDCALL;
  D3D9DeviceLock lock = m_device->LockDevice();
  if (m_hasNextAlpha)
    return D3DERR_INVALIDCALL;
  m_nextAlpha = alpha;
  m_hasNextAlpha = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::DrawIndexedAnnotated(UINT schema, const void* data, UINT bytes,
    D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
    UINT startIndex, UINT primitiveCount) {
  D3D9DeviceLock lock = m_device->LockDevice();
  // The companion record goes with this draw, as the proxy's would; a refused
  // draw takes it along.
  const bool alpha = m_hasNextAlpha;
  m_hasNextAlpha = false;
  scene::ShadowCaster record;
  if (schema != scene::ShadowCasterSchema ||
      !scene::DecodeShadowCaster(static_cast<const uint8_t*>(data), bytes, record))
    return D3DERR_INVALIDCALL;
  if (m_shadow->pool == nullptr)
    m_shadow->pool = std::make_unique<war3::effects::ShadowCasterPool>(m_device->GetDXVKDevice());
  return m_device->War3DrawShadowCasterForFixture(record, alpha ? &m_nextAlpha : nullptr, type, baseVertexIndex,
      minVertexIndex, numVertices, startIndex, primitiveCount, m_frames + 1, m_shadow->scene, *m_shadow->pool,
      m_shadow->account);
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::GetShadowAccount(WarVKShadowFixtureAccount* account) {
  if (!account)
    return D3DERR_INVALIDCALL;
  D3D9DeviceLock lock = m_device->LockDevice();
  *account = m_shadowAccount;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::RequestSessionReset(UINT reason, UINT generationLow,
    UINT generationHigh) {
  const uint64_t generation = uint64_t(generationLow) | (uint64_t(generationHigh) << 32);
  if ((reason != 1u && reason != 2u) || !generation)
    return D3DERR_INVALIDCALL;
  D3D9DeviceLock lock = m_device->LockDevice();
  // Generations only ascend, and a session never changes inside a world scene.
  if (!m_tracker.reset(generation))
    return D3DERR_INVALIDCALL;
  clearScene();
  // What the game's map unload calls (war3.cpp). The device ends the shadow
  // session at its next Present and hands the receiver the new epoch there.
  m_device->War3RequestShadowMapEpochReset();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9War3SceneEffectsFixture::MarkStage(UINT stage) {
  if (!scene::ValidStage(stage))
    return D3DERR_INVALIDCALL;
  D3D9DeviceLock lock = m_device->LockDevice();
  // Effects go between the completed world scene and whatever follows it,
  // once. Anywhere else the marker is refused and nothing runs.
  const bool insert = stage == uint32_t(scene::Stage::BeforeUi) &&
      (m_packet.effects() || m_shadowPacket.work());
  if (insert && !m_tracker.canInsertBeforeUi())
    return D3DERR_INVALIDCALL;
  // The frame's facts go to the data layer before the pass asks it, when the
  // pass will run at all; a record this process cannot follow stops the frame.
  const bool facts = insert && m_shadowPacket.work() && m_shadowPacket.hasFrame;
  if (facts && !ShadowFactsReachable(m_shadowPacket.frame))
    return D3DERR_INVALIDCALL;
  if (!m_tracker.mark(stage, 0))
    return D3DERR_INVALIDCALL;
  if (insert) {
    if (facts)
      ApplyShadowFacts(m_shadowPacket.frame);
    // The frame's point lights, into the data layer's light manager, the
    // objects its unit outline draws, into the data layer's outline handles,
    // and the map's fog volumes, into the data layer's fog volume manager.
    if (m_shadowPacket.work()) {
      war3::effects::ApplyPointLights(m_shadowPacket);
      war3::effects::ApplyOutlineHandles(m_shadowPacket);
      war3::effects::ApplyFogVolumes(m_shadowPacket);
    }
    const bool legacy = LegacyBloom() && m_packet.hasBloom;
    // The host's account of the frame's casters, taken before the scene moves
    // into the pass.
    auto& account = m_shadowAccount;
    const auto casters = m_shadow->account;
    war3::effects::ShadowRunFacts run;
    bool shadowRan = false;
    HRESULT hr = m_device->War3RunSceneEffectsForFixture(m_packet, m_frames + 1, !legacy,
        &m_shadowPacket, &m_shadow->scene, &m_shadow->account, &run, &shadowRan);
    if (SUCCEEDED(hr) && legacy)
      hr = runLegacyBloom();
    if (FAILED(hr))
      return hr;
    account.castersRefused += casters.refused;
    account.castersUsed += casters.received - casters.refused;
    for (uint32_t i = 0; i < 4; ++i)
      account.refusals[i] += casters.refusals[i];
    account.castersCopied += casters.copied;
    account.copiedBytes += casters.copiedBytes;
    if (shadowRan) {
      ++account.runs;
      account.mapRenders += run.mapExecuted;
      account.receiverDraws += run.receiverExecuted;
      account.castersDrawn += run.drawnCasters;
      account.replayRejected += run.replayRejected;
      account.peakResourceBytes = std::max(account.peakResourceBytes, run.resourceBytes);
      auto& last = account.last;
      last.casters = run.casters; last.replayDraws = run.replayDraws;
      last.drawnCasters = run.drawnCasters; last.cascades = run.cascades;
      std::copy(run.cascadeDrawn.begin(), run.cascadeDrawn.end(), last.cascadeDrawn);
      last.mapExecuted = run.mapExecuted; last.receiverExecuted = run.receiverExecuted;
      last.earlyReturn = run.earlyReturn; last.inputReject = run.inputReject;
      last.replayRejected = run.replayRejected;
      last.completeMap = run.completeMap; last.taaModule = run.taaModule; last.taaMode = run.taaMode;
      last.alphaPrepared = run.alphaPrepared;
      last.taaHistory = run.taaHistory; last.taaInvalidation = run.taaInvalidation;
      last.pointShadow = run.pointShadow; last.outline = run.outline;
      last.volumetric = run.volumetric; last.volumetricComposite = run.volumetricComposite;
      last.volumetricBackend = run.volumetricBackend; last.volumetricBytes = run.volumetricBytes;
      last.volumetricFogVolumes = run.volumetricFogVolumes; last.volumetricWeather = run.volumetricWeather;
      last.gameTime = run.gameTime; last.tombstoneSerial = run.tombstoneSerial;
      last.stagePolicyRevision = run.stagePolicyRevision; last.resourceBytes = run.resourceBytes;
    }
    *m_shadow = ShadowArm();
    ++m_frames;
    m_tracker.insertedBeforeUi();
  }
  if (stage == uint32_t(scene::Stage::FrameEnd))
    clearScene();
  return D3D_OK;
}

} // namespace dxvk
