// war3_proxy_host_lost.h - the render host is gone
//
// In proxy mode the picture is drawn by the x64 host. Once the session with it
// has ended - the host exited, stopped answering, lost its GPU device, or the
// stream between the two is no longer valid - nothing can be drawn again:
// every D3D9 call answers D3DERR_DEVICELOST, and the textures the host held
// exist nowhere else, so the device cannot be rebuilt
// (docs/plan/2026-11-16-x64-phase5-1-first-wiring.md, 6.2). The game itself is
// still running. The player is told why the picture stopped, and the game ends.
#pragma once

#include "../core/war3_d3d9_observer.h"

#include <cstdint>

namespace dxvk::war3::proxy {

// The exit code of a game process that ended because its render host was
// lost: an application-defined error code, for launchers and tests.
constexpr uint32_t kRenderHostLostExitCode = 0xE0574B01u;

// What this process had done with the device.
struct HostLostContext {
  uint64_t presents = 0;
  uint64_t draws = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Writes <game directory>\WarVK\Log\render-host-lost.txt, shows the player a
// dialog (war3_player_notice.h; DXVK_WAR3_HOST_LOST_DIALOG overrides where it
// is shown), then ends the process with kRenderHostLostExitCode.
//
// The process is terminated, not exited: the game's and the front end's
// shutdown paths would run against a device that answers nothing, and what the
// player was told must not depend on them finishing. The game in progress is
// not saved.
[[noreturn]] void ReportRenderHostLostAndExit(const WarVKD3D9TerminalFacts& facts,
                                              const HostLostContext& context);

} // namespace dxvk::war3::proxy
