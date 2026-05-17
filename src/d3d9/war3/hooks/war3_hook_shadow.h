#pragma once

#include <windows.h>
#include <cstdint>

namespace dxvk::war3::hooks {

/**
 * @brief RegisterImage 入口的来源分类。
 *
 * 说明：
 * - 来源由 RegisterImage 调用点“返回地址”精确识别；
 * - `Unknown` 表示未命中已知返回点。
 */
enum class ShadowRegisterSource : uint8_t {
  Unknown = 0,
  StaticStamp,
  EmitterStamp,
  SelectionCircleColorFriend,
  MarkColorOcclusion,
  WithParams,
  ObjectBridge,
  FromPoint,
  FromTwoPoints,
};

/**
 * @brief RegisterImage owner 对象类别。
 */
enum class ShadowOwnerKind : uint8_t {
  Unknown = 0,
  Unit,
  Building,
  Destructible,
  Item,
};

/**
 * @brief RegisterImage 决策输入上下文。
 */
struct ShadowRegisterContext {
  uint32_t mode = 0;
  ShadowRegisterSource source = ShadowRegisterSource::Unknown;
  ShadowOwnerKind ownerKind = ShadowOwnerKind::Unknown;
  uint32_t ownerRawcode = 0;
  uint32_t retRva = 0;
  int argType = 0;
  bool hasKey = false;
  const char* key = nullptr;
};

/**
 * @brief RegisterImage 决策结果。
 */
struct ShadowRegisterDecision {
  bool blocked = false;
  const char* reason = "PassThrough";
};

/**
 * @brief 阴影域 Hook 地址集合。
 *
 * 该结构用于集中传递阴影链路各关键入口地址，
 * 避免在安装函数中散落硬编码地址。
 */
struct ShadowHookAddresses {
  /** @brief `Terrain_RenderShadowLayer` 函数地址。 */
  LPVOID terrainShadowLayerAddr = nullptr;
  /** @brief `TerrainShadow_RenderListA` 函数地址。 */
  LPVOID terrainRenderListAAddr = nullptr;
  /** @brief `TerrainShadow_RenderListB` 函数地址。 */
  LPVOID terrainRenderListBAddr = nullptr;
  /** @brief `ShadowUpdate_WriteEntry` 函数地址。 */
  LPVOID shadowUpdateWriteEntryAddr = nullptr;
  /** @brief `ShadowProjector_Add_Simple` 函数地址。 */
  LPVOID shadowProjectorAddSimpleAddr = nullptr;
  /** @brief `ShadowProjector_Add_FromObject` 函数地址。 */
  LPVOID shadowProjectorAddFromObjectAddr = nullptr;
  /** @brief `TerrainShadow_RegisterImageEntry` 函数地址。 */
  LPVOID shadowRegisterImageEntryAddr = nullptr;
  /** @brief `Add_Simple` 的桥接来源地址（用于来源识别）。 */
  LPVOID shadowProjectorSimpleBridgeAddr = nullptr;
  /** @brief Runtime 对象投影路径地址（用于来源识别）。 */
  LPVOID shadowPathObjectProjectorRuntimeAddr = nullptr;
  /** @brief JassBridge 对象投影路径地址（用于来源识别）。 */
  LPVOID shadowPathObjectProjectorJassBridgeAddr = nullptr;
  /** @brief ShadowPath_StaticStamp_Toggle（静态 stamp 直写链路）。 */
  LPVOID shadowPathStaticStampToggleAddr = nullptr;

  // RegisterImage 调用点返回地址（用于精确来源判定）
  LPVOID shadowRegisterRetWithParamsAddr = nullptr;            // 0x7291DC
  LPVOID shadowRegisterRetSelectionCircleAddr = nullptr;       // 0x74DAB6
  LPVOID shadowRegisterRetStaticStampAddr = nullptr;           // 0x74DBFA
  LPVOID shadowRegisterRetEmitterStampAddr = nullptr;          // 0x74DF55
  LPVOID shadowRegisterRetObjectBridgeAddr = nullptr;          // 0x76D44A
  LPVOID shadowRegisterRetMarkOcclusionAddr = nullptr;         // 0x76D5A4
  LPVOID shadowRegisterRetFromPointAddr = nullptr;             // 0x76D69A
  LPVOID shadowRegisterRetFromTwoPointsAddr = nullptr;         // 0x76D719

  /** @brief Phase 7.100: TerrainShadow_WriteMaskRegion (静态阴影治理方案 A)。
   * 30+ 写入路径的最终汇聚点，hook 后按 maskIdx==3 拒绝可干净屏蔽建筑/装饰物
   * 静态阴影，不影响 fog/LOS/path。详见
   * docs/plan/overnight_render_paper_2026_05_15/06_fogmask_static_shadow.md §7.1。 */
  LPVOID terrainShadowWriteMaskRegionAddr = nullptr;           // 0x234710

  /** @brief Phase 7.116: TerrainShadow_DispatchToShape - 建筑/装饰物/可破坏物
   * 静态阴影 footprint 写入的唯一汇聚点。内部走 BoxFastpath/PolyFastpath
   * 直接修改 mask grid，与 fog/LOS/path/visibility（走 WriteMaskRegion）完全独立。
   * 5 个 caller 全部是 shadow path，hook 入口直接 return 0 即可干净屏蔽
   * 所有建筑/装饰物/可破坏物 footprint shadow，不影响其他 mask 写入。 */
  LPVOID terrainShadowDispatchToShapeAddr = nullptr;           // 0x234420
};

/**
 * @brief 安装阴影域 Hook。
 * @param addrs 阴影域地址集合。
 * @return 至少安装一个 Hook 返回 `true`，否则返回 `false`。
 *
 * @warning 调用方必须确保地址已完成版本校验与可执行性检查。
 */
bool InstallShadowHooks(const ShadowHookAddresses &addrs);

// Phase 7.100：WriteMaskRegion 诊断计数器（control plane 透传用）。
uint64_t QueryWriteMaskRegionEnterCount();
uint64_t QueryWriteMaskRegionRejectedIdx3Count();
uint64_t QueryWriteMaskRegionPassFogCount();
uint64_t QueryWriteMaskRegionPassLosCount();
uint64_t QueryWriteMaskRegionPassPathCount();
uint64_t QueryWriteMaskRegionPassOtherCount();

// Phase 7.112：caller-aware 静态阴影屏蔽诊断（control plane 透传）。
uint64_t QueryWriteMaskRegionFromBuildingStampCount();
uint64_t QueryWriteMaskRegionRejectedBuildingCount();
uint64_t QueryWriteMaskRegionFromRegisterFootprintCount();
uint64_t QueryWriteMaskRegionFromRebuildMaskCount();
uint64_t QueryWriteMaskRegionFromActorRuntimeCount();
uint64_t QueryWriteMaskRegionFromForObjectCount();
uint64_t QueryWriteMaskRegionFromOtherCallerCount();

// Phase 7.116：DispatchToShape (建筑/装饰物/可破坏物 shadow footprint) 诊断。
uint64_t QueryDispatchToShapeEnterCount();
uint64_t QueryDispatchToShapeRejectedCount();
uint64_t QueryDispatchToShapeFromRebuildMaskCount();
uint64_t QueryDispatchToShapeFromShadowSetupCount();
uint64_t QueryDispatchToShapeFromOtherCallerCount();

// Phase 7.108：ShadowProjector 诊断计数器（control plane 透传用）。
// 这条路径是 Game.dll 自己的"投影器阴影"系统（CTerrainUberSplats），
// 与 D3D9 mesh draw 完全独立——path blocker 视觉残留可能就来自这条
// 路径，但当前 stats 只在 Verbose/Stats 编译期开关下计数，关闭时计数为 0。
// 这里把它升级为永久 atomic，方便实测分辨究竟哪条路径在画 path blocker。
uint64_t QueryShadowProjectorAddFromObjectEnterCount();
uint64_t QueryShadowProjectorAddFromObjectBlockedCount();
uint64_t QueryShadowProjectorAddFromObjectFourCCExtractedCount();
uint64_t QueryShadowProjectorAddFromObjectFourCCMissCount();
uint64_t QueryShadowProjectorAddFromObjectBlockedFourCCCount();
uint64_t QueryShadowProjectorAddSimpleEnterCount();
uint64_t QueryShadowProjectorAddSimpleBlockedCount();
// 最近被 reject 的 fourcc（环形采样，前 8 个 unique）。
uint32_t QueryShadowProjectorBlockedFourCCSampleAt(uint32_t idx);
uint32_t QueryShadowProjectorObservedFourCCSampleAt(uint32_t idx);

} // namespace dxvk::war3::hooks
