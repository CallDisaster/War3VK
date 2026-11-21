// war3_proxy_frame.h - the records of a frame, produced by the data layer
//
// In proxy mode the effects run in the x64 host, from records
// (effects/war3_effect_scene.h, effects/war3_shadow_scene.h). This file is
// their producer: it says where the game's world scene begins and ends, which
// camera it was drawn with, and what the author's settings ask for this
// frame. In process the device gathered the same facts inline
// (D3D9DeviceEx::War3MaybeInsertBeforeUi, War3RenderPipeline::OnFrameStart).
//
//   world RenderScene enters      MarkStage(WorldSceneEnter)
//     world draws                 the camera is sampled from VIEW / PROJECTION
//   world RenderScene returns     MarkStage(WorldSceneExit)
//   first draw after it, or       Camera, Lighting, ..., Bloom records, then
//   Present when none follows     MarkStage(BeforeUi): the host runs the effects
//   Present                       the next frame's settings are resolved
//
// Everything here runs on the game's D3D thread.
#pragma once

#include "../../d3d9_include.h"
#include "../core/war3_d3d9_observer.h"
#include "war3_proxy_casters.h"

#include <cstdint>
#include <memory>

namespace dxvk {
struct War3RenderSettingsMailbox;
}

namespace dxvk::war3::proxy {

// The settings mailbox of proxy mode: the author's edits (JAPI, shader API)
// land in it and the frame producer resolves them. One for the process,
// seeded on first use from the defaults, the player's saved settings and the
// DXVK_WAR3_* overrides, as War3RenderPipeline's constructor does.
std::shared_ptr<War3RenderSettingsMailbox> FrameSettingsMailbox();

// A device was connected / is about to be destroyed.
void FrameConnect();
void FrameDisconnect();

// The game's world RenderScene entered / returned (Hook_WorldRenderScene).
void FrameWorldScene(bool entered);
// Before a draw of the game reaches the host.
void FrameDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts);
// Before the Present of the frame reaches the host, then the frame boundary.
void FramePresent(IDirect3DDevice9* device);
// The map changed: what was sampled for the old map is dropped. Returns the
// generation for the host's session reset.
uint64_t FrameSessionReset();

// What the producer did, for the log and the control plane.
struct FrameStatus {
  uint64_t frames = 0;
  uint64_t worldScenes = 0;
  uint64_t insertions = 0;       // BeforeUi markers sent with records
  uint64_t insertionsAtPresent = 0;
  uint64_t framesWithoutCamera = 0;
  uint64_t cameraFallbacks = 0;  // the previous frame's camera was used
  uint64_t records = 0;
  uint64_t refused = 0;          // a record or marker the front end refused
  uint64_t sessionGeneration = 0;
  uint32_t lastRecordMask = 0;   // bit = schema of each record of the last insertion
  bool sendingStopped = false;   // after a refusal: nothing further is sent
  // The casters' account as of the last published frame (CastersStatus is
  // owner-thread state; this copy is what other threads read).
  CasterStatus casters;
};
FrameStatus QueryFrameStatus() noexcept;

} // namespace dxvk::war3::proxy
