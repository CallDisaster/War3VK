// Table-driven positive/negative tests for the frozen export progress
// classifier (delegation plan 2026-09-16-dsh-dual-agent-supervision, GLM path 1).
//
// Includes the ACTUAL production header at its frozen path; no local
// re-declaration and no second capacity constant: MaxSlots comes from the
// existing history core so the boundary cannot drift from production.
//
// Scope: pure CPU classification only. No allocation beyond these constant
// tables, no OS/GPU/log calls. This file is static text at authoring time;
// compilation is run later by the main thread. Passing these rows is a CPU
// contract check of ClassifyExportProgress only - never a runtime commit,
// HUD, GPU or game evidence, and not proof that the wiring calls it.

#include "../src/d3d9/war3/tools/war3_frame_history_export_core.h"

#include "../src/d3d9/war3/tools/war3_frame_history_core.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace history = dxvk::war3::tools::history;
using history::ClassifyExportProgress;
using history::ExportOutcome;
using history::ExportProgress;
using history::MaxSlots;

// Frozen signature stability: the classifier must stay noexcept.
static_assert(noexcept(ClassifyExportProgress(std::declval<const ExportProgress&>())),
              "ClassifyExportProgress must remain noexcept per the frozen contract");
// Pure-CPU value contract: the frozen struct is a plain trivially copyable
// aggregate with default member initialisers, no ownership, no allocation.
static_assert(std::is_trivially_copyable<ExportProgress>::value,
              "ExportProgress must stay a trivially copyable CPU-only value");

namespace {

struct Case {
  const char* name;
  ExportProgress progress;
  ExportOutcome expected;
};

ExportProgress progress(std::uint32_t retained, std::uint32_t queued,
                        std::uint32_t done, std::uint32_t failed,
                        bool requestsValid) {
  ExportProgress p;
  p.retained = retained;
  p.queued = queued;
  p.done = done;
  p.failed = failed;
  p.requestsValid = requestsValid;
  return p;
}

const char* outcome_name(ExportOutcome outcome) {
  switch (outcome) {
    case ExportOutcome::Pending: return "Pending";
    case ExportOutcome::Complete: return "Complete";
    case ExportOutcome::ReadbackFailed: return "ReadbackFailed";
    case ExportOutcome::InvalidProgress: return "InvalidProgress";
  }
  return "<out-of-range>";
}

// Every row is one frozen-semantics decision. Rows are named so a failure
// names the violated rule instead of an index only.
const std::vector<Case>& case_table() {
  static const std::vector<Case> kTable = {
      // InvalidProgress: request flag and population/order contradictions,
      // in the frozen semantic order (any hit classifies as invalid).
      {"requests-invalid-flag",            progress(MaxSlots, MaxSlots, MaxSlots, 0, false), ExportOutcome::InvalidProgress},
      {"retained-zero",                    progress(0, 0, 0, 0, true),                      ExportOutcome::InvalidProgress},
      {"retained-maxslots-plus-one",       progress(MaxSlots + 1, MaxSlots + 1, MaxSlots + 1, 0, true), ExportOutcome::InvalidProgress},
      {"retained-uint32-max",              progress(UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, true),      ExportOutcome::InvalidProgress},
      {"failed-greater-than-done",         progress(4, 4, 2, 3, true),                      ExportOutcome::InvalidProgress},
      {"done-greater-than-queued",         progress(4, 2, 3, 0, true),                      ExportOutcome::InvalidProgress},
      {"queued-greater-than-retained",     progress(4, 5, 3, 0, true),                      ExportOutcome::InvalidProgress},
      // done > queued with done+failed overflowing: classification must come
      // from the ordering rule, never from an overflowing sum.
      {"done-uint32-max-overflow-bait",    progress(MaxSlots, MaxSlots, UINT32_MAX, 1, true), ExportOutcome::InvalidProgress},
      {"all-contradictions-at-once",       progress(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, false), ExportOutcome::InvalidProgress},

      // Pending: valid and done < retained; failures do not shortcut the
      // window, remaining queue entries keep it pending.
      {"pending-none-done",                progress(4, 4, 0, 0, true),                      ExportOutcome::Pending},
      {"pending-partial-failure-waits",    progress(4, 4, 2, 1, true),                      ExportOutcome::Pending},
      {"pending-partially-queued",         progress(4, 2, 1, 0, true),                      ExportOutcome::Pending},
      {"pending-maxslots-one-remaining",   progress(MaxSlots, MaxSlots, MaxSlots - 1, 3, true), ExportOutcome::Pending},
      {"pending-done-queued-below-retained", progress(4, 3, 3, 1, true),                    ExportOutcome::Pending},

      // Complete: everything done, nothing failed, at small and boundary sizes.
      {"complete-all-done-no-failures",    progress(4, 4, 4, 0, true),                      ExportOutcome::Complete},
      {"complete-maxslots-exact-boundary", progress(MaxSlots, MaxSlots, MaxSlots, 0, true), ExportOutcome::Complete},
      {"complete-single-slot",             progress(1, 1, 1, 0, true),                      ExportOutcome::Complete},

      // ReadbackFailed: everything done with at least one failure.
      {"readback-failed-partial",          progress(4, 4, 4, 2, true),                      ExportOutcome::ReadbackFailed},
      {"readback-failed-all-failed",       progress(4, 4, 4, 4, true),                      ExportOutcome::ReadbackFailed},
      {"readback-failed-maxslots-exact",   progress(MaxSlots, MaxSlots, MaxSlots, 1, true), ExportOutcome::ReadbackFailed},
      // failed == done is the equality edge at full capacity; only failed >
      // done would be invalid.
      {"readback-failed-failed-eq-done-max", progress(MaxSlots, MaxSlots, MaxSlots, MaxSlots, true), ExportOutcome::ReadbackFailed},
  };
  return kTable;
}

}  // namespace

int main() {
  const std::vector<Case>& table = case_table();
  std::vector<ExportOutcome> first;
  first.reserve(table.size());
  std::size_t failures = 0;
  for (std::size_t i = 0; i < table.size(); ++i) {
    const ExportOutcome got = ClassifyExportProgress(table[i].progress);
    first.push_back(got);
    if (got != table[i].expected) {
      std::printf("FAIL row %zu (%s): expected %s, got %s\n", i, table[i].name,
                  outcome_name(table[i].expected), outcome_name(got));
      ++failures;
    }
  }
  // Boundary stability: the classifier owns no mutable state, so repeating
  // the same immutable queries must reproduce the first pass exactly.
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (ClassifyExportProgress(table[i].progress) != first[i]) {
      std::printf("FAIL row %zu (%s): unstable on repeat call\n", i, table[i].name);
      ++failures;
    }
  }
  std::printf("export core classifier rows=%zu failures=%zu %s\n", table.size(),
              failures, failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}