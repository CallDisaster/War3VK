#pragma once
#include <cstdint>

// 2026-09-18 主线程裁定（T7 批次 4 / U5：Registry domain 隔离，形态 (a)）。
//
// 本头是 shadow persistent-geometry registry 的 **domain 唯一显式表达** 与
// **owner-check 判定内核**。生产侧（d3d9_device.h / d3d9_device.cpp）与宿主机边界
// 测试（war3_shadow_geometry_domain_test.cpp）包含同一份实现，因此
// "域内命中 / 跨域拒绝 / 归属校验"不是两套各写一遍的镜像代码。
//
// 取值沿用既有**隐式** tag 的语义，数值逐位不变：
//   0x53310001 = S1 地形 persistent geometry（历史上以裸常数写进 layoutHash 的 S1 tag）
//   0x53314301 = Stage13 static-retention 常驻条目（历史上
//                buildStage13RetentionKey 的 kStage13ReferencedContentTag）
// 其余 registry 调用方（Semantic append / UpperLayer / 主 world capture 的非 S1 分支）
// 归入 Generic。
//
// 纪律（本块只做隔离，不做策略变更）：
//   * 不引入新 env，不改任何准入 / 发布默认值，不改 cap / age / 帧数默认；
//   * Generic == 0 只是"未打 domain 标签的共享 registry 调用方"这一既有事实的
//     显式写法，**本身不是拒绝理由**：只有 domain 归属不一致才拒绝；
//   * 任何归属不一致一律 fail-closed：拒绝命中 / 拒绝发布 / 拒绝擦除，
//     并且**不触碰**对方的槽位与字节账。

namespace dxvk::war3::shadow {

// registry 条目的具名 domain。显式字段取代"魔法数散落在调用点"的写法。
enum class ShadowGeometryDomain : uint32_t {
  Generic = 0u,
  S1Terrain = 0x53310001u,
  Stage13Exact = 0x53314301u,
};

// 既有隐式 tag 的唯一来源。调用点必须经由此函数取常数，不得再写裸 magic number。
constexpr uint32_t ShadowGeometryDomainTag(ShadowGeometryDomain domain) {
  return static_cast<uint32_t>(domain);
}

// domain 分账数组的维度（Generic / S1Terrain / Stage13Exact）。
constexpr uint32_t kShadowGeometryDomainCount = 3u;

constexpr uint32_t ShadowGeometryDomainIndex(ShadowGeometryDomain domain) {
  switch (domain) {
    case ShadowGeometryDomain::Generic:
      return 0u;
    case ShadowGeometryDomain::S1Terrain:
      return 1u;
    case ShadowGeometryDomain::Stage13Exact:
      return 2u;
  }
  return 0u;
}

// owner-check 判定结果。Accept 之外的任何取值都必须让调用方放弃本次操作。
enum class ShadowGeometryOwnerDecision : uint8_t {
  Accept = 0,
  // 槽位属于另一个 domain：跨 domain 命中，一律拒绝。
  DomainMismatch = 1,
  // domain 相同，但操作方指名了另一条 geometry（GC / 退役按 geometryId 归属）。
  GeometryMismatch = 2,
};

// 纯判定：操作方 (expectedDomain, expectedGeometryId) 是否有权触碰
// 槽位 (entryDomain, entryGeometryId)。
//
//   * expectedGeometryId == 0 表示"只做 domain 归属校验"（lookup / domain 作用域 clear）；
//   * domain 不一致优先于 geometryId 不一致（跨域必须报 DomainMismatch，
//     以免把跨域命中误读成普通的 stale-slot 清理）。
inline ShadowGeometryOwnerDecision DecideShadowGeometryOwner(
    ShadowGeometryDomain expectedDomain, uint32_t expectedGeometryId,
    ShadowGeometryDomain entryDomain, uint32_t entryGeometryId) {
  if (expectedDomain != entryDomain)
    return ShadowGeometryOwnerDecision::DomainMismatch;
  if (expectedGeometryId != 0u && expectedGeometryId != entryGeometryId)
    return ShadowGeometryOwnerDecision::GeometryMismatch;
  return ShadowGeometryOwnerDecision::Accept;
}

inline bool ShadowGeometryOwnerAccepts(
    ShadowGeometryDomain expectedDomain, uint32_t expectedGeometryId,
    ShadowGeometryDomain entryDomain, uint32_t entryGeometryId) {
  return DecideShadowGeometryOwner(expectedDomain, expectedGeometryId,
                                   entryDomain,
                                   entryGeometryId) ==
         ShadowGeometryOwnerDecision::Accept;
}

} // namespace dxvk::war3::shadow
