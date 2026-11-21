#pragma once

#include "d3d9_backend.h"
#include "../../../src/d3d9/d3d9_war3_settings.h"
#include "../../../src/d3d9/war3/platform/war3_proxy_diag_block.h"
#include "../../../src/dxvk/dxvk_context.h"
#include "../../../src/dxvk/dxvk_device.h"
#include <chrono>

namespace warvk::host::dxvkcore {

class WaterInPlace {
  using Reason = D3D9Backend::WaterFallback;
  using Stats = D3D9Backend::WaterStats;
  template<class T> using Rc = dxvk::Rc<T>;
public:
  void beginScene(const Rc<dxvk::DxvkDevice>& device, DWORD gamePid, uint64_t completed, Stats& stats);
  void endFrame() { m_enabled = false; m_drewThisFrame = false; }
  bool enabled() const { return m_enabled; }
  bool referenceStyle() const { return m_settings.style == 0.0f; }
  bool drewThisFrame() const { return m_drewThisFrame; }
  void resetTargets(const Rc<dxvk::DxvkContext>& ctx);
  // A refusal leaves the original geometry pending.
  Reason bind(const Rc<dxvk::DxvkDevice>& device, const Rc<dxvk::DxvkContext>& ctx,
      const Rc<dxvk::DxvkImage>& color, const Rc<dxvk::DxvkImage>& depth,
      const d9::FixedFunctionState* fixed, const D3DVIEWPORT9& vp, const dxvk::Matrix4& stockInverseView,
      const Rc<dxvk::DxvkShader>& stockVS, const Rc<dxvk::DxvkShader>& stockFS, Stats& stats);
  void finishDraw(Reason reason, Stats& stats);
  void beginTiming(const Rc<dxvk::DxvkDevice>& device, const Rc<dxvk::DxvkCommandList>& list, uint64_t submission);
  void endTiming(const Rc<dxvk::DxvkCommandList>& list);
  void collectTiming(uint64_t completed, Stats& stats);

private:
  struct Uniform {
    dxvk::Matrix4 world, invViewProj, view, viewProj;
    dxvk::Vector4 viewport, depthRange, controls;
    float time = 0, waveScale = 0, forwardX = 0, forwardY = 0;
    dxvk::Vector4 camera, toSun, sunColor, ambient, lightControls;
  };
  static_assert(offsetof(Uniform, world) == 0 && offsetof(Uniform, invViewProj) == 64);
  static_assert(offsetof(Uniform, view) == 128 && offsetof(Uniform, viewProj) == 192);
  static_assert(offsetof(Uniform, viewport) == 256 && offsetof(Uniform, depthRange) == 272);
  static_assert(offsetof(Uniform, controls) == 288 && offsetof(Uniform, time) == 304);
  static_assert(offsetof(Uniform, waveScale) == 308 && offsetof(Uniform, forwardX) == 312 && offsetof(Uniform, forwardY) == 316);
  static_assert(offsetof(Uniform, camera) == 320 && offsetof(Uniform, toSun) == 336);
  static_assert(offsetof(Uniform, sunColor) == 352 && offsetof(Uniform, ambient) == 368);
  static_assert(offsetof(Uniform, lightControls) == 384 && sizeof(Uniform) == 400);
  Reason snapshot(const Rc<dxvk::DxvkDevice>& device, const Rc<dxvk::DxvkContext>& ctx,
                  const Rc<dxvk::DxvkImage>& color, const Rc<dxvk::DxvkImage>& depth);
  bool fillUniform(Uniform& data, const d9::FixedFunctionState& fixed, const D3DVIEWPORT9& vp,
                   const dxvk::Matrix4& stockInverseView) const;
  void bindUniform(const Rc<dxvk::DxvkDevice>& device, const Rc<dxvk::DxvkContext>& ctx, const Uniform& data);
  warvk::diag::CasterSettingsView m_settingsView;
  dxvk::War3WaterSettings m_settings;
  bool m_enabled = false, m_first = true, m_drewThisFrame = false;
  Reason m_snapshotResult = Reason::Snapshot;
  Rc<dxvk::DxvkImageView> m_color, m_depth;
  Rc<dxvk::DxvkShader> m_vs, m_fs;
  Rc<dxvk::DxvkBuffer> m_uniform;
  Uniform m_lastUniform;
  std::chrono::steady_clock::time_point m_tick = std::chrono::steady_clock::now();
  double m_waveTime = 0;
  // One bounded, periodically sampled world scene. Only completed submissions are read.
  static constexpr uint32_t MaxTimedDraws = 64;
  struct Timestamps : dxvk::RcObject {
    explicit Timestamps(const Rc<dxvk::DxvkDevice>& device);
    ~Timestamps();
    Rc<dxvk::vk::DeviceFn> vk;
    VkQueryPool pool = VK_NULL_HANDLE;
  };
  Rc<Timestamps> m_timestamps;
  uint32_t m_stampCount = 0;
  uint64_t m_waterScenes = 0, m_submission = 0, m_tickMask = 0;
  double m_periodUs = 0;
  bool m_clockChecked = false, m_sampling = false, m_partial = false;
};

}
