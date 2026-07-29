#pragma once

#include "../../d3d9_device.h"
#include <array>
#include <d3d9.h>
#include <mutex>


namespace dxvk {

class D3D9DeviceEx;

namespace war3 {
namespace reimpl {

/**
 * War3TeamColorManager - 管理Team Color纹理数组
 *
 * War3使用ReplaceableId=1来标记Team Color材质槽。
 * 原始实现：每个玩家有独立的TeamColor纹理(TeamColor00.blp到TeamColor23.blp)
 *
 * 优化实现：将24个TeamColor纹理合并为一个Texture2DArray，
 * 允许不同阵营的单位使用同一个DrawCall渲染。
 */
class War3TeamColorManager {
public:
  static War3TeamColorManager &Get();

  // 初始化 - 在D3D Device创建后调用
  HRESULT Initialize(D3D9DeviceEx *device);

  // 关闭 - 释放资源
  void Shutdown();

  // 注册TeamColor纹理 (Hook纹理加载时调用)
  // index: 玩家索引 (0-23)
  // texture: 原始TeamColor纹理
  void RegisterTeamColor(uint32_t index, IDirect3DTexture9 *texture);

  // 构建Texture2DArray (在所有TeamColor注册后调用)
  HRESULT BuildTextureArray();

  // 获取合并后的Texture Array (用于绑定到Shader)
  IDirect3DTexture9 *GetTextureArray() const { return m_textureArray; }

  // 是否就绪
  bool IsReady() const { return m_isReady; }

  // 获取TeamColor数量
  uint32_t GetTeamColorCount() const { return m_registeredCount; }

  // 根据原始纹理指针获取索引 (用于确定单位的TeamColorIndex)
  int32_t GetIndexFromTexture(IDirect3DBaseTexture9 *texture) const;

  // 设备丢失/重置处理
  void OnLostDevice();
  void OnResetDevice();

private:
  War3TeamColorManager() = default;
  ~War3TeamColorManager();

  // 禁止拷贝
  War3TeamColorManager(const War3TeamColorManager &) = delete;
  War3TeamColorManager &operator=(const War3TeamColorManager &) = delete;

  D3D9DeviceEx *m_device = nullptr;

  // 原始TeamColor纹理 (弱引用，不AddRef)
  static constexpr uint32_t kMaxTeamColors = 24;
  std::array<IDirect3DTexture9 *, kMaxTeamColors> m_originalTextures = {};
  uint32_t m_registeredCount = 0;

  // 合并后的Texture2DArray
  // 注意: D3D9原生不支持Texture2DArray，但DXVK可以模拟
  // 我们使用一个高度为 (TeamColorCount * TextureHeight) 的2D纹理来模拟
  IDirect3DTexture9 *m_textureArray = nullptr;

  // 每个TeamColor纹理的尺寸 (假设所有TeamColor尺寸相同)
  uint32_t m_textureWidth = 0;
  uint32_t m_textureHeight = 0;
  D3DFORMAT m_textureFormat = D3DFMT_UNKNOWN;

  bool m_isReady = false;
  // mutable：GetIndexFromTexture 是 const 访问器，但仍需与写入 m_originalTextures
  // 的姐妹方法(Initialize/RegisterTeamColor/BuildTextureArray/OnLostDevice)共享
  // 同一把锁；否则 const 读取无法加锁，形成加载线程写入 vs 渲染线程查询的数据竞争。
  mutable std::mutex m_mutex;
};

} // namespace reimpl
} // namespace war3
} // namespace dxvk
