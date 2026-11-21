// war3_proxy_casters.h - the shadow casters of a frame, from the game's draws
//
// In proxy mode the shadow pass runs in the x64 host. A caster is a draw of
// the game with a ShadowCaster record bound to it (effects/war3_shadow_scene.h):
// the host takes the geometry from the draw's own buffers and everything else
// from the record. This file decides, at each world draw, whether the draw
// casts a shadow, and writes its record.
//
// The rules are those of the in-process capture
// (D3D9DeviceEx::War3TryCaptureShadowCaster): which stages and batches cast,
// which render states make a draw an overlay, what a path blocker is. Two
// things differ, both because the geometry stays in the host:
//
//   - Units. In process they come from the semantic scene, which reads the
//     game's models; here they are the game's own stage draws like everything
//     else, by the rules the capture applied to them before the semantic
//     scene took them over.
//   - What the host cannot replay is not sent: anything but an indexed
//     triangle list of three-float positions in stream 0, fixed-function
//     vertex blending, an alpha-tested draw whose texture or coordinates the
//     host could not read. The executor refuses such a caster, and a refused
//     caster makes the frame's set incomplete - the pass then keeps the
//     previous shadow map. A draw that is not sent casts no shadow instead.
//
// Everything here runs on the game's D3D thread.
#pragma once

#include "../../d3d9_include.h"
#include "../core/war3_d3d9_observer.h"

#include <array>
#include <cstdint>

namespace dxvk {
struct War3RenderSettings;
}

namespace dxvk::war3::proxy {

// Why a world draw did not become a caster.
enum class CasterSkip : uint32_t {
  Policy = 0,        // a stage or batch that never casts (S12 overlay, S20 lightning)
  PathBlocker,       // a pathing / line-of-sight blocker
  NotCaster,         // neither terrain nor an object
  NoDepth,           // no depth test
  VertexShader,
  Overlay,           // writes no depth and is not alpha-tested
  Additive,
  BlendNoAlphaTest,  // blended without alpha test: never an invented cutout
  NotReplayable,     // not an indexed triangle list of float3 positions in stream 0
  VertexBlend,       // fixed-function vertex blending
  AlphaPayload,      // alpha-tested, without a texture or stream-0 float2 coordinates
  Marker,            // a small anonymous marker below the ground
  Budget,            // the caster record count cap is used up
  Count,
};

// The casters of one frame (up to its BeforeUi insertion), by class.
struct CasterFrameCounts {
  uint32_t considered = 0;
  uint32_t sent = 0;
  uint32_t terrain = 0, doodads = 0, worldObjects = 0, units = 0;
  // Of the sent casters: with a non-zero batch handle (what the outline
  // matches), units only, and those whose handle is in the outline set.
  uint32_t withHandle = 0, unitsWithHandle = 0, outlineMatched = 0;
  uint32_t demandCasters = 0, admittedBytes = 0;
  uint64_t demandBytes = 0; // full declared demand, in pool chunks
};

struct CasterStatus {
  uint64_t considered = 0;   // world draws that passed the stage gates
  uint64_t sent = 0;         // ShadowCaster records
  uint64_t alpha = 0;        // of them with a ShadowCasterAlpha record
  uint64_t terrain = 0, doodads = 0, worldObjects = 0, units = 0; // sent, by class
  uint64_t water = 0;        // of the sent records: the game's water (casts nothing)
  uint64_t dynamicBytes = 0; // original dynamic stream bytes of the sent draws
  uint64_t omittedFrames = 0;
  std::array<uint64_t, uint32_t(CasterSkip::Count)> skipped{};
  // Of the sent casters, cumulative: with a non-zero batch handle, units with
  // one, and those whose handle the outline set holds (the host's outline
  // pass matches exactly these).
  uint64_t withHandle = 0, unitsWithHandle = 0, outlineMatched = 0;
  // The last frame that closed with a ShadowFrame record.
  CasterFrameCounts lastFrame;
};

// The frame begins: whether the frame's settings ask for casters at all
// (War3RenderPipeline::WantsShadowCapture and the capture's settings gate).
void CastersBeginFrame(const War3RenderSettings& settings);
// Whether this frame's world draws are looked at.
bool CastersWanted();

// A world draw, before it is sent. S_OK: its records were written. S_FALSE:
// not a caster. A failure is the front end's refusal of a record.
HRESULT CastersDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts);

// The frame's account for its ShadowFrame record: the records sent, and the
// casters left out by the record count cap.
struct CasterFrameAccount {
  uint32_t casters = 0;
  uint32_t omitted = 0;
  uint32_t omissionReasons = 0;
  uint32_t dynamicCasters = 0;
  uint64_t dynamicSignature = 0;
};
CasterFrameAccount CastersFrameAccount();
CasterFrameCounts CastersCurrentCounts();

// The frame ended, or the session was reset.
void CastersEndFrame();

const CasterStatus& CastersStatus();

} // namespace dxvk::war3::proxy
