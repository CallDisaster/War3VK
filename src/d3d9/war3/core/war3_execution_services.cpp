// war3_execution_services.cpp - registry of the process-wide service provider

#include "war3_execution_services.h"

#include "../../d3d9_war3_shadow.h"
#include "../../d3d9_war3_volumetric_light.h"
#include "../../d3d9_war3_weather.h"
#include "../../war3_shaderpack.h"
#include "../../../dxvk/dxvk_startup_report.h"

#include <atomic>

namespace dxvk::war3 {

namespace {

// What the data layer gets when no execution end has registered: zeros and
// value-initialized records. It only reads types from the headers above.
class UnavailableProcessServices final : public IProcessServices {
public:
  constexpr UnavailableProcessServices() noexcept = default;

  uint64_t War3ResetCpuSemanticMapSession(
      uint64_t* outTombstoneSerial) override {
    if (outTombstoneSerial != nullptr)
      *outTombstoneSerial = 0u;
    return 0u;
  }

  uint64_t QueryShadowAppendTotal() override { return 0u; }
  uint64_t QueryShadowAppendUnique() override { return 0u; }
  uint32_t QueryShadowAppendRawcodeAt(uint32_t) override { return 0u; }
  uint64_t QueryDevicePaletteSlotCacheServedAfterConfirmCount() override {
    return 0u;
  }
  uint64_t QueryDevicePaletteSlotCacheRejectedStaleCount() override {
    return 0u;
  }

  bool TestFrameHistoryWindowShortcut() override { return false; }

  DxvkStartupReportSnapshot StartupReportSnapshot() override { return {}; }

  ShadowTaaDiagnostics QueryShadowTaaDiagnostics() override { return {}; }
  CsmResolutionDiagnostics QueryCsmResolutionDiagnostics() override {
    return {};
  }
  PointShadowPersistentDiagnostics
  QueryPointShadowPersistentDiagnostics() override {
    return {};
  }
  ShadowReplayDiagnostics QueryShadowReplayDiagnostics() override {
    return {};
  }
  War3VolumetricExecutionDiagnostics
  QueryWar3VolumetricExecutionDiagnostics() override {
    return {};
  }
  War3VolumetricExecutionDiagnostics
  QueryWar3WeatherVolumeDiagnostics() override {
    return {};
  }
  War3WeatherDiagnostics QueryWar3WeatherDiagnostics() override { return {}; }

  war3shader::ShaderPackError LoadShaderPack(const char*) override {
    return war3shader::ShaderPackError::INTERNAL_ERROR;
  }
  bool EnableShaderPack(bool) override { return false; }
  war3shader::ShaderPackError SetParamVec4(
      war3shader::ParamSlot, float, float, float, float) override {
    return war3shader::ShaderPackError::INTERNAL_ERROR;
  }

  void TryShowBrandingMessage(float) override { }
};

// Both are constant-initialized, so a provider that registers from a static
// initializer of another translation unit is never overwritten afterwards.
UnavailableProcessServices s_unavailable;
std::atomic<IProcessServices*> s_services{ &s_unavailable };

} // namespace

IProcessServices& ProcessServices() noexcept {
  return *s_services.load(std::memory_order_acquire);
}

void RegisterProcessServices(IProcessServices* services) noexcept {
  s_services.store(services != nullptr ? services : &s_unavailable,
                   std::memory_order_release);
}

} // namespace dxvk::war3
