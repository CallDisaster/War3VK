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
};

/**
 * @brief 安装阴影域 Hook。
 * @param addrs 阴影域地址集合。
 * @return 至少安装一个 Hook 返回 `true`，否则返回 `false`。
 *
 * @warning 调用方必须确保地址已完成版本校验与可执行性检查。
 */
bool InstallShadowHooks(const ShadowHookAddresses &addrs);

} // namespace dxvk::war3::hooks
