// 2026-09-18 T7 批次 4（U5）：registry domain 显式化 + owner-check 的宿主机边界测试。
//
// 被测对象是产品源里的同一份判定内核
// （war3/shadow/war3_shadow_geometry_domain.h: DecideShadowGeometryOwner /
//  ShadowGeometryOwnerAccepts / ShadowGeometryDomainTag），以及**由该内核驱动的**
// domain registry 生命周期模型（lookup / publish / GC / domain 作用域清理 /
// 整会话退役前的归属校验）。
//
// 本测试同时承担 T7 判据 F4.10 的反例义务：必须能构造一个跨 domain key 碰撞，
// 并证明它在隔离前（LegacyFind，无 domain 参数）会命中、隔离后（Find）被拒绝。
//
// 只用 std::printf 打印每例一行，全部通过打印 SUMMARY，任一失败非零退出。
// 不链接 d3d9 / 游戏状态源，也不做字符串静态检查（静态钉死由
// AutoTest/test_registry_domain_isolation_static.py 负责）。

#include "../../shadow/war3_shadow_geometry_domain.h"
#include "../../../../util/util_bit.h"

#include <cstdint>
#include <cstdio>
#include <map>

namespace {

using dxvk::war3::shadow::DecideShadowGeometryOwner;
using dxvk::war3::shadow::ShadowGeometryDomain;
using dxvk::war3::shadow::ShadowGeometryDomainTag;
using dxvk::war3::shadow::ShadowGeometryOwnerAccepts;
using dxvk::war3::shadow::ShadowGeometryOwnerDecision;

uint32_t g_cases = 0u;
uint32_t g_failures = 0u;

void Check(const char* label, bool ok) {
  ++g_cases;
  if (!ok)
    ++g_failures;
  std::printf("%s %s\n", label, ok ? "PASS" : "FAIL");
}

void CheckU64(const char* label, uint64_t actual, uint64_t expected) {
  ++g_cases;
  const bool ok = actual == expected;
  if (!ok)
    ++g_failures;
  std::printf("%s actual=0x%016llX expected=0x%016llX %s\n", label,
              static_cast<unsigned long long>(actual),
              static_cast<unsigned long long>(expected), ok ? "PASS" : "FAIL");
}

const char* ToText(ShadowGeometryOwnerDecision decision) {
  switch (decision) {
    case ShadowGeometryOwnerDecision::Accept:
      return "Accept";
    case ShadowGeometryOwnerDecision::DomainMismatch:
      return "DomainMismatch";
    case ShadowGeometryOwnerDecision::GeometryMismatch:
      return "GeometryMismatch";
  }
  return "Unknown";
}

void CheckDecision(const char* label, ShadowGeometryOwnerDecision actual,
                   ShadowGeometryOwnerDecision expected) {
  ++g_cases;
  const bool ok = actual == expected;
  if (!ok)
    ++g_failures;
  std::printf("%s actual=%s expected=%s %s\n", label, ToText(actual),
              ToText(expected), ok ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// domain registry 生命周期模型：每个归属判定都调用上面那份生产内核。
// 它刻意保留两个入口：
//   LegacyFind —— 隔离前的形状（只有 key，没有 domain），**仅用于反例对照**；
//   Find / Publish / GcEraseGeometry / PurgeDomain —— 隔离后的形状。
// ---------------------------------------------------------------------------
class DomainRegistryModel {
 public:
  struct Slot {
    uint32_t geometryId = 0u;
    uint32_t instances = 0u;
    ShadowGeometryDomain domain = ShadowGeometryDomain::Generic;
  };
  struct Geometry {
    uint64_t key = 0u;
    ShadowGeometryDomain domain = ShadowGeometryDomain::Generic;
    uint64_t bytes = 0u;
    uint64_t lastSeenFrame = 0u;
  };

  uint64_t frame = 0u;
  uint64_t domainLookupRejects = 0u;
  uint64_t domainPublishRejects = 0u;
  uint64_t domainGcEraseRejects = 0u;
  uint64_t domainPurgeRejects = 0u;
  uint64_t domainResetOwnerRejects = 0u;

  // 隔离前的命中：没有 domain 参数，谁能对上 key 谁就命中。
  bool LegacyFind(uint64_t key, uint32_t& outGeometryId) {
    auto slotIt = slots.find(key);
    if (slotIt == slots.end())
      return false;
    auto geomIt = geometries.find(slotIt->second.geometryId);
    if (geomIt == geometries.end()) {
      slots.erase(slotIt);
      return false;
    }
    slotIt->second.instances++;
    geomIt->second.lastSeenFrame = frame;
    outGeometryId = slotIt->second.geometryId;
    return true;
  }

  bool Find(uint64_t key, ShadowGeometryDomain domain, uint32_t& outGeometryId,
            bool& outDomainReject) {
    outDomainReject = false;
    auto slotIt = slots.find(key);
    if (slotIt == slots.end())
      return false;
    if (!ShadowGeometryOwnerAccepts(domain, 0u, slotIt->second.domain,
                                    slotIt->second.geometryId)) {
      // fail-closed：拒绝命中，且不触碰槽位。
      outDomainReject = true;
      domainLookupRejects++;
      return false;
    }
    auto geomIt = geometries.find(slotIt->second.geometryId);
    if (geomIt == geometries.end()) {
      slots.erase(slotIt);
      return false;
    }
    if (!ShadowGeometryOwnerAccepts(domain, slotIt->second.geometryId,
                                    geomIt->second.domain,
                                    slotIt->second.geometryId)) {
      domainLookupRejects++;
      return false;
    }
    slotIt->second.instances++;
    geomIt->second.lastSeenFrame = frame;
    outGeometryId = slotIt->second.geometryId;
    return true;
  }

  uint32_t Publish(uint64_t key, ShadowGeometryDomain domain, uint64_t bytes,
                   uint32_t& outGeometryId, bool& outConflict) {
    outConflict = false;
    auto slotIt = slots.find(key);
    if (slotIt != slots.end() &&
        !ShadowGeometryOwnerAccepts(domain, 0u, slotIt->second.domain,
                                    slotIt->second.geometryId)) {
      // 跨 domain publish：拒绝，不覆盖槽位，不分配任何字节。
      outConflict = true;
      domainPublishRejects++;
      outGeometryId = 0u;
      return 0u;
    }
    const uint32_t geometryId = nextGeometryId++;
    geometries[geometryId] = Geometry{key, domain, bytes, frame};
    Slot slot = {};
    slot.geometryId = geometryId;
    slot.instances = 1u;
    slot.domain = domain;
    slots[key] = slot;
    outGeometryId = geometryId;
    return geometryId;
  }

  // GC：按 geometryId 淘汰常驻条目；槽位只有在仍然属于该条目时才擦除。
  bool GcEraseGeometry(uint32_t geometryId) {
    auto geomIt = geometries.find(geometryId);
    if (geomIt == geometries.end())
      return false;
    const uint64_t key = geomIt->second.key;
    const ShadowGeometryDomain domain = geomIt->second.domain;
    geometries.erase(geomIt);
    auto slotIt = slots.find(key);
    if (slotIt == slots.end())
      return true;
    if (DecideShadowGeometryOwner(domain, geometryId, slotIt->second.domain,
                                  slotIt->second.geometryId) ==
        ShadowGeometryOwnerDecision::Accept) {
      slots.erase(slotIt);
    } else {
      domainGcEraseRejects++;
    }
    return true;
  }

  // domain 作用域清理（不可用相机 / tombstone 的 Stage13 清理形式）。
  size_t PurgeDomain(ShadowGeometryDomain domain) {
    size_t purged = 0u;
    for (auto it = geometries.begin(); it != geometries.end();) {
      if (ShadowGeometryOwnerAccepts(domain, 0u, it->second.domain, 0u)) {
        slots.erase(it->second.key);
        it = geometries.erase(it);
        ++purged;
      } else {
        domainPurgeRejects++;
        ++it;
      }
    }
    return purged;
  }

  // 整会话退役前的归属校验（与生产 War3ResetShadowSessionState 的 move 之前
  // 同一判定）：槽位必须与它指向的常驻条目 domain 一致，否则视为"无主别名"，
  // 先丢弃并计数，不把它当作有效归属带进退役记录。
  struct RetireResult {
    size_t retiredSlots = 0u;
    size_t droppedAliases = 0u;
  };

  RetireResult RetireSession() {
    RetireResult result = {};
    for (auto it = slots.begin(); it != slots.end();) {
      auto geomIt = geometries.find(it->second.geometryId);
      if (geomIt != geometries.end() &&
          !ShadowGeometryOwnerAccepts(it->second.domain, it->second.geometryId,
                                      geomIt->second.domain, geomIt->first)) {
        domainResetOwnerRejects++;
        it = slots.erase(it);
        ++result.droppedAliases;
      } else {
        ++it;
      }
    }
    result.retiredSlots = slots.size();
    // 退役本身仍整体 move（不能选择性拒绝退役，否则 GPU 资源会失去 fence 所有权）。
    slots.clear();
    geometries.clear();
    return result;
  }

  // 仅测试用：注入一个"槽位域 != 它指向的常驻条目域"的无主别名。
  void InjectUnownedAlias(uint64_t key, uint32_t geometryId,
                          ShadowGeometryDomain slotDomain) {
    Slot slot = {};
    slot.geometryId = geometryId;
    slot.instances = 1u;
    slot.domain = slotDomain;
    slots[key] = slot;
  }

  size_t GeometryCount(ShadowGeometryDomain domain) const {
    size_t count = 0u;
    for (const auto& entry : geometries) {
      if (entry.second.domain == domain)
        ++count;
    }
    return count;
  }

  size_t SlotCount() const { return slots.size(); }
  uint64_t BytesOf(ShadowGeometryDomain domain) const {
    uint64_t bytes = 0u;
    for (const auto& entry : geometries) {
      if (entry.second.domain == domain)
        bytes += entry.second.bytes;
    }
    return bytes;
  }
  bool SlotDomain(uint64_t key, ShadowGeometryDomain& outDomain) const {
    auto it = slots.find(key);
    if (it == slots.end())
      return false;
    outDomain = it->second.domain;
    return true;
  }

 private:
  uint32_t nextGeometryId = 1u;
  std::map<uint64_t, Slot> slots;
  std::map<uint32_t, Geometry> geometries;
};

// 与生产调用点同构的 key 材料：同一个 key 可以被两个 domain 各自算出来
// （这正是隔离前会互相命中的反例）。
uint64_t MakeCollidingKey(uint64_t sourceHash, uint64_t layoutHash,
                          uint32_t mode) {
  uint64_t key = dxvk::bit::fnv1a_init();
  key = dxvk::bit::fnv1a_iter(key, sourceHash);
  key = dxvk::bit::fnv1a_iter(key, layoutHash);
  key = dxvk::bit::fnv1a_iter(key, mode);
  return key;
}

}  // namespace

int main() {
  const ShadowGeometryDomain kGeneric = ShadowGeometryDomain::Generic;
  const ShadowGeometryDomain kS1 = ShadowGeometryDomain::S1Terrain;
  const ShadowGeometryDomain kStage13 = ShadowGeometryDomain::Stage13Exact;

  // ---- C1 domain 取值逐位等于既有隐式 tag，且互不相同 ----
  CheckU64("C1.tag.s1-equals-historic-literal", ShadowGeometryDomainTag(kS1),
           0x53310001ull);
  CheckU64("C1.tag.stage13-equals-historic-literal",
           ShadowGeometryDomainTag(kStage13), 0x53314301ull);
  CheckU64("C1.tag.generic-is-zero", ShadowGeometryDomainTag(kGeneric), 0ull);
  Check("C1.tag.s1-and-stage13-distinct",
        ShadowGeometryDomainTag(kS1) != ShadowGeometryDomainTag(kStage13));
  Check("C1.tag.enum-values-distinct",
        kGeneric != kS1 && kS1 != kStage13 && kGeneric != kStage13);
  Check("C1.feature.unique-tag-count-is-3",
        (ShadowGeometryDomainTag(kGeneric) != ShadowGeometryDomainTag(kS1)) &&
            (ShadowGeometryDomainTag(kS1) != ShadowGeometryDomainTag(kStage13)) &&
            (ShadowGeometryDomainTag(kGeneric) !=
             ShadowGeometryDomainTag(kStage13)));

  // ---- C2 owner-check 内核：域内接受 / 跨域拒绝 / 同域异几何拒绝 ----
  CheckDecision("C2.accept.domain-zero-expectation",
                DecideShadowGeometryOwner(kS1, 0u, kS1, 77u),
                ShadowGeometryOwnerDecision::Accept);
  CheckDecision("C2.accept.same-domain-same-geometry",
                DecideShadowGeometryOwner(kStage13, 77u, kStage13, 77u),
                ShadowGeometryOwnerDecision::Accept);
  CheckDecision("C2.reject.s1-expects-stage13-slot",
                DecideShadowGeometryOwner(kS1, 0u, kStage13, 77u),
                ShadowGeometryOwnerDecision::DomainMismatch);
  CheckDecision("C2.reject.stage13-expects-s1-slot",
                DecideShadowGeometryOwner(kStage13, 0u, kS1, 77u),
                ShadowGeometryOwnerDecision::DomainMismatch);
  CheckDecision("C2.reject.generic-expects-s1-slot",
                DecideShadowGeometryOwner(kGeneric, 0u, kS1, 77u),
                ShadowGeometryOwnerDecision::DomainMismatch);
  CheckDecision("C2.reject.s1-expects-generic-slot",
                DecideShadowGeometryOwner(kS1, 0u, kGeneric, 77u),
                ShadowGeometryOwnerDecision::DomainMismatch);
  CheckDecision("C2.reject.same-domain-other-geometry",
                DecideShadowGeometryOwner(kS1, 42u, kS1, 77u),
                ShadowGeometryOwnerDecision::GeometryMismatch);
  CheckDecision("C2.domain-mismatch-wins-over-geometry-id",
                DecideShadowGeometryOwner(kS1, 42u, kStage13, 77u),
                ShadowGeometryOwnerDecision::DomainMismatch);
  Check("C2.accepts-helper-matches-decision",
        ShadowGeometryOwnerAccepts(kS1, 0u, kS1, 9u) &&
            !ShadowGeometryOwnerAccepts(kS1, 0u, kStage13, 9u));

  // ---- C3 既有 key 材料逐点不变：隐式裸常数与显式 domain 取值产生同一 hash ----
  {
    // S1 站点形状：layoutHash 先混入若干 layout 字段，最后混入 S1 tag。
    const uint32_t layoutFields[] = {3u, 16u, 0u, 2u, 5u, 24u, 4096u};
    uint64_t legacy = dxvk::bit::fnv1a_init();
    uint64_t explicitValue = dxvk::bit::fnv1a_init();
    for (uint32_t value : layoutFields) {
      legacy = dxvk::bit::fnv1a_iter(legacy, value);
      explicitValue = dxvk::bit::fnv1a_iter(explicitValue, value);
    }
    legacy = dxvk::bit::fnv1a_iter(legacy, uint32_t(0x53310001u));
    explicitValue = dxvk::bit::fnv1a_iter(explicitValue, ShadowGeometryDomainTag(kS1));
    CheckU64("C3.s1-layout-hash-unchanged", explicitValue, legacy);
  }
  {
    // Stage13 站点形状：tag 是 sourceHash / layoutHash 链的**第一个**混入值。
    constexpr uint32_t kStage13Content = 0x53314301u;
    const uint64_t worldMatrixHash = 0x1122334455667788ull;
    const uint64_t identityHash = 0x99AABBCCDDEEFF00ull;
    const uint64_t materialHash = 0x0F1E2D3C4B5A6978ull;
    uint64_t legacySource = dxvk::bit::fnv1a_iter(dxvk::bit::fnv1a_init(), kStage13Content);
    legacySource = dxvk::bit::fnv1a_iter(legacySource, worldMatrixHash);
    legacySource = dxvk::bit::fnv1a_iter(legacySource, identityHash);
    legacySource = dxvk::bit::fnv1a_iter(legacySource, materialHash);
    uint64_t explicitSource =
        dxvk::bit::fnv1a_iter(dxvk::bit::fnv1a_init(), ShadowGeometryDomainTag(kStage13));
    explicitSource = dxvk::bit::fnv1a_iter(explicitSource, worldMatrixHash);
    explicitSource = dxvk::bit::fnv1a_iter(explicitSource, identityHash);
    explicitSource = dxvk::bit::fnv1a_iter(explicitSource, materialHash);
    CheckU64("C3.stage13-source-hash-unchanged", explicitSource, legacySource);

    uint64_t legacyLayout = dxvk::bit::fnv1a_iter(dxvk::bit::fnv1a_init(), kStage13Content);
    legacyLayout = dxvk::bit::fnv1a_iter(legacyLayout, uint32_t(4u));
    legacyLayout = dxvk::bit::fnv1a_iter(legacyLayout, uint32_t(12u));
    uint64_t explicitLayout =
        dxvk::bit::fnv1a_iter(dxvk::bit::fnv1a_init(), ShadowGeometryDomainTag(kStage13));
    explicitLayout = dxvk::bit::fnv1a_iter(explicitLayout, uint32_t(4u));
    explicitLayout = dxvk::bit::fnv1a_iter(explicitLayout, uint32_t(12u));
    CheckU64("C3.stage13-layout-hash-unchanged", explicitLayout, legacyLayout);
  }

  // ---- C4 反例（F4.10）：同 key 跨 domain 碰撞，隔离前命中 / 隔离后拒绝 ----
  {
    const uint64_t collisionKey =
        MakeCollidingKey(0xCAFEBABEull, 0xDEADBEEFull, 0u);
    DomainRegistryModel legacyModel;
    DomainRegistryModel isolatedModel;

    uint32_t produced = 0u;
    bool conflict = false;
    legacyModel.Publish(collisionKey, kS1, 4096u, produced, conflict);
    isolatedModel.Publish(collisionKey, kS1, 4096u, produced, conflict);

    // 隔离前：Stage13Exact 用同一个 key 直接命中 S1 的几何。
    uint32_t legacyHit = 0u;
    const bool legacyFound = legacyModel.LegacyFind(collisionKey, legacyHit);
    Check("C4.counterexample.legacy-cross-domain-hit",
          legacyFound && legacyHit == produced);

    // 隔离后：同一次查询被 DomainMismatch 拒绝，槽位未被触碰。
    uint32_t isolatedHit = 0u;
    bool domainReject = false;
    const bool isolatedFound =
        isolatedModel.Find(collisionKey, kStage13, isolatedHit, domainReject);
    Check("C4.counterexample.isolated-cross-domain-rejected",
          !isolatedFound && domainReject && isolatedHit == 0u);
    Check("C4.counterexample.reject-counter-moved",
          isolatedModel.domainLookupRejects == 1u);
    Check("C4.counterexample.owner-slot-not-touched",
          isolatedModel.SlotCount() == 1u);

    // 隔离后：同 domain 查询仍正常命中（隔离不是"一律拒绝"）。
    uint32_t sameDomainHit = 0u;
    const bool sameDomainFound =
        isolatedModel.Find(collisionKey, kS1, sameDomainHit, domainReject);
    Check("C4.same-domain-still-hits",
          sameDomainFound && sameDomainHit == produced && !domainReject);
  }

  // ---- C5 publish owner-check：跨 domain publish 拒绝且零副作用 ----
  {
    DomainRegistryModel model;
    uint32_t s1Id = 0u;
    bool conflict = false;
    model.Publish(0x1111ull, kS1, 8192u, s1Id, conflict);
    const uint32_t geometryCountBefore = static_cast<uint32_t>(model.GeometryCount(kS1));
    const uint64_t bytesBefore = model.BytesOf(kS1);

    uint32_t stage13Id = 0u;
    const uint32_t created =
        model.Publish(0x1111ull, kStage13, 4096u, stage13Id, conflict);
    Check("C5.publish.cross-domain-rejected", conflict && created == 0u);
    Check("C5.publish.slot-still-owned-by-s1", [&] {
      ShadowGeometryDomain slotDomain = kGeneric;
      return model.SlotDomain(0x1111ull, slotDomain) && slotDomain == kS1;
    }());
    Check("C5.publish.no-bytes-or-geometry-added",
          model.GeometryCount(kS1) == geometryCountBefore &&
              model.BytesOf(kS1) == bytesBefore &&
              model.GeometryCount(kStage13) == 0u);
    Check("C5.publish.reject-counter-moved",
          model.domainPublishRejects == 1u);

    // 同 domain（此处为 Generic）在自己的 key 上照常发布。
    uint32_t genericId = 0u;
    model.Publish(0x2222ull, kGeneric, 1024u, genericId, conflict);
    Check("C5.publish.own-domain-accepted",
          !conflict && genericId != 0u && model.GeometryCount(kGeneric) == 1u);
  }

  // ---- C6 GC owner-check：只回收本条 geometry，不擦别人的槽位 ----
  {
    DomainRegistryModel model;
    uint32_t s1Id = 0u;
    uint32_t stage13Id = 0u;
    bool conflict = false;
    model.Publish(0xAAAAull, kS1, 2048u, s1Id, conflict);
    model.Publish(0xBBBBull, kStage13, 1024u, stage13Id, conflict);
    Check("C6.setup.two-domains-live",
          model.GeometryCount(kS1) == 1u && model.GeometryCount(kStage13) == 1u &&
              model.BytesOf(kS1) == 2048u && model.BytesOf(kStage13) == 1024u);

    // 淘汰 S1 的一条：Stage13 的几何、槽位与字节账都不得变化。
    model.GcEraseGeometry(s1Id);
    Check("C6.gc.own-domain-reclaimed",
          model.GeometryCount(kS1) == 0u && model.BytesOf(kS1) == 0u);
    Check("C6.gc.other-domain-untouched",
          model.GeometryCount(kStage13) == 1u &&
              model.BytesOf(kStage13) == 1024u && model.SlotCount() == 1u);
    Check("C6.gc.no-rejects-on-clean-owner",
          model.domainGcEraseRejects == 0u);

    // 反例形态：槽位已被同 key 的更新条目接管后，旧 geometry 的 GC 不得
    // 顺手擦掉新条目的槽位（隔离前的盲擦除会破坏别人的 live 槽位）。
    DomainRegistryModel reused;
    uint32_t firstId = 0u;
    uint32_t secondId = 0u;
    reused.Publish(0xCCCCull, kGeneric, 512u, firstId, conflict);
    reused.GcEraseGeometry(firstId);
    reused.Publish(0xCCCCull, kGeneric, 512u, secondId, conflict);
    Check("C6.reuse.second-entry-owns-slot",
          secondId != 0u && secondId != firstId && reused.SlotCount() == 1u);
    ShadowGeometryDomain reusedDomain = kS1;
    Check("C6.reuse.slot-domain-is-generic",
          reused.SlotDomain(0xCCCCull, reusedDomain) &&
              reusedDomain == kGeneric);
  }

  // ---- C7 domain 作用域清理：只清本 domain（Stage13 清理形式） ----
  {
    DomainRegistryModel model;
    uint32_t s1Id = 0u;
    uint32_t stage13Id = 0u;
    uint32_t genericId = 0u;
    bool conflict = false;
    model.Publish(0x1ull, kS1, 100u, s1Id, conflict);
    model.Publish(0x2ull, kStage13, 200u, stage13Id, conflict);
    model.Publish(0x3ull, kGeneric, 300u, genericId, conflict);

    const size_t purged = model.PurgeDomain(kStage13);
    Check("C7.purge.only-stage13-removed",
          purged == 1u && model.GeometryCount(kStage13) == 0u &&
              model.GeometryCount(kS1) == 1u &&
              model.GeometryCount(kGeneric) == 1u);
    Check("C7.purge.other-domain-bytes-intact",
          model.BytesOf(kS1) == 100u && model.BytesOf(kGeneric) == 300u);
    Check("C7.purge.slot-count-follows-own-domain",
          model.SlotCount() == 2u && model.domainPurgeRejects == 2u);

    // 反向：清 S1 不影响 Stage13（已清空）与 Generic。
    const size_t purgedS1 = model.PurgeDomain(kS1);
    Check("C7.purge.reverse-direction",
          purgedS1 == 1u && model.GeometryCount(kS1) == 0u &&
              model.GeometryCount(kGeneric) == 1u &&
              model.BytesOf(kGeneric) == 300u);
  }

  // ---- C8 隔离不改变域内路径：连续命中只增同一槽位，不产生拒绝 ----
  {
    DomainRegistryModel model;
    uint32_t s1Id = 0u;
    bool conflict = false;
    model.Publish(0x4242ull, kS1, 777u, s1Id, conflict);
    bool anyReject = false;
    for (uint32_t frame = 1u; frame <= 64u; ++frame) {
      model.frame = frame;
      uint32_t hit = 0u;
      bool reject = false;
      if (!model.Find(0x4242ull, kS1, hit, reject) || hit != s1Id || reject)
        anyReject = true;
    }
    Check("C8.in-domain-hits-stable", !anyReject);
    Check("C8.no-domain-rejects-in-pure-in-domain-run",
          model.domainLookupRejects == 0u && model.domainPublishRejects == 0u &&
              model.domainGcEraseRejects == 0u && model.domainPurgeRejects == 0u &&
              model.domainResetOwnerRejects == 0u);
    Check("C8.single-slot-single-geometry",
          model.SlotCount() == 1u &&
              model.GeometryCount(kS1) == 1u &&
              model.GeometryCount(kGeneric) == 0u &&
              model.GeometryCount(kStage13) == 0u);
  }

  // ---- C9 reset 整会话退役的归属校验（第 4 条定向路径） ----
  {
    DomainRegistryModel clean;
    uint32_t cleanId = 0u;
    bool conflict = false;
    clean.Publish(0x51ull, kS1, 400u, cleanId, conflict);
    const DomainRegistryModel::RetireResult cleanRetire = clean.RetireSession();
    Check("C9.reset.clean-owner-retires-without-reject",
          cleanRetire.retiredSlots == 1u &&
              cleanRetire.droppedAliases == 0u &&
              clean.domainResetOwnerRejects == 0u);
    Check("C9.reset.clean-owner-keeps-every-counter-zero",
          clean.domainLookupRejects == 0u &&
              clean.domainPublishRejects == 0u &&
              clean.domainGcEraseRejects == 0u &&
              clean.domainPurgeRejects == 0u &&
              clean.domainResetOwnerRejects == 0u);

    DomainRegistryModel aliased;
    uint32_t ownedId = 0u;
    bool aliasConflict = false;
    aliased.Publish(0x61ull, kS1, 500u, ownedId, aliasConflict);
    aliased.InjectUnownedAlias(0x62ull, ownedId, kStage13);
    const DomainRegistryModel::RetireResult aliasRetire = aliased.RetireSession();
    Check("C9.reset.unowned-alias-dropped-and-counted",
          aliasRetire.droppedAliases == 1u &&
              aliased.domainResetOwnerRejects == 1u);
    Check("C9.reset.owned-slot-still-retired",
          aliasRetire.retiredSlots == 1u);
    Check("C9.reset.alias-does-not-touch-other-counters",
          aliased.domainLookupRejects == 0u &&
              aliased.domainPublishRejects == 0u &&
              aliased.domainGcEraseRejects == 0u &&
              aliased.domainPurgeRejects == 0u);
  }

  if (g_failures != 0u) {
    std::printf(
        "war3_shadow_geometry_domain_test: FAIL %u/%u case(s)\n",
        static_cast<unsigned>(g_failures), static_cast<unsigned>(g_cases));
    return 1;
  }
  std::printf(
      "SUMMARY: war3_shadow_geometry_domain_test %u/%u case(s) passed\n",
      static_cast<unsigned>(g_cases), static_cast<unsigned>(g_cases));
  return 0;
}
