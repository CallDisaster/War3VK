#pragma once
#include <cstdint>
#include <string>
#if defined(WARVK_DATA_COLLECTION_TREE_DEV) && WARVK_DATA_COLLECTION_TREE_DEV
#include "war3_data_collection_tree_core.h"
namespace dxvk::war3::collection {
void SetRecording(bool recording);
void ResetSession();
void NoteFrame();
std::string CaptureJson(uint32_t mainThreadId);
class Pause {
public:
  Pause() noexcept;
  ~Pause() noexcept;
  Pause(const Pause&) = delete;
  Pause& operator=(const Pause&) = delete;
private:
  bool m_enabled = false;
};
class Scope {
public:
  explicit Scope(Tag tag) noexcept;
  ~Scope() noexcept;
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
private:
  void* m_state = nullptr;
  uint64_t m_token = 0, m_epoch = 0;
  bool m_sampled = false;
};
}
#define WARVK_DATA_JOIN_(a,b) a##b
#define WARVK_DATA_JOIN(a,b) WARVK_DATA_JOIN_(a,b)
#define WARVK_DATA_SCOPE(tag) ::dxvk::war3::collection::Scope WARVK_DATA_JOIN(warvkDataScope_,__LINE__)(::dxvk::war3::collection::Tag::tag)
#else
namespace dxvk::war3::collection {
inline void SetRecording(bool) {}
inline void ResetSession() {}
inline void NoteFrame() {}
inline std::string CaptureJson(uint32_t) { return "{\"compiled\":false,\"enabled\":false,\"coverageComplete\":false}"; }
class Pause {};
}
#define WARVK_DATA_SCOPE(tag) do {} while (false)
#endif
