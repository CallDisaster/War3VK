// war3_execution_services.h - what the data layer may ask of the execution end
//
// The data layer (Game.dll hooks, JASS natives, registries, diagnostics and
// test tools) stays in the 32-bit game process. The execution end (the D3D9
// device, the effects, DXVK) runs in the same process today and in the x64
// host in proxy mode. Everything the data layer needs from it that is not a
// plain IDirect3DDevice9 call goes through the two interfaces below, so no
// data-layer object links against a device, effect or DXVK symbol.
//
//   IDeviceServices   entries of the published device. In process this is
//                     D3D9DeviceEx itself; in proxy mode the proxy front end
//                     (ordered messages to the host, or "unavailable").
//   IProcessServices  entries that exist without a device: process-wide
//                     diagnostics and the no-device CPU reset. In process they
//                     are registered at load by d3d9_war3_execution_services.cpp.
//
// Callers must hold these interface types, never D3D9DeviceEx: that class is
// final, so a call through it is devirtualized into a direct reference to
// d3d9_device.cpp. See docs/plan/2026-11-15-x64-phase2-device-interface.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dxvk {

// Defined by the execution end; a caller includes the owning header to use
// the value. Value-initialized means "unavailable".
struct War3ShadowLifecycleDiagnostics;     // d3d9_device.h
struct ShadowTaaDiagnostics;               // d3d9_war3_shadow.h
struct CsmResolutionDiagnostics;           // d3d9_war3_shadow.h
struct PointShadowPersistentDiagnostics;   // d3d9_war3_shadow.h
struct ShadowReplayDiagnostics;            // d3d9_war3_shadow.h
struct War3VolumetricExecutionDiagnostics; // d3d9_war3_volumetric_light.h
struct War3WeatherDiagnostics;             // d3d9_war3_weather.h
struct DxvkStartupReportSnapshot;          // dxvk_startup_report.h

namespace war3::reimpl {
class War3InstanceBuffer;                  // war3/reimpl/war3_instance_buffer.h
} // namespace war3::reimpl

} // namespace dxvk

struct IDirect3DVertexShader9;             // d3d9.h
struct IDirect3DPixelShader9;              // d3d9.h

namespace war3shader {

enum class ShaderPackError : uint32_t;     // war3_shaderpack.h
enum class ParamSlot : uint32_t;           // war3_shaderpack.h

} // namespace war3shader

namespace dxvk::war3 {

// Entries of the published device. Reached only inside
// RunWithActiveDeviceServices (war3.h), which holds the publication lock for
// the whole transaction. Every member is pure: an implementation has to
// decide each one, and a signature that drifts stops compiling.
class IDeviceServices {
public:
  // World RenderScene boundary, on the D3D thread. Proxy mode: the
  // WorldSceneEnter / WorldSceneExit stage markers.
  virtual void War3SetWorldLightingScope(bool inWorld) = 0;
  // Map change. Applied by the device at its next Present. Proxy mode: the
  // session reset request.
  virtual void War3RequestShadowMapEpochReset() = 0;
  // The native GPU-skin hook transaction is installed.
  virtual void War3AttachGpuSkinNativeBridge(uintptr_t gameBase) = 0;

  // Validation host helpers.
  virtual uint32_t War3PopulateSemanticShadowSceneForValidation(
      bool unitsOnly, bool executeNativeBackendValidation) = 0;
  virtual bool War3ExecuteSemanticShadowSceneForValidation(
      bool unitsOnly, bool executeNativeBackendValidation) = 0;

  // Diagnostics and internal test entries.
  virtual War3ShadowLifecycleDiagnostics
  QueryWar3ShadowLifecycleDiagnostics() const = 0;
  // VkResult of the DXVK device as an integer.
  virtual int64_t QueryWar3DeviceStatus() const = 0;
  virtual bool War3ResetGpuSkinBridgeForTest(bool deviceEpoch) = 0;
  virtual bool War3LogGpuSkinDiagnosticsForTest(
      bool requireQuiescent, bool* outQuiescent) = 0;

  // The device's instancing ring (created on first use, per device).
  virtual reimpl::War3InstanceBuffer* GetWar3InstanceBuffer() = 0;

  // Material shaders (war3/shader/war3_material.cpp): DXSO tokens the data
  // layer compiled. The execution end checks the module, creates the shader
  // object and, on failure, sets *error to the message the material reports.
  virtual bool War3CreateMaterialVertexShader(const uint32_t* tokens,
      size_t bytes, const std::string& path, IDirect3DVertexShader9** shader,
      std::string* error) = 0;
  virtual bool War3CreateMaterialPixelShader(const uint32_t* tokens,
      size_t bytes, const std::string& path, IDirect3DPixelShader9** shader,
      std::string* error) = 0;

protected:
  ~IDeviceServices() = default;
};

// Entries that need no device. Names match the functions they stand for.
class IProcessServices {
public:
  // Process-global CPU semantic reset for a map unload with no device
  // published. Returns the new map epoch; 0 when unavailable.
  virtual uint64_t War3ResetCpuSemanticMapSession(
      uint64_t* outTombstoneSerial = nullptr) = 0;

  // Cumulative device-side counters.
  virtual uint64_t QueryShadowAppendTotal() = 0;
  virtual uint64_t QueryShadowAppendUnique() = 0;
  virtual uint32_t QueryShadowAppendRawcodeAt(uint32_t idx) = 0;
  virtual uint64_t QueryDevicePaletteSlotCacheServedAfterConfirmCount() = 0;
  virtual uint64_t QueryDevicePaletteSlotCacheRejectedStaleCount() = 0;

  // Internal-test route through the installed window procedure.
  virtual bool TestFrameHistoryWindowShortcut() = 0;

  // Adapter and device-creation record of the last startup attempt.
  virtual DxvkStartupReportSnapshot StartupReportSnapshot() = 0;

  // Effect diagnostics.
  virtual ShadowTaaDiagnostics QueryShadowTaaDiagnostics() = 0;
  virtual CsmResolutionDiagnostics QueryCsmResolutionDiagnostics() = 0;
  virtual PointShadowPersistentDiagnostics
  QueryPointShadowPersistentDiagnostics() = 0;
  virtual ShadowReplayDiagnostics QueryShadowReplayDiagnostics() = 0;
  virtual War3VolumetricExecutionDiagnostics
  QueryWar3VolumetricExecutionDiagnostics() = 0;
  virtual War3VolumetricExecutionDiagnostics
  QueryWar3WeatherVolumeDiagnostics() = 0;
  virtual War3WeatherDiagnostics QueryWar3WeatherDiagnostics() = 0;

  // Shader pack commands of the plugin SDK (war3_shaderpack.h). Unavailable
  // answers INTERNAL_ERROR and false: a value-initialized ShaderPackError
  // would read as OK.
  virtual war3shader::ShaderPackError LoadShaderPack(const char* path) = 0;
  virtual bool EnableShaderPack(bool enable) = 0;
  virtual war3shader::ShaderPackError SetParamVec4(
      war3shader::ParamSlot slot, float x, float y, float z, float w) = 0;

  // The one-time branding notice (d3d9_war3_branding.h), from the render hook.
  virtual void TryShowBrandingMessage(float gameTime) = 0;

protected:
  ~IProcessServices() = default;
};

// The registered provider; before any registration, one that answers
// "unavailable" to everything. Never null, callable from any thread.
IProcessServices& ProcessServices() noexcept;

// Replaces the provider. It must live for the rest of the process. nullptr
// restores the "unavailable" provider.
void RegisterProcessServices(IProcessServices* services) noexcept;

} // namespace dxvk::war3
