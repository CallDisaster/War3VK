#pragma once
// The shadow pass of the x64 host: War3ShadowReceiverPass, the same source as
// the x86 device's (src/d3d9/d3d9_war3_shadow*.cpp), fed from the frame's
// shadow records and from the caster draws the session marked.
//
// It runs first at BeforeUi, as the device registers it before its other
// BeforeUi passes: the shadows darken the world the effects then process.
#include <d3d9.h>
#include <array>
#include <cstdint>
#include <memory>

#include "shadow_outcome.h"
#include "shadow_host_services.h"
#include "../../../src/d3d9/war3/effects/war3_shadow_scene.h"
#include "../../../src/dxvk/dxvk_buffer.h"
#include "../../../src/dxvk/dxvk_cmdlist.h"
#include "../../../src/dxvk/dxvk_device.h"
#include "../../../src/dxvk/dxvk_image.h"

namespace warvk::host::dxvkcore {

// A caster draw as the backend holds it: its own buffers and declaration.
struct ShadowCasterSource {
  dxvk::Rc<dxvk::DxvkBuffer> vertexBuffer, indexBuffer;
  const D3DVERTEXELEMENT9* elements = nullptr;
  uint32_t elementCount = 0;
  uint32_t streamOffset = 0, stride = 0;
  bool triangleList = false;
  // A dynamic buffer and its bytes as the draw read them, from its start: the
  // pass copies what the draw reads before the next upload can change it.
  bool vertexDynamic = false, indexDynamic = false;
  uint64_t readableVertexBytes = 0;
  uint32_t vertexSlot = 0, indexSlot = 0;
  uint64_t vertexGeneration = 0, indexGeneration = 0;
  const uint8_t* vertexData = nullptr;
  const uint8_t* indexData = nullptr;
  // The audit's (WARVK_D3D9_CASTER_AUDIT): the buffer's own mapping, where vertexData is the cached copy.
  const uint8_t* vertexLive = nullptr;
  // A ShadowCasterAlpha record: its threshold and TEXCOORD index, and the
  // texture the draw bound at its stage.
  bool alphaTest = false;
  float alphaRef = 0.5f;
  uint32_t texcoord = 0;
  dxvk::Rc<dxvk::DxvkImageView> alphaTexture;
  int32_t baseVertexIndex = 0;
  uint32_t minVertexIndex = 0, numVertices = 0, startIndex = 0, primitiveCount = 0;
  dxvk::war3::effects::scene::CasterIndexSpan frozenIndices;
};

class ShadowBeforeUi {
public:
  explicit ShadowBeforeUi(const dxvk::Rc<dxvk::DxvkDevice>& device, uint32_t gamePid = 0);
  ~ShadowBeforeUi();

  // The Lighting table and War3RenderSettings name the same defaults. False
  // means this build's record layout is not the settings' and nothing may run.
  static bool layoutAgrees();

  // A draw of this frame that carried a ShadowCaster record. Called before any
  // later upload is applied: a dynamic buffer still holds what the draw read.
  void addCaster(const dxvk::war3::effects::scene::ShadowCaster& record, const ShadowCasterSource& source,
      uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch);
  // Runs the pass over the frame's casters and forgets them. A changed epoch
  // pair invalidates what the pass kept, as the device does at a map change.
  ShadowOutcome run(const dxvk::Rc<dxvk::DxvkCommandList>& list,
      const dxvk::Rc<dxvk::DxvkImageView>& color, const dxvk::Rc<dxvk::DxvkImageView>& depth,
      const dxvk::war3::effects::scene::FramePacket& packet,
      const dxvk::war3::effects::scene::ShadowPacket& shadow,
      bool inPlaceWater, uint64_t& skippedWaterPasses,
      uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch, uint64_t presents,
      bool evidenceRing, shadowhost::CasterBoundsCounts& boundsCounts);
  // The frame ended without a run: its casters go with it.
  void endFrame();
  void readPresentedCounters(uint64_t present, shadowhost::CasterBoundsCounts& counts);
  // Frame evidence wants the caster audit: every caster's geometry is checked
  // as it is copied, and the counts go to the frame's outcome.
  static void setCasterAudit(bool wanted);

private:
  struct State;
  std::unique_ptr<State> m_state;
};

} // namespace warvk::host::dxvkcore
