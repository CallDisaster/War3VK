#pragma once

// In-process arm of the two-mode effect comparison.
//
// The x64 host runs the effect executors from scene records and stage markers
// that arrive over the wire. To compare it with this DLL on the same input, a
// test fixture needs to give this device the same records and markers instead
// of a running game. This private interface is that entry: it feeds the
// device's own SSAO, weather, AA and bloom at an explicit BeforeUi, and (phase
// 4c) its shadow pass from Lighting / ShadowFrame records and caster draws.
//
// It exists only when DXVK_WAR3_SCENE_EFFECTS_FIXTURE=1 was set before the
// device was asked for it; otherwise QueryInterface answers E_NOINTERFACE and
// nothing here is reachable. It is not an export and the game never sets it.
//
// DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM=1 additionally replaces the
// bloom executor with the earlier D3D9-API implementation
// (war3/render/war3_post_process.cpp) on the same record: the oracle the
// Vulkan port is measured against. Nothing else uses that implementation.

#include "d3d9_include.h"
#include "war3/effects/war3_shadow_scene.h"

#include <memory>

namespace dxvk::war3::effects {
// war3_shadow_scene_input.h
struct ShadowCasterAccount;
struct ShadowRunFacts;
class ShadowCasterPool;
}

namespace dxvk {

class D3D9DeviceEx;
class War3PostProcess;

// {5D6C0B8A-4F21-4E53-9A37-1C64E28F40B1}
inline constexpr GUID IID_WarVKSceneEffectsFixture =
    {0x5d6c0b8a, 0x4f21, 0x4e53, {0x9a, 0x37, 0x1c, 0x64, 0xe2, 0x8f, 0x40, 0xb1}};

// One run of the shadow pass: war3_shadow_scene_input.h ShadowRunFacts.
struct WarVKShadowFixtureRun {
  uint32_t casters = 0, replayDraws = 0, drawnCasters = 0, cascades = 0, cascadeDrawn[4] = { };
  uint32_t mapExecuted = 0, receiverExecuted = 0, earlyReturn = 0, inputReject = 0, replayRejected = 0;
  uint32_t completeMap = 0, alphaPrepared = 0, taaModule = 0, taaMode = 0, taaHistory = 0, taaInvalidation = 0;
  uint32_t pointShadow = 0, outline = 0;
  uint32_t volumetric = 0, volumetricComposite = 0;
  int32_t volumetricBackend = -1;
  uint64_t volumetricBytes = 0;
  uint32_t volumetricFogVolumes = 0, volumetricWeather = 0;
  float gameTime = 0.0f;
  uint64_t tombstoneSerial = 0, stagePolicyRevision = 0, resourceBytes = 0;
};

// The shadow pass's account of the fixture's frames: the counters the x64
// host prints as "D3D9 shadow-host", so a gate can compare the two.
struct WarVKShadowFixtureAccount {
  uint64_t runs = 0, mapRenders = 0, receiverDraws = 0;
  uint64_t castersUsed = 0, castersRefused = 0, refusals[4] = { };
  // Casters taken from dynamic buffers, and the bytes copied for them.
  uint64_t castersCopied = 0, copiedBytes = 0;
  uint64_t castersDrawn = 0, replayRejected = 0, peakResourceBytes = 0;
  WarVKShadowFixtureRun last;
};

// Same contract as the x64 proxy's WarVKD3D9SetFrameScene / WarVKD3D9MarkStage
// / WarVKD3D9RequestSessionReset exports: war3_effect_scene.h and
// war3_shadow_scene.h records, and the same refusals.
//
// The x64 host takes a ShadowFrame record's facts as they come. Here they go
// into this process's data layer, whose two counters only count up and whose
// module switches are fixed when the process starts. So a BeforeUi marker is
// refused, and nothing runs, when the frame's record names a tombstone serial
// or a policy revision behind the data layer's or more than 64 events ahead
// of it, or a shadow module switch (ShadowPassModules) this process does not
// have.
struct IWarVKSceneEffectsFixture : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetFrameScene(UINT schema, const void* data, UINT bytes) = 0;
  virtual HRESULT STDMETHODCALLTYPE MarkStage(UINT stage) = 0;
  // WarVKD3D9AnnotateNextDraw followed by DrawIndexedPrimitive, as one call:
  // this device has no place where a record waits for the next draw. Only a
  // ShadowCaster record; a record the proxy would refuse is refused here and
  // nothing is drawn.
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedAnnotated(UINT schema, const void* data, UINT bytes,
      D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
      UINT startIndex, UINT primitiveCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShadowAccount(WarVKShadowFixtureAccount* account) = 0;
  // The map changed (reason 1) or the game ended (2). As the game's own map
  // unload does, this asks the device to end its shadow session; the device
  // does so at its next Present, where the proxy sends its request. Refused
  // inside a world scene: the proxy would hold the request until the Present.
  virtual HRESULT STDMETHODCALLTYPE RequestSessionReset(UINT reason, UINT generationLow, UINT generationHigh) = 0;
  // WarVKD3D9AnnotateNextDraw of a ShadowCasterAlpha record: it goes with the
  // next DrawIndexedAnnotated, whose ShadowCaster record the proxy sends
  // before it. Any other record, or a second one, is refused.
  virtual HRESULT STDMETHODCALLTYPE AnnotateNextDraw(UINT schema, const void* data, UINT bytes) = 0;
};

class D3D9War3SceneEffectsFixture final : public IWarVKSceneEffectsFixture {
public:
  explicit D3D9War3SceneEffectsFixture(D3D9DeviceEx* device);
  ~D3D9War3SceneEffectsFixture();

  static bool Enabled();
  static bool LegacyBloom();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
  HRESULT STDMETHODCALLTYPE SetFrameScene(UINT schema, const void* data, UINT bytes) override;
  HRESULT STDMETHODCALLTYPE MarkStage(UINT stage) override;
  HRESULT STDMETHODCALLTYPE DrawIndexedAnnotated(UINT schema, const void* data, UINT bytes,
      D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
      UINT startIndex, UINT primitiveCount) override;
  HRESULT STDMETHODCALLTYPE GetShadowAccount(WarVKShadowFixtureAccount* account) override;
  HRESULT STDMETHODCALLTYPE RequestSessionReset(UINT reason, UINT generationLow, UINT generationHigh) override;
  HRESULT STDMETHODCALLTYPE AnnotateNextDraw(UINT schema, const void* data, UINT bytes) override;

private:
  // The frame's casters and what became of their records (.cpp).
  struct ShadowArm;

  HRESULT runLegacyBloom();
  void clearScene();

  D3D9DeviceEx* m_device;
  std::unique_ptr<War3PostProcess> m_legacyBloom;
  war3::effects::scene::FramePacket m_packet;
  war3::effects::scene::ShadowPacket m_shadowPacket;
  std::unique_ptr<ShadowArm> m_shadow;
  WarVKShadowFixtureAccount m_shadowAccount;
  war3::effects::scene::SceneTracker m_tracker;
  uint64_t m_frames = 0;
  // The companion record of the next caster draw.
  bool m_hasNextAlpha = false;
  war3::effects::scene::ShadowCasterAlpha m_nextAlpha;
};

} // namespace dxvk
