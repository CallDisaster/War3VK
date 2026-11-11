#pragma once

#include "war3_startup_witness.h"
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace dxvk::war3::diagnostics::startupwitness {

constexpr uint32_t kMagic = 0x53545731; // STW1
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderWords = 64;
constexpr uint32_t kEventWords = 16;
constexpr uint32_t kCapacity = 256;
constexpr uint32_t kLastStage = uint32_t(StartupStage::MainRunnerAltComplete);
constexpr uint32_t kFileBytes = 16640;
enum class Phase : uint32_t { Begin = 1, End = 2, Note = 3 };

// Exact little-endian wire, read ONLY after the owning process has exited.
// Header: magic/version/dimensions [0..4], committedCount[5], pid[6],
// creationFILETIME[7..8], initialized[9], busy[10], busyOverflow[11],
// full[12], invalid[13], counterOverflow[14], clockFailure[15],
// QPCFrequency[16..17]; [18..63] must be zero.
// Event: sequence(commit LAST)[0], stage[1], phase[2], tid[3], token[4],
// QPC[5..6], detail[7], extra[8]; [9..15] must be zero.
struct Header { std::atomic<uint32_t> words[kHeaderWords]{}; };
struct Event { std::atomic<uint32_t> words[kEventWords]{}; };
struct Journal { Header header; Event events[kCapacity]; };
struct Sample { uint32_t thread = 0; uint64_t ticks = 0;
                uint32_t detail = 0; uint32_t extra = 0; };
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "startup witness requires lock-free 32-bit atomics");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
static_assert(sizeof(Header) == 256 && sizeof(Event) == 64);
static_assert(sizeof(Journal) == kFileBytes);
static_assert(std::is_trivially_copyable<Sample>::value);

// Caller supplies a cold, zeroed journal and immutable identity before
// publication. No WinAPI, original calls, allocation, formatting, I/O, waits,
// spin loops, game-memory reads or device locks occur in this core.
class Core {
public:
  explicit Core(Journal& journal) noexcept : m_journal(journal) { }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  uint32_t begin(StartupStage stage, const Sample& sample) noexcept {
    if (!acquire()) return 0;
    Guard guard{m_cell};
    if (!valid(stage, sample)) return 0;
    const uint32_t token = count() + 1;
    return publish(stage, Phase::Begin, token, sample) ? token : 0;
  }

  bool end(StartupStage stage, uint32_t token, const Sample& sample) noexcept {
    if (!token) return false; // Its refused Begin already accounted for loss.
    if (!acquire()) return false;
    Guard guard{m_cell};
    if (!valid(stage, sample)) return false;
    if (token > count() || token > kCapacity || m_finished[token - 1]) {
      increment(13); return false;
    }
    const auto& begin = m_journal.events[token - 1].words;
    if (begin[0].load(std::memory_order_relaxed) != token ||
        begin[1].load(std::memory_order_relaxed) != uint32_t(stage) ||
        begin[2].load(std::memory_order_relaxed) != uint32_t(Phase::Begin) ||
        begin[3].load(std::memory_order_relaxed) != sample.thread ||
        begin[4].load(std::memory_order_relaxed) != token) {
      increment(13); return false;
    }
    if (!pipeResultValid(stage, sample)) return false;
    if (!publish(stage, Phase::End, token, sample)) return false;
    m_finished[token - 1] = true;
    return true;
  }

  bool note(StartupStage stage, const Sample& sample) noexcept {
    if (!acquire()) return false;
    Guard guard{m_cell};
    if (!valid(stage, sample)) return false;
    if ((stage == StartupStage::PipeCreate || stage == StartupStage::PipeConnect)
        && sample.detail != 0) {
      increment(13); return false;
    }
    return publish(stage, Phase::Note, 0, sample);
  }

  void clockFailure() noexcept { increment(15); }

private:
  friend struct CoreTestAccess;
  struct Guard {
    std::atomic_flag& cell;
    ~Guard() { cell.clear(std::memory_order_release); }
  };

  bool acquire() noexcept {
    if (!m_cell.test_and_set(std::memory_order_acquire)) return true;
    // One operation, never retry. Wrap explicitly invalidates exact counting.
    if (m_journal.header.words[10].fetch_add(1, std::memory_order_relaxed)
        == UINT32_MAX)
      m_journal.header.words[11].store(1, std::memory_order_relaxed);
    return false;
  }
  void increment(uint32_t slot) noexcept {
    if (m_journal.header.words[slot].fetch_add(1, std::memory_order_relaxed)
        == UINT32_MAX)
      m_journal.header.words[14].store(1, std::memory_order_relaxed);
  }
  uint32_t count() const noexcept {
    return m_journal.header.words[5].load(std::memory_order_relaxed);
  }
  bool valid(StartupStage stage, const Sample& sample) noexcept {
    if (!sample.thread || !uint32_t(stage) || uint32_t(stage) > kLastStage) {
      increment(13); return false;
    }
    return true;
  }
  bool pipeResultValid(StartupStage stage, const Sample& sample) noexcept {
    if (stage != StartupStage::PipeCreate && stage != StartupStage::PipeConnect)
      return true;
    if (sample.detail > 1 || (sample.detail == 1 && sample.extra != 0)) {
      increment(13); return false;
    }
    return true;
  }
  bool publish(StartupStage stage, Phase phase, uint32_t token,
               const Sample& sample) noexcept {
    const uint32_t index = count();
    if (index >= kCapacity) { increment(12); return false; }
    auto& event = m_journal.events[index].words;
    event[1].store(uint32_t(stage), std::memory_order_relaxed);
    event[2].store(uint32_t(phase), std::memory_order_relaxed);
    event[3].store(sample.thread, std::memory_order_relaxed);
    event[4].store(token, std::memory_order_relaxed);
    event[5].store(uint32_t(sample.ticks), std::memory_order_relaxed);
    event[6].store(uint32_t(sample.ticks >> 32), std::memory_order_relaxed);
    event[7].store(sample.detail, std::memory_order_relaxed);
    event[8].store(sample.extra, std::memory_order_relaxed);
    event[0].store(index + 1, std::memory_order_release);
    m_journal.header.words[5].store(index + 1, std::memory_order_release);
    return true;
  }

  Journal& m_journal;
  std::atomic_flag m_cell = ATOMIC_FLAG_INIT;
  bool m_finished[kCapacity]{};
};
} // namespace dxvk::war3::diagnostics::startupwitness
