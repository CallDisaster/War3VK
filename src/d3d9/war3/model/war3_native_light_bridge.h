#pragma once
#include "war3_native_light_core.h"
#include "../../d3d9_include.h"

namespace dxvk::war3::native_light {
bool Install(uintptr_t gameBase);
bool Enabled() noexcept;
void CloneInstance(void* instance, void* source, void* result) noexcept;
void BeginWorld() noexcept;
void EndWorld() noexcept;
void ResetMap() noexcept;
struct Frame {
  std::array<Sample, 128> lights = {};
  uint32_t count = 0;
  uint64_t epoch = 0, serial = 0;
  bool complete = false;
};
Frame Snapshot();
// Render-owner thread only. Identity is established before D3D9 value copying,
// then the complete API value is compared, never used to infer model identity.
uint64_t CurrentWorldSerial() noexcept;
bool ClaimDrawLight(uint32_t slot, const D3DLIGHT9& value, Sample& out) noexcept;
bool ValidateLease(const Sample* lights, uint32_t count, uint64_t serial) noexcept;
uint64_t EmitterGenerationForModel(const void* model) noexcept;
// Map-scoped CPU policy. Registration does not load models or invent instances.
bool SetModelPath(std::string_view path, bool enabled, bool shadows);
int32_t ModelPathCount(std::string_view path);
bool ModelPathRegistered(std::string_view path);
} // namespace dxvk::war3::native_light
