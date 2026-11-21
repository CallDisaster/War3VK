// In-process provider of war3::IProcessServices. Each entry forwards to the
// device, effect or DXVK function the data layer used to call directly; the
// device-bound entries are D3D9DeviceEx itself (war3::IDeviceServices).

#define WAR3_SHADER_API_INTERNAL
#include "d3d9_device.h"
#include "d3d9_window.h"
#include "d3d9_war3_branding.h"
#include "d3d9_war3_shadow.h"
#include "d3d9_war3_volumetric_light.h"
#include "d3d9_war3_weather.h"
#include "war3_shaderpack.h"
#include "war3/core/war3_execution_services.h"

#include "../dxvk/dxvk_startup_report.h"

namespace dxvk {

namespace war3_diag {
// d3d9_device.cpp
uint64_t QueryShadowAppendTotal();
uint64_t QueryShadowAppendUnique();
uint32_t QueryShadowAppendRawcodeAt(uint32_t idx);
uint64_t QueryDevicePaletteSlotCacheServedAfterConfirmCount();
uint64_t QueryDevicePaletteSlotCacheRejectedStaleCount();
} // namespace war3_diag

namespace {

class War3InProcessServices final : public war3::IProcessServices {
public:
  constexpr War3InProcessServices() noexcept = default;

  uint64_t War3ResetCpuSemanticMapSession(
      uint64_t* outTombstoneSerial) override {
    return D3D9DeviceEx::War3ResetCpuSemanticMapSession(outTombstoneSerial);
  }

  uint64_t QueryShadowAppendTotal() override {
    return war3_diag::QueryShadowAppendTotal();
  }
  uint64_t QueryShadowAppendUnique() override {
    return war3_diag::QueryShadowAppendUnique();
  }
  uint32_t QueryShadowAppendRawcodeAt(uint32_t idx) override {
    return war3_diag::QueryShadowAppendRawcodeAt(idx);
  }
  uint64_t QueryDevicePaletteSlotCacheServedAfterConfirmCount() override {
    return war3_diag::QueryDevicePaletteSlotCacheServedAfterConfirmCount();
  }
  uint64_t QueryDevicePaletteSlotCacheRejectedStaleCount() override {
    return war3_diag::QueryDevicePaletteSlotCacheRejectedStaleCount();
  }

  bool TestFrameHistoryWindowShortcut() override {
    return ::dxvk::TestFrameHistoryWindowShortcut();
  }

  DxvkStartupReportSnapshot StartupReportSnapshot() override {
    return startup_report::snapshot();
  }

  ShadowTaaDiagnostics QueryShadowTaaDiagnostics() override {
    return ::dxvk::QueryShadowTaaDiagnostics();
  }
  CsmResolutionDiagnostics QueryCsmResolutionDiagnostics() override {
    return ::dxvk::QueryCsmResolutionDiagnostics();
  }
  PointShadowPersistentDiagnostics
  QueryPointShadowPersistentDiagnostics() override {
    return ::dxvk::QueryPointShadowPersistentDiagnostics();
  }
  ShadowReplayDiagnostics QueryShadowReplayDiagnostics() override {
    return ::dxvk::QueryShadowReplayDiagnostics();
  }
  War3VolumetricExecutionDiagnostics
  QueryWar3VolumetricExecutionDiagnostics() override {
    return ::dxvk::QueryWar3VolumetricExecutionDiagnostics();
  }
  War3VolumetricExecutionDiagnostics
  QueryWar3WeatherVolumeDiagnostics() override {
    return ::dxvk::QueryWar3WeatherVolumeDiagnostics();
  }
  War3WeatherDiagnostics QueryWar3WeatherDiagnostics() override {
    return ::dxvk::QueryWar3WeatherDiagnostics();
  }

  war3shader::ShaderPackError LoadShaderPack(const char* path) override {
    return ::war3shader::LoadShaderPack(path);
  }
  bool EnableShaderPack(bool enable) override {
    return ::war3shader::EnableShaderPack(enable);
  }
  war3shader::ShaderPackError SetParamVec4(
      war3shader::ParamSlot slot, float x, float y, float z, float w) override {
    return ::war3shader::SetParamVec4(slot, x, y, z, w);
  }

  void TryShowBrandingMessage(float gameTime) override {
    War3VKBranding::TryShowBrandingMessage(gameTime);
  }
};

War3InProcessServices g_war3InProcessServices;

// Registered while the DLL loads: the startup failure report and the
// no-device map unload both run before, or without, a device.
struct War3InProcessServicesRegistration {
  War3InProcessServicesRegistration() noexcept {
    war3::RegisterProcessServices(&g_war3InProcessServices);
  }
} g_war3InProcessServicesRegistration;

} // namespace

} // namespace dxvk
