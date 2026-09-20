#include "war3_data_collection_tree.h"
#if defined(WARVK_DATA_COLLECTION_TREE_DEV) && WARVK_DATA_COLLECTION_TREE_DEV
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>

namespace dxvk::war3::collection {
namespace {
struct Config {
  bool enabled = false, valid = true;
  uint32_t period = 64;
  Config() {
    const char* env = std::getenv("DXVK_WAR3_DATA_COLLECTION_TREE");
    enabled = env && std::strcmp(env, "1") == 0;
    env = std::getenv("DXVK_WAR3_DATA_COLLECTION_SAMPLE_PERIOD");
    if (env && *env) {
      char* end = nullptr;
      const unsigned long value = std::strtoul(env, &end, 10);
      valid = end && !*end && value && value <= 4096 && !(value & (value - 1));
      if (valid) period = uint32_t(value);
    }
    enabled = enabled && valid;
  }
};
const Config& config() { static const Config c; return c; }
uint64_t clockTicks() noexcept { LARGE_INTEGER t; QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }
struct Published {
  uint32_t tid = 0;
  uint64_t epoch = 0, eligibleRoots = 0, publicationTicks = 0;
  TreeSnapshot tree;
};
struct Store {
  std::mutex mutex;
  std::array<Published, 8> threads;
  std::array<std::atomic<bool>, 8> inFlight = {};
  std::atomic<uint64_t> epoch{1}, frames{0}, threadOverflow{0}, abandoned{0};
  std::atomic<bool> recording{false};
};
Store& store() { static Store s; return s; }
struct Local {
  Tree tree;
  uint64_t epoch = 0, roots = 0, publicationTicks = 0;
  uint32_t depth = 0;
  uint32_t paused = 0;
  int slot = -1;
  bool sampled = false;
};
thread_local Local local;
void publish(Local& l) {
  auto& s = store();
  const auto begin = clockTicks();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (l.epoch != s.epoch.load(std::memory_order_relaxed) || l.slot < 0) return;
    auto& out = s.threads[size_t(l.slot)];
    out.tree = l.tree.snapshot();
    out.eligibleRoots = l.roots;
    out.publicationTicks = l.publicationTicks;
  }
  const uint64_t elapsed = clockTicks() - begin;
  if (UINT64_MAX - l.publicationTicks < elapsed) l.tree.fault();
  else l.publicationTicks += elapsed;
}
}

void SetRecording(bool recording) {
  if (!config().enabled) return;
  store().recording.store(recording, std::memory_order_release);
}
void ResetSession() {
  if (!config().enabled) return;
  auto& s = store();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto epoch = s.epoch.load();
  if (epoch == UINT64_MAX) { s.recording.store(false); return; }
  s.epoch.store(epoch + 1, std::memory_order_release);
  s.frames.store(0);
  // Old in-flight guards are rejected on epoch mismatch. Slots are not reused
  // until their next entry; stale writers never publish into a new session.
}
void NoteFrame() {
  if (config().enabled && store().recording.load(std::memory_order_relaxed))
    store().frames.fetch_add(1, std::memory_order_relaxed);
}

Scope::Scope(Tag tag) noexcept {
  if (!config().enabled) return;
  if (local.paused) return;
  auto& s = store();
  if (!s.recording.load(std::memory_order_relaxed)) return;
  auto& l = local;
  const auto epoch = s.epoch.load(std::memory_order_acquire);
  if (l.epoch != epoch) {
    if (l.depth) { s.abandoned.fetch_add(1); return; }
    l.tree.reset(); l.epoch = epoch; l.roots = 0; l.publicationTicks = 0;
    if (l.slot < 0) {
      std::lock_guard<std::mutex> lock(s.mutex);
      for (size_t i = 0; i < s.threads.size(); ++i) {
        if (!s.threads[i].tid) { l.slot = int(i); s.threads[i].tid = GetCurrentThreadId(); break; }
      }
      if (l.slot < 0) { s.threadOverflow.fetch_add(1); return; }
    }
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.threads[size_t(l.slot)].epoch = epoch;
      s.threads[size_t(l.slot)].tree = {};
    }
  }
  if (l.slot < 0) return;
  if (!l.depth) {
    if (l.roots == UINT64_MAX) { l.tree.fault(); return; }
    l.sampled = (l.roots++ & (config().period - 1u)) == 0;
    if (l.sampled) s.inFlight[size_t(l.slot)].store(true, std::memory_order_release);
  }
  m_state = &l; m_epoch = epoch; m_sampled = l.sampled;
  ++l.depth;
  if (m_sampled) m_token = l.tree.enter(tag, clockTicks());
}

Pause::Pause() noexcept : m_enabled(config().enabled) { if (m_enabled) ++local.paused; }
Pause::~Pause() noexcept { if (m_enabled) --local.paused; }
Scope::~Scope() noexcept {
  if (!m_state) return;
  auto& l = *static_cast<Local*>(m_state);
  auto& s = store();
  const bool same = m_epoch == s.epoch.load(std::memory_order_acquire);
  if (!l.depth) { l.tree.fault(); return; }
  if (m_sampled && same) l.tree.leave(m_token, clockTicks());
  --l.depth;
  if (!l.depth && m_sampled) {
    if (same) publish(l);
    else s.abandoned.fetch_add(1);
    s.inFlight[size_t(l.slot)].store(false, std::memory_order_release);
  }
}

std::string CaptureJson(uint32_t mainThreadId) {
  Pause excludeProfiler;
  if (!config().enabled) return std::string("{\"compiled\":true,\"enabled\":false,\"configValid\":") +
      (config().valid ? "true" : "false") + ",\"coverageComplete\":false}";
  auto& s = store();
  // Copy published owned records only, not another thread's live TLS tree.
  auto copy = std::make_unique<std::array<Published, 8>>();
  uint64_t epoch, frames;
  std::array<bool, 8> active = {};
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    *copy = s.threads;
    epoch = s.epoch.load(); frames = s.frames.load();
    for (size_t i = 0; i < active.size(); ++i) active[i] = s.inFlight[i].load();
  }
  LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
  const double scale = 1000.0 / double(frequency.QuadPart);
  std::ostringstream out;
  out.precision(12);
  out << "{\"compiled\":true,\"enabled\":true,\"schemaVersion\":1,\"coverageComplete\":false,"
         "\"counterScope\":\"recording-session-not-selected-report-window\","
         "\"semantics\":\"per-thread-union-of-instrumented-data-entry-intervals-not-thread-CPU\","
         "\"sampling\":\"one-decision-per-outer-invocation-all-descendants-inherit\","
         "\"notAdditiveWithExistingHookTree\":true,\"unknownOutsideRoots\":true,"
         "\"frames\":" << frames << ",\"session\":" << epoch << ",\"samplePeriod\":" << config().period
      << ",\"threadOverflow\":" << s.threadOverflow.load() << ",\"abandoned\":" << s.abandoned.load()
      << ",\"threads\":[";
  bool firstThread = true;
  for (size_t t = 0; t < copy->size(); ++t) {
    const auto& p = (*copy)[t];
    if (!p.tid || p.epoch != epoch) continue;
    if (!firstThread) out << ',';
    firstThread = false;
    const bool closed = Tree::closed(p.tree);
    out << "{\"threadId\":" << p.tid << ",\"mainThread\":" << (p.tid == mainThreadId ? "true" : "false")
        << ",\"inFlight\":" << (active[t] ? "true" : "false") << ",\"closed\":" << (closed ? "true" : "false")
        << ",\"faults\":" << p.tree.faults << ",\"eligibleRootsAtLastPublish\":" << p.eligibleRoots
        << ",\"publicationOverheadMsLowerBound\":" << double(p.publicationTicks) * scale << ",\"nodes\":[";
    for (uint16_t i = 0; i < p.tree.count; ++i) {
      const auto& n = p.tree.nodes[i];
      if (i) out << ',';
      out << "{\"id\":" << i << ",\"parent\":" << n.parent << ",\"name\":\"" << TagName(n.tag)
          << "\",\"sampledCalls\":" << n.calls << ",\"ticks\":\"" << n.ticks
          << "\",\"childTicks\":\"" << n.childTicks << "\",\"inclusiveMs\":" << double(n.ticks) * scale
          << ",\"unclassifiedMs\":" << double(n.ticks >= n.childTicks ? n.ticks - n.childTicks : 0) * scale << '}';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}
} // namespace dxvk::war3::collection
#endif
