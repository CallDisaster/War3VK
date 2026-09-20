#pragma once
#include "war3_frame_recorder_control.h"
#include <atomic>
#include <string>
#include <nlohmann/json.hpp>

namespace dxvk::war3::tools::evidence {
// Pipe worker or DLL recorder worker only; never a render/hotkey callback.
// The local owner excludes mutating pipe commands for the entire session.
bool ClaimLocalRecorder(RecorderControlLease& lease) noexcept;
bool ReleaseLocalRecorder(RecorderControlLease& lease) noexcept;
bool OwnsLocalRecorder(const RecorderControlLease& lease) noexcept;
nlohmann::json Control(const nlohmann::json& payload, const RecorderControlLease* localLease=nullptr,
                      const std::atomic<bool>* stopping=nullptr);
using InputExporter = nlohmann::json (*)(uint64_t, const std::wstring&, const std::atomic<bool>*);
void RegisterInputExporter(InputExporter exporter) noexcept;
}

namespace dxvk::war3::tools {
nlohmann::json FrameHistoryControl(const nlohmann::json& payload,
                                  const evidence::RecorderControlLease* localLease=nullptr);
}
