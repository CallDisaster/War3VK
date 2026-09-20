// 2026-09-18 独立复审批次 4：**真实准入规则**的正常/异常对照（纯 CPU，无 IO/Vulkan/游戏）。
//
// 背景：`war3_live_palette_selection.cpp` 是互斥双路；本构建 `ContractEnabled()==true`，
// 因此真实入口是 `QueryOwnedRenderablePartPaletteSnapshot()`（war3_model_hook.cpp:9384），
// 其准入判定**全部**由本文件所测的纯谓词完成（war3_skin_palette_selection.h:34-62）。
// 既有 differential 测试把该函数**替换成替身**，故其真实规则此前无覆盖 —— 本文件补上。
//
// 四类对照（对应复审要求：正常输入 / 矩阵数量不足 / 提交中途被换 / 对象或地图切换）：
//   ① 正常：全部条件成立 ⇒ 允许；
//   ② 数量不足：required > actualGroupCount ⇒ 拒绝（GroupRange）；
//   ③ 提交中途被换：slot 或 frameTag 与当场读到的值不符 ⇒ 拒绝；
//   ④ 对象/地图切换：ownerEpoch 改变（或 model/part 不符）⇒ 拒绝。
// 另测 CanReplace（提交期替换规则）与 publicationTicket/hash/slot 上界等前置。

#include "../war3_skin_palette_selection.h"

#include <cstdint>
#include <cstdio>

namespace skin = dxvk::war3::render::skin;

namespace {

uint32_t g_checks = 0u;
uint32_t g_failed = 0u;

bool Require(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failed;
    std::printf("    FAIL: %s\n", what);
  }
  return condition;
}

// 一个**完全合规**的 owned-part 快照选择（正常对照的基准）。
skin::Selection MakeValidSelection(uintptr_t model, uintptr_t part,
                                   uint64_t epoch, uint32_t slot,
                                   uint32_t frameTag, uint32_t actualGroupCount) {
  skin::Selection s{};
  s.source = skin::Source::OwnedPartSnapshot;
  s.space = skin::Space::World;
  s.domain = skin::Domain::VertexGroups;
  s.runtimeModel = model;
  s.part = part;
  s.ownerEpoch = epoch;
  s.publicationTicket = 1u;
  s.hash = 0xABCDEFull;
  s.slot = slot;
  s.frameTag = frameTag;
  s.actualGroupCount = actualGroupCount;
  return s;
}

// ---- ① 正常 + ② 数量不足 ----
bool CaseNormalAndCountShort() {
  bool ok = true;
  const uintptr_t model = 0x1000u, part = 0x2000u;
  const uint64_t epoch = 7u;
  const uint32_t slot = 42u, frameTag = 900u;
  const skin::Selection valid = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);

  // ① 正常：required(9) <= actual(9) 且其余全合规 ⇒ 允许
  ok &= Require(skin::OwnedSnapshotMatches(valid, model, part, epoch, slot, frameTag, 9u),
                "normal: fully compliant selection must be admitted");
  ok &= Require(skin::OwnedSnapshotMatches(valid, model, part, epoch, slot, frameTag, 4u),
                "normal: required below actual must be admitted");
  ok &= Require(skin::GroupRange(9u, 9u), "GroupRange: equal is allowed");
  ok &= Require(skin::GroupRange(4u, 9u), "GroupRange: less is allowed");

  // ② 数量不足：required(10) > actual(9) ⇒ 拒绝（这正是外部复审在 legacy 冷缓存发现的缺口）
  ok &= Require(!skin::GroupRange(10u, 9u), "GroupRange: required > actual must be refused");
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, epoch, slot, frameTag, 10u),
                "count short: required > actualGroupCount must be refused");
  ok &= Require(!skin::GroupRange(0u, 9u), "GroupRange: required == 0 must be refused");
  ok &= Require(!skin::GroupRange(9u, 0u), "GroupRange: actual == 0 must be refused");
  ok &= Require(!skin::GroupRange(260u, 300u), "GroupRange: actual > 256 must be refused");
  return ok;
}

// ---- ③ 提交中途被换（slot / frameTag） ----
bool CaseReplacedMidSubmission() {
  bool ok = true;
  const uintptr_t model = 0x1000u, part = 0x2000u;
  const uint64_t epoch = 7u;
  const uint32_t slot = 42u, frameTag = 900u;
  const skin::Selection valid = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);

  // 快照的 slot 与当场读到的 slot 不符 ⇒ 该次提交不得使用它
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, epoch, 43u, frameTag, 9u),
                "slot moved after copy: must be refused");
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, epoch, slot, 901u, 9u),
                "frame tag moved after copy: must be refused");
  // slot 上界：0x3a98 起为非法域
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, epoch, 0x3a98u, frameTag, 9u),
                "slot at the illegal-domain boundary must be refused");
  return ok;
}

// ---- ④ 对象 / 地图切换（epoch、model、part） ----
bool CaseObjectOrMapSwitch() {
  bool ok = true;
  const uintptr_t model = 0x1000u, part = 0x2000u;
  const uint64_t epoch = 7u;
  const uint32_t slot = 42u, frameTag = 900u;
  const skin::Selection valid = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);

  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, 8u, slot, frameTag, 9u),
                "map/owner epoch changed: must be refused");
  ok &= Require(!skin::OwnedSnapshotMatches(valid, 0x1001u, part, epoch, slot, frameTag, 9u),
                "runtime model changed: must be refused");
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, 0x2001u, epoch, slot, frameTag, 9u),
                "renderable part changed: must be refused");
  ok &= Require(!skin::OwnedSnapshotMatches(valid, model, part, 0u, slot, frameTag, 9u),
                "epoch == 0: must be refused");
  return ok;
}

// ---- 前置：ticket / hash / source / space / domain ----
bool CasePreconditions() {
  bool ok = true;
  const uintptr_t model = 0x1000u, part = 0x2000u;
  const uint64_t epoch = 7u;
  const uint32_t slot = 42u, frameTag = 900u;

  skin::Selection noTicket = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);
  noTicket.publicationTicket = 0u;
  ok &= Require(!skin::OwnedSnapshotMatches(noTicket, model, part, epoch, slot, frameTag, 9u),
                "publicationTicket == 0 must be refused");

  skin::Selection noHash = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);
  noHash.hash = 0u;
  ok &= Require(!skin::OwnedSnapshotMatches(noHash, model, part, epoch, slot, frameTag, 9u),
                "hash == 0 must be refused");

  skin::Selection captured = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);
  captured.source = skin::Source::CapturedWriter;
  ok &= Require(!skin::OwnedSnapshotMatches(captured, model, part, epoch, slot, frameTag, 9u),
                "a CapturedWriter selection may not pass the owned-snapshot rule");

  skin::Selection wrongSpace = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);
  wrongSpace.space = skin::Space::ModelLocal;
  ok &= Require(!skin::OwnedSnapshotMatches(wrongSpace, model, part, epoch, slot, frameTag, 9u),
                "a model-local selection must be refused");

  skin::Selection wrongDomain = MakeValidSelection(model, part, epoch, slot, frameTag, 9u);
  wrongDomain.domain = skin::Domain::Bones;
  ok &= Require(!skin::OwnedSnapshotMatches(wrongDomain, model, part, epoch, slot, frameTag, 9u),
                "a non-VertexGroups domain must be refused");
  return ok;
}

// ---- 提交期替换规则 CanReplace ----
bool CaseCanReplace() {
  bool ok = true;
  const skin::Selection empty{};
  const skin::Selection next =
      MakeValidSelection(0x1000u, 0x2000u, 7u, 42u, 900u, 9u);
  ok &= Require(skin::CanReplace(empty, next), "CanReplace: first publication is allowed");

  const skin::Selection sameOld =
      MakeValidSelection(0x1000u, 0x2000u, 7u, 41u, 899u, 9u);
  ok &= Require(skin::CanReplace(sameOld, next),
                "CanReplace: same part/model/epoch may be replaced");

  const skin::Selection otherPart =
      MakeValidSelection(0x1000u, 0x2999u, 7u, 41u, 899u, 9u);
  ok &= Require(!skin::CanReplace(otherPart, next),
                "CanReplace: a different part must not take over");

  const skin::Selection otherModel =
      MakeValidSelection(0x1999u, 0x2000u, 7u, 41u, 899u, 9u);
  ok &= Require(!skin::CanReplace(otherModel, next),
                "CanReplace: a different runtime model must not take over");

  const skin::Selection otherEpoch =
      MakeValidSelection(0x1000u, 0x2000u, 6u, 41u, 899u, 9u);
  ok &= Require(!skin::CanReplace(otherEpoch, next),
                "CanReplace: a previous epoch must not take over");

  skin::Selection nextNoTicket = next;
  nextNoTicket.publicationTicket = 0u;
  ok &= Require(!skin::CanReplace(empty, nextNoTicket),
                "CanReplace: a candidate without a publication ticket must be refused");
  return ok;
}

// ---- 共享数量准入规则（R1 的单一实现）----
bool CaseProducerGroupCountRule() {
  bool ok = true;
  // 冷/热缓存必须同规则：producer 的 groupCount 必须覆盖本次所需矩阵数
  ok &= Require(skin::ProducerGroupCountCovers(9u, 9u), "rule: equal covers");
  ok &= Require(skin::ProducerGroupCountCovers(4u, 9u), "rule: surplus covers");
  ok &= Require(!skin::ProducerGroupCountCovers(10u, 9u),
                "rule: one short MUST be refused (this is the R1 case)");
  ok &= Require(!skin::ProducerGroupCountCovers(9u, 0u),
                "rule: producer reporting nothing must be refused");
  ok &= Require(!skin::ProducerGroupCountCovers(0u, 9u),
                "rule: a zero requirement is not a pass");
  // 本函数**只管覆盖关系**：上界由调用方把关（选择链在 `requiredPaletteCount > 256u` 处
  // 早退，严格准入函数在 `required > 64` 处早退）⇒ 此处 257/300 应当为 true。
  ok &= Require(skin::ProducerGroupCountCovers(257u, 300u),
                "rule: this predicate only checks coverage; the bound lives in the callers");
  return ok;
}

struct Case { const char* name; bool (*body)(); };

} // namespace

int main() {
  const Case cases[] = {
      {"1 normal admission and count-short refusal", &CaseNormalAndCountShort},
      {"2 replaced mid-submission (slot/frameTag)", &CaseReplacedMidSubmission},
      {"3 object or map switch (epoch/model/part)", &CaseObjectOrMapSwitch},
      {"4 preconditions (ticket/hash/source/space/domain)", &CasePreconditions},
      {"5 submission-time replacement rule (CanReplace)", &CaseCanReplace},
      {"6 shared producer-group-count admission rule", &CaseProducerGroupCountRule},
  };
  uint32_t passed = 0u, failed = 0u;
  for (const Case& c : cases) {
    const uint32_t before = g_failed;
    const uint32_t checksBefore = g_checks;
    const bool ok = c.body();
    const uint32_t caseFailures = g_failed - before;
    const uint32_t caseChecks = g_checks - checksBefore;
    std::printf("[%s] %s (%u checks, %u failures)\n", ok ? "PASS" : "FAIL", c.name,
                static_cast<unsigned>(caseChecks),
                static_cast<unsigned>(caseFailures));
    if (ok) ++passed; else ++failed;
  }
  std::printf("SUMMARY: %u passed, %u failed\n", static_cast<unsigned>(passed),
              static_cast<unsigned>(failed));
  return failed == 0u ? 0 : 1;
}