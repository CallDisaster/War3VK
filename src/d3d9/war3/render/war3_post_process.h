#pragma once

#include "../../d3d9_util.h"
#include "../../d3d9_war3_settings.h"

#include <array>
#include <string>

namespace dxvk {

  /**
   * @brief War3 后处理：Bloom + ACES ToneMapping（D3D9 版本）
   *
   * 说明：
   * - 在 UI 绘制前对世界画面做后处理，避免影响原生 UI。
   * - 使用 D3D9 渲染目标链实现双重过滤 Bloom。
   */
  class War3PostProcess {
  public:
    explicit War3PostProcess(IDirect3DDevice9* device);

    void Init(uint32_t width, uint32_t height, D3DFORMAT format);
    void Shutdown();
    void OnResize(uint32_t width, uint32_t height, D3DFORMAT format);

    // 从源纹理进行后处理并输出到目标 Surface
    bool Apply(IDirect3DTexture9* srcTexture, IDirect3DSurface9* dstSurface, const War3PostFxSettings& settings);
    // 直接从源 Surface 进行后处理（内部会先拷贝到临时纹理）
    bool ApplyFromSurface(IDirect3DSurface9* srcSurface, IDirect3DSurface9* dstSurface, const War3PostFxSettings& settings);

    bool IsReady() const { return m_initialized; }

  private:
    bool ensureResources(uint32_t width, uint32_t height, D3DFORMAT format);
    bool compileShaders(const std::string& path);
    bool createFullscreenResources();
    bool createBloomResources(uint32_t width, uint32_t height, D3DFORMAT format);
    bool copySurfaceToTexture(IDirect3DSurface9* srcSurface, IDirect3DTexture9* dstTexture);

    void drawFullscreen(IDirect3DPixelShader9* pixelShader);
    void setVSTexelSize(uint32_t width, uint32_t height);
    void setPSTexelSize(uint32_t width, uint32_t height);
    void setBloomParams(float threshold, float softKnee, float intensity, float acesEnabled);
    void setExposure(float exposure, bool useSrgb);
    void setCommonRenderStates();

  private:
    static constexpr uint32_t kBloomLevels = 6;

    IDirect3DDevice9* m_device = nullptr;
    bool m_initialized = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    D3DFORMAT m_format = D3DFMT_UNKNOWN;

    Com<IDirect3DTexture9> m_sceneTexture;
    Com<IDirect3DSurface9> m_sceneSurface;

    std::array<Com<IDirect3DTexture9>, kBloomLevels> m_bloomTextures;
    std::array<Com<IDirect3DSurface9>, kBloomLevels> m_bloomSurfaces;
    std::array<D3DVIEWPORT9, kBloomLevels> m_bloomViewports;

    Com<IDirect3DVertexShader9> m_vsFullscreen;
    Com<IDirect3DPixelShader9> m_psPrefilter;
    Com<IDirect3DPixelShader9> m_psDownsample;
    Com<IDirect3DPixelShader9> m_psUpsample;
    Com<IDirect3DPixelShader9> m_psComposite;
    Com<IDirect3DVertexDeclaration9> m_decl;
    Com<IDirect3DVertexBuffer9> m_fullscreenVB;
  };

} // namespace dxvk
