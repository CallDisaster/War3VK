// war3_proxy_status.h - proxy mode's section of the runtime status
//
// The control plane's get_runtime_status and the periodic status file
// (WarVK/Temp/runtime_status.json) carry it as "proxyMode": what the session
// with the host has done, as far as this process knows it - the front end
// (war3_proxy_mode.h Status), the frame producer (war3_proxy_frame.h
// FrameStatus) and its shadow casters (war3_proxy_casters.h CasterStatus).
// The real-game gates read it in proxy mode where the in-process arm reads
// the in-process renderer's diagnostics.
//
// Implemented in war3_proxy_frame.cpp. Callable from any thread.
#pragma once

#include <nlohmann/json.hpp>

namespace dxvk::war3::proxy {

// Null when proxy mode was not requested: the in-process status is unchanged.
nlohmann::json StatusJson();

} // namespace dxvk::war3::proxy
