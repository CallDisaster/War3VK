#pragma once

#include <cstdint>

namespace dxvk::war3::diagnostics {

// Wire v1 IDs. Append only with a new reader contract; do not reorder.
enum class StartupStage : uint32_t {
  Bootstrap = 1,
  BootstrapGate,
  GameModuleValidation,
  MinHookInitialization,
  EarlyHooks,
  JassHookInstallation,
  LifecycleHookInstallation,
  NativeLightInstallation,
  EarlyModelInstallation,
  MainRunner,
  MainRunnerEarlyHooks,
  MainRunnerOriginal,
  MainRunnerAlt,
  MainRunnerAltEarlyHooks,
  MainRunnerAltOriginal,
  RuntimeActivation,
  RuntimeActivationGate,
  RuntimeCore,
  War3Initialize,
  ShaderReload,
  NetEventInitialize,
  ControlPlaneInitialize,
  ControlServerStart,
  ControlServerLoop,
  PipeCreate,
  PipeConnect,
  StormRuntimeInitialize,
  War3PreInit,
  JassRoot,
  War3Init,
  JassReadyCallbacks,
  ModulesInitialize,
  GameHooksInstall,
  RuntimeReady,
  MainRunnerComplete,
  MainRunnerAltComplete,
};

// For PipeCreate/PipeConnect End ONLY: detail is 1 on success, 0 on failure;
// extra is zero on success and the immediately saved Win32 error on failure.
// A later failed pipe operation may be a Note(detail=0, extra=savedError)
// without another Begin. A successful pipe operation MUST NOT be a Note.
// Other detail/extra fields are raw, stage-local observations, not proof of
// readiness. RuntimeReady is an explicit Note after actual activation finishes.
// Begin token 0 means not recorded. End(0) is a no-op. Other Ends require the
// exact successful Begin token, stage and thread; no guessed cross-thread pair.
// All enabled wrappers preserve LastError, including initialization failures.
#if defined(WARVK_STARTUP_WITNESS_DEV)

// Call once from the existing D3D9 bootstrap, NEVER DllMain/loader callbacks.
// Independent opt-in: DXVK_WAR3_STARTUP_WITNESS=1 and a fresh absolute local
// DXVK_WAR3_STARTUP_WITNESS_FILE path. No path default; no overwrite/reopen.
// Successful mapping remains alive until process exit. The external reader
// must have the exact PID/creation exit witness before opening the file.
void InitializeStartupWitness() noexcept;
uint32_t BeginStartupStage(StartupStage stage, uint32_t detail = 0,
                          uint32_t extra = 0) noexcept;
void EndStartupStage(StartupStage stage, uint32_t token,
                     uint32_t detail = 0, uint32_t extra = 0) noexcept;
void NoteStartupStage(StartupStage stage, uint32_t detail = 0,
                      uint32_t extra = 0) noexcept;

#else

inline void InitializeStartupWitness() noexcept { }
inline uint32_t BeginStartupStage(StartupStage, uint32_t = 0,
                                  uint32_t = 0) noexcept { return 0; }
inline void EndStartupStage(StartupStage, uint32_t, uint32_t = 0,
                            uint32_t = 0) noexcept { }
inline void NoteStartupStage(StartupStage, uint32_t = 0,
                             uint32_t = 0) noexcept { }

#endif
} // namespace dxvk::war3::diagnostics
