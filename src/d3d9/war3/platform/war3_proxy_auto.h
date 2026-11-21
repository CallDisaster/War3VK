// war3_proxy_auto.h - proxy mode without a launcher
//
// A player unpacks d3d9.dll and WarVK\x64 into the game directory and starts
// war3.exe. This DLL then finds the front end and the x64 host beside itself
// and writes the proxy settings into its own process environment, which the
// front end reads and the host inherits. Nothing is written outside the process
// except the run's log directory. Without WarVK\x64 nothing changes.
//
// The game loads and frees d3d9.dll several times, so the decision is kept in
// the process environment (WARVK_D3D9_AUTO_STATE) and survives a reload.
// WARVK_D3D9_AUTO=0 turns the search off.
#pragma once

namespace dxvk::war3::proxy::autostart {

enum class Mode {
  Off,        // in process
  Explicit,   // WARVK_D3D9_FRONT came from outside: never falls back
  Automatic,  // found beside this DLL: falls back to in process if it cannot start
};

// Safe during static initialisation: the first caller may be under the loader lock.
Mode CurrentMode() noexcept;

// Automatic mode, before the first factory: gives a process without a stderr
// handle a log file for the front end's diagnostics and trims old run logs.
void PrepareFirstFactory() noexcept;

// Automatic mode gives up for this process; CurrentMode() answers Off from now on.
void FallBack(const char* reason) noexcept;

}  // namespace dxvk::war3::proxy::autostart
