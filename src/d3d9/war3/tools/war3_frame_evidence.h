#pragma once

#include "war3_frame_evidence_core.h"
#include <exception>
#include <string>

namespace dxvk::war3::tools::evidence {

bool Enabled() noexcept;
bool InputsEnabled() noexcept;
// 2026-09-17 上级裁定（Step 1③）：对象级 palette 证据子门（默认关、受主门约束）。
bool PaletteObjectEvidenceEnabled() noexcept;
bool DrawsEnabled() noexcept;
bool LocalRecorderEnabled() noexcept;
bool InternalRecorderBuild() noexcept;
std::wstring OutputDirectory();
uint64_t ActiveSession() noexcept;
bool FrozenForExport(uint64_t session) noexcept;
uint64_t ProcessNonce() noexcept;
uint64_t Record(uint64_t session, Event event) noexcept;
bool RequestTriggerFromGame() noexcept;
class Scope {
public:
  Scope(Kind kind, const Key& key, const char* label, uint64_t parent=0) noexcept;
  ~Scope();
  Scope(const Scope&)=delete;
  Scope& operator=(const Scope&)=delete;
  uint64_t id() const noexcept { return m_id; }
  uint64_t session() const noexcept { return m_session; }
  void outcome(uint64_t value) noexcept { m_data[0]=value; }
  void value(uint32_t index,uint64_t v) noexcept { if(index<m_data.size()) m_data[index]=v; }
private:
  Key m_key{};
  Kind m_kind=Kind::Camera;
  std::array<char,32> m_label{};
  std::array<uint64_t,4> m_data{};
  uint64_t m_truncated=0;
  uint64_t m_session=0,m_id=0;
  int m_exceptions=0;
};
} // namespace dxvk::war3::tools::evidence
