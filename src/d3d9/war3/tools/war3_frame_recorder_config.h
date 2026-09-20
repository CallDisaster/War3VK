#pragma once
#include <cstdint>
#include <cstring>

namespace dxvk::war3::tools::evidence {
struct RecorderConfiguration {
  bool enabled=false,inputs=false,draws=false,local=false,internalBuild=false;
  // 2026-09-17 上级裁定（Step 1③）：对象级 palette 证据**子门**。默认关、受主门约束；
  // 进入统一配置面（effectiveConfiguration 可见），不在 core 内另造隐藏 env 入口。
  bool paletteObject=false;
};
inline bool RecorderFlag(const char* value,bool fallback) noexcept {
  // Unset inherits this build's default. Explicit 0 and malformed values fail
  // closed, including an empty supplied string; never modify process env here.
  return value?std::strcmp(value,"1")==0:fallback;
}
inline RecorderConfiguration ResolveRecorderConfiguration(bool internal,
    const char* master,const char* inputs,const char* draws,const char* local,
    const char* paletteObject) noexcept {
  const bool enabled=RecorderFlag(master,internal);
  // 子门默认关（fallback=false），且**受主门约束**：主门关时子门必然关。
  return {enabled,enabled&&RecorderFlag(inputs,internal),
    enabled&&RecorderFlag(draws,internal),enabled&&RecorderFlag(local,internal),internal,
    enabled&&RecorderFlag(paletteObject,false)};
}
// Admission is a disk budget, not a guarantee against concurrent disk writers.
// Preserve partial evidence if later writes fail; do not delete older sessions.
inline constexpr uint64_t RecorderDiskHeadroom=6ull*1024*1024*1024;
inline constexpr bool RecorderDiskBudget(bool queryOk,uint64_t available) noexcept {
  return queryOk&&available>=RecorderDiskHeadroom;
}
} // namespace dxvk::war3::tools::evidence
