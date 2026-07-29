#include "war3_team_color_manager.h"
#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"

namespace dxvk {
namespace war3 {
namespace reimpl {

War3TeamColorManager &War3TeamColorManager::Get() {
  static War3TeamColorManager s_instance;
  return s_instance;
}

War3TeamColorManager::~War3TeamColorManager() { Shutdown(); }

HRESULT War3TeamColorManager::Initialize(D3D9DeviceEx *device) {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_device == device && m_isReady)
    return S_OK;

  m_device = device;
  m_registeredCount = 0;
  m_isReady = false;

  // 清空原始纹理引用
  for (auto &tex : m_originalTextures) {
    tex = nullptr;
  }

  WAR3_RENDER_LOG("War3TeamColorManager: Initialized\n");
  return S_OK;
}

void War3TeamColorManager::Shutdown() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_textureArray) {
    m_textureArray->Release();
    m_textureArray = nullptr;
  }

  for (auto &tex : m_originalTextures) {
    tex = nullptr; // 弱引用，不释放
  }

  m_registeredCount = 0;
  m_isReady = false;
  m_device = nullptr;

  WAR3_RENDER_LOG("War3TeamColorManager: Shutdown\n");
}

void War3TeamColorManager::RegisterTeamColor(uint32_t index,
                                             IDirect3DTexture9 *texture) {
  if (index >= kMaxTeamColors || !texture)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);

  // 如果已经注册过，跳过
  if (m_originalTextures[index] != nullptr)
    return;

  m_originalTextures[index] = texture;
  m_registeredCount++;

  // 获取纹理尺寸信息 (假设所有TeamColor尺寸相同)
  if (m_textureWidth == 0) {
    D3DSURFACE_DESC desc;
    if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
      m_textureWidth = desc.Width;
      m_textureHeight = desc.Height;
      m_textureFormat = desc.Format;
    }
  }

  WAR3_RENDER_LOG("War3TeamColorManager: Registered TeamColor[%u], total=%u\n",
                  index, m_registeredCount);
}

HRESULT War3TeamColorManager::BuildTextureArray() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (!m_device || m_registeredCount == 0)
    return E_FAIL;

  if (m_textureArray) {
    m_textureArray->Release();
    m_textureArray = nullptr;
  }

  // D3D9不原生支持Texture2DArray，我们使用一个高纹理模拟
  // 高度 = TeamColorCount * SingleTextureHeight
  // Shader中通过 uv.y = (index + frac_y) / TeamColorCount 来采样

  uint32_t arrayHeight = m_registeredCount * m_textureHeight;

  HRESULT hr = m_device->CreateTexture(
      m_textureWidth, arrayHeight, 1, D3DUSAGE_DYNAMIC, m_textureFormat,
      D3DPOOL_DEFAULT, &m_textureArray, nullptr);

  if (FAILED(hr)) {
    WAR3_RENDER_LOG(
        "War3TeamColorManager: Failed to create texture array %ux%u\n",
        m_textureWidth, arrayHeight);
    return hr;
  }

  // 复制每个TeamColor到对应的slice位置
  D3DLOCKED_RECT destRect;
  hr = m_textureArray->LockRect(0, &destRect, nullptr, D3DLOCK_DISCARD);
  if (FAILED(hr)) {
    WAR3_RENDER_LOG("War3TeamColorManager: Failed to lock texture array\n");
    return hr;
  }

  for (uint32_t i = 0; i < kMaxTeamColors; i++) {
    if (!m_originalTextures[i])
      continue;

    D3DLOCKED_RECT srcRect;
    if (SUCCEEDED(m_originalTextures[i]->LockRect(0, &srcRect, nullptr,
                                                  D3DLOCK_READONLY))) {
      // 计算目标位置
      uint32_t destY = i * m_textureHeight;
      uint8_t *destPtr =
          static_cast<uint8_t *>(destRect.pBits) + destY * destRect.Pitch;

      // 复制行
      for (uint32_t row = 0; row < m_textureHeight; row++) {
        uint8_t *src =
            static_cast<uint8_t *>(srcRect.pBits) + row * srcRect.Pitch;
        uint8_t *dst = destPtr + row * destRect.Pitch;
        memcpy(dst, src, m_textureWidth * 4); // 假设4字节/像素
      }

      m_originalTextures[i]->UnlockRect(0);
    }
  }

  m_textureArray->UnlockRect(0);

  m_isReady = true;
  WAR3_RENDER_LOG("War3TeamColorManager: Built texture array %ux%u (%u "
                  "TeamColors)\n",
                  m_textureWidth, arrayHeight, m_registeredCount);

  return S_OK;
}

int32_t War3TeamColorManager::GetIndexFromTexture(
    IDirect3DBaseTexture9 *texture) const {
  if (!texture)
    return -1;

  // 与所有姐妹方法一致：读取共享的 m_originalTextures 前先取 m_mutex，避免加载线程
  // 写入/清空与查询线程之间的数据竞争。m_mutex 已改为 mutable 以支持此 const 访问器。
  std::lock_guard<std::mutex> lock(m_mutex);
  for (uint32_t i = 0; i < kMaxTeamColors; i++) {
    if (m_originalTextures[i] == texture) {
      return static_cast<int32_t>(i);
    }
  }

  return -1;
}

void War3TeamColorManager::OnLostDevice() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_textureArray) {
    m_textureArray->Release();
    m_textureArray = nullptr;
  }

  m_isReady = false;
  WAR3_RENDER_LOG("War3TeamColorManager: OnLostDevice\n");
}

void War3TeamColorManager::OnResetDevice() {
  // 重新构建纹理数组
  if (m_registeredCount > 0) {
    BuildTextureArray();
  }
  WAR3_RENDER_LOG("War3TeamColorManager: OnResetDevice\n");
}

} // namespace reimpl
} // namespace war3
} // namespace dxvk
