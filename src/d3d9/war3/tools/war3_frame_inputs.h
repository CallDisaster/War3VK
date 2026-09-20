#pragma once

#include "war3_frame_evidence.h"
#include "../../../dxvk/dxvk_device.h"
#include "../../d3d9_war3_scene.h"
#include <nlohmann/json.hpp>

namespace dxvk::war3::tools::evidence {

// Optional diagnostic provider. No changes to geometry, material or scheduling
// policy. Captures resolved inputs once, outside rendering, before consumption.
bool InputsEnabled() noexcept;
std::wstring OutputDirectory(); // explicit absolute override, otherwise game-local

class InputCapture {
public:
  static std::shared_ptr<InputCapture> Create(const Rc<DxvkDevice>& device);
  ~InputCapture();
  uint64_t capture(const Rc<DxvkCommandList>& commands, Key key, bool volume,
      uint64_t renderSerial, const std::vector<const War3ShadowCasterDraw*>& draws,
      const std::vector<uint32_t>& prepared, const void* matrices, size_t matrixBytes,
      uint32_t objectBase, uint64_t sceneKey, uint64_t uploadSerial,
      DxvkResourceBufferInfo matrixInfo) noexcept;
  nlohmann::json exportNew(uint64_t session, const std::wstring& prefix, const std::atomic<bool>* stopping);
private:
  explicit InputCapture(const Rc<DxvkDevice>& device);
  struct Impl;
  std::unique_ptr<Impl> m;
};
nlohmann::json ExportInputs(uint64_t session, const std::wstring& prefix, const std::atomic<bool>* stopping);
} // namespace dxvk::war3::tools::evidence
