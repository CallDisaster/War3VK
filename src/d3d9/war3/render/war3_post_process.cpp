#include "war3_post_process.h"

#include "../shader/war3_shader_manager.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace dxvk {

  namespace {
    constexpr uint32_t kDxbcMagic = uint32_t('D') | (uint32_t('X') << 8) | (uint32_t('B') << 16) | (uint32_t('C') << 24);

    bool IsDxsoToken(uint32_t token) {
      const uint32_t hi = token & 0xFFFF0000u;
      return hi == 0xFFFE0000u || hi == 0xFFFF0000u;
    }

    struct PostFxVertex {
      float x, y, z, w;
      float u, v;
    };

    const PostFxVertex kFullscreenTri[3] = {
      { -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
      { -1.0f,  3.0f, 0.0f, 1.0f, 0.0f, -1.0f },
      {  3.0f, -1.0f, 0.0f, 1.0f, 2.0f, 1.0f },
    };

    using pD3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    using pD3DGetBlobPart = HRESULT (WINAPI*)(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT, ID3DBlob**);

    // [Embedded Shader Source]
    const char* kWar3PostFxShader = R"HLSL(
// War3 后处理：Bloom + ACES ToneMapping（D3D9）

float4 g_bloomParams : register(c0); // x=阈值 y=softKnee z=强度 w=ACES启用
float4 g_texelSize   : register(c1); // x=1/w y=1/h
float4 g_exposure    : register(c2); // x=曝光

sampler2D s_source : register(s0);
sampler2D s_bloom  : register(s1);

struct VS_IN {
  float4 pos : POSITION0;
  float2 uv  : TEXCOORD0;
};

struct VS_OUT {
  float4 pos : POSITION0;
  float2 uv  : TEXCOORD0;
};

VS_OUT VS_Fullscreen(VS_IN input) {
  VS_OUT o;
  o.pos = input.pos;
  // D3D9 Half-Pixel 校正，避免全屏采样模糊
  o.uv = input.uv + g_texelSize.xy * 0.5;
  return o;
}

float3 ACESFitted(float3 color) {
  const float3x3 ACESInputMat = {
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777
  };
  const float3x3 ACESOutputMat = {
    1.60475, -0.53108, -0.07367,
    -0.10208, 1.10813, -0.00605,
    -0.00327, -0.07276, 1.07602
  };

  color = mul(ACESInputMat, color);

  // RRT + ODT 近似
  float3 a = color * (color + 0.0245786) - 0.000090537;
  float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
  color = a / b;

  color = mul(ACESOutputMat, color);
  return saturate(color);
}

float3 LinearToSRGB(float3 c) {
  return pow(saturate(c), 1.0 / 2.2);
}

float3 SRGBToLinear(float3 c) {
  return pow(saturate(c), 2.2);
}

float4 PS_BloomPrefilter(VS_OUT input) : COLOR0 {
  float3 color = SRGBToLinear(tex2D(s_source, input.uv).rgb);
  float brightness = max(max(color.r, color.g), color.b);
  float threshold = g_bloomParams.x;
  float knee = max(1e-4, g_bloomParams.y);
  float soft = saturate((brightness - threshold + knee) / (2.0 * knee));
  float weight = max(brightness - threshold, 0.0) / max(brightness, 1e-4);
  weight = max(weight, soft * soft);
  return float4(color * weight, 1.0);
}

float4 PS_Downsample(VS_OUT input) : COLOR0 {
  float2 t = g_texelSize.xy;
  float2 uv = input.uv;
  float3 sum = 0.0;
  sum += tex2D(s_source, uv).rgb;
  sum += tex2D(s_source, uv + t * float2( 1,  0)).rgb;
  sum += tex2D(s_source, uv + t * float2(-1,  0)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0,  1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 1,  1)).rgb;
  sum += tex2D(s_source, uv + t * float2(-1,  1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 1, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2(-1, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 2,  0)).rgb;
  sum += tex2D(s_source, uv + t * float2(-2,  0)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0,  2)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0, -2)).rgb;
  return float4(sum / 13.0, 1.0);
}

float4 PS_Upsample(VS_OUT input) : COLOR0 {
  float2 t = g_texelSize.xy;
  float2 uv = input.uv;
  float3 sum = 0.0;
  sum += tex2D(s_source, uv + t * float2(-1, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 1, -1)).rgb;
  sum += tex2D(s_source, uv + t * float2(-1,  0)).rgb;
  sum += tex2D(s_source, uv).rgb;
  sum += tex2D(s_source, uv + t * float2( 1,  0)).rgb;
  sum += tex2D(s_source, uv + t * float2(-1,  1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 0,  1)).rgb;
  sum += tex2D(s_source, uv + t * float2( 1,  1)).rgb;
  return float4(sum / 9.0, 1.0);
}

float4 PS_Composite(VS_OUT input) : COLOR0 {
  float3 baseColor = SRGBToLinear(tex2D(s_source, input.uv).rgb);
  float3 bloom = tex2D(s_bloom, input.uv).rgb;
  float3 color = baseColor + bloom * g_bloomParams.z;
  color *= max(g_exposure.x, 0.0);
  if (g_bloomParams.w > 0.5) {
    color = ACESFitted(color);
  } else {
    color = saturate(color);
  }
  color = LinearToSRGB(color);
  return float4(color, 1.0);
}
)HLSL";

    bool CompileDxso(const std::vector<uint8_t>& source,
                     const std::string& sourceName,
                     const char* entry,
                     const char* profile,
                     std::vector<uint32_t>& outTokens,
                     std::string& outError) {
      static HMODULE s_compiler = LoadLibraryA("d3dcompiler_47.dll");
      if (!s_compiler) {
        outError = "缺少 d3dcompiler_47.dll";
        return false;
      }

      auto fnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(s_compiler, "D3DCompile"));
      auto fnGetBlobPart = reinterpret_cast<pD3DGetBlobPart>(GetProcAddress(s_compiler, "D3DGetBlobPart"));
      if (!fnCompile) {
        outError = "获取 D3DCompile 失败";
        return false;
      }

      ID3DBlob* code = nullptr;
      ID3DBlob* error = nullptr;
      const UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
      HRESULT hr = fnCompile(source.data(), source.size(), sourceName.c_str(),
                             nullptr, nullptr, entry, profile, flags, 0, &code, &error);
      if (FAILED(hr)) {
        if (error) {
          const char* errText = static_cast<const char*>(error->GetBufferPointer());
          outError = errText ? std::string(errText) : std::string("编译失败");
          error->Release();
        } else {
          outError = "编译失败";
        }
        if (code)
          code->Release();
        return false;
      }

      if (!code) {
        outError = "编译器返回空结果";
        return false;
      }

      ID3DBlob* legacy = nullptr;
      if (fnGetBlobPart) {
        const HRESULT hrLegacy = fnGetBlobPart(code->GetBufferPointer(), code->GetBufferSize(),
                                               D3D_BLOB_LEGACY_SHADER, 0, &legacy);
        if (SUCCEEDED(hrLegacy) && legacy) {
          code->Release();
          code = legacy;
        }
      }

      if (code->GetBufferSize() < sizeof(uint32_t)) {
        outError = "DXSO 字节码过短";
        code->Release();
        return false;
      }

      const uint32_t magic = *reinterpret_cast<const uint32_t*>(code->GetBufferPointer());
      if (magic == kDxbcMagic || !IsDxsoToken(magic)) {
        outError = "DXSO 字节码格式不兼容 D3D9";
        code->Release();
        return false;
      }

      const size_t dwordCount = code->GetBufferSize() / sizeof(uint32_t);
      outTokens.resize(dwordCount);
      std::memcpy(outTokens.data(), code->GetBufferPointer(), code->GetBufferSize());
      code->Release();
      return true;
    }
  }

  War3PostProcess::War3PostProcess(IDirect3DDevice9* device)
  : m_device(device) {
  }

  void War3PostProcess::Init(uint32_t width, uint32_t height, D3DFORMAT format) {
    if (!m_device || width == 0 || height == 0)
      return;

    m_width = width;
    m_height = height;
    m_format = format;

    if (!compileShaders("shaders/war3_postfx.hlsl"))
      return;

    if (!createFullscreenResources())
      return;

    if (!createBloomResources(width, height, format))
      return;

    m_initialized = true;
  }

  void War3PostProcess::Shutdown() {
    m_sceneSurface = nullptr;
    m_sceneTexture = nullptr;
    for (auto& tex : m_bloomTextures)
      tex = nullptr;
    for (auto& surf : m_bloomSurfaces)
      surf = nullptr;

    m_vsFullscreen = nullptr;
    m_psPrefilter = nullptr;
    m_psDownsample = nullptr;
    m_psUpsample = nullptr;
    m_psComposite = nullptr;
    m_decl = nullptr;
    m_fullscreenVB = nullptr;

    m_initialized = false;
    m_width = 0;
    m_height = 0;
    m_format = D3DFMT_UNKNOWN;
  }

  void War3PostProcess::OnResize(uint32_t width, uint32_t height, D3DFORMAT format) {
    if (width == 0 || height == 0)
      return;
    if (width == m_width && height == m_height && format == m_format && m_initialized)
      return;

    Shutdown();
    Init(width, height, format);
  }

  bool War3PostProcess::ApplyFromSurface(IDirect3DSurface9* srcSurface, IDirect3DSurface9* dstSurface, const War3PostFxSettings& settings) {
    if (!srcSurface || !dstSurface || !m_device)
      return false;

    D3DSURFACE_DESC desc = { };
    if (FAILED(srcSurface->GetDesc(&desc)))
      return false;

    if (!ensureResources(desc.Width, desc.Height, desc.Format))
      return false;

    if (!copySurfaceToTexture(srcSurface, m_sceneTexture.ptr()))
      return false;

    return Apply(m_sceneTexture.ptr(), dstSurface, settings);
  }

  bool War3PostProcess::Apply(IDirect3DTexture9* srcTexture, IDirect3DSurface9* dstSurface, const War3PostFxSettings& settings) {
    if (!srcTexture || !dstSurface || !m_device)
      return false;

    D3DSURFACE_DESC srcDesc = { };
    if (FAILED(srcTexture->GetLevelDesc(0, &srcDesc)))
      return false;

    // [Save State] RT/DS and Viewport/Scissor must be manually restored
    Com<IDirect3DSurface9> oldRT;
    Com<IDirect3DSurface9> oldDS;
    m_device->GetRenderTarget(0, &oldRT);
    m_device->GetDepthStencilSurface(&oldDS);

    D3DVIEWPORT9 oldViewport;
    m_device->GetViewport(&oldViewport);
    RECT oldScissor;
    m_device->GetScissorRect(&oldScissor);

    if (!ensureResources(srcDesc.Width, srcDesc.Height, srcDesc.Format))
      return false;

    if (m_psPrefilter == nullptr || m_psDownsample == nullptr || m_psUpsample == nullptr || m_psComposite == nullptr) {
      static bool s_logMissing = false;
      if (!s_logMissing) {
         s_logMissing = true;
         Logger::err("War3PostProcess: Shaders are NULL in Apply! Aborting PostFX."); 
      }
      return false;
    }

    Com<IDirect3DStateBlock9> stateBlock;
    if (FAILED(m_device->CreateStateBlock(D3DSBT_ALL, &stateBlock))) {
      Logger::err("War3PostProcess: 状态块创建失败");
      return false;
    }
    stateBlock->Capture();

    setCommonRenderStates();

    m_device->SetVertexShader(m_vsFullscreen.ptr());
    m_device->SetVertexDeclaration(m_decl.ptr());
    m_device->SetStreamSource(0, m_fullscreenVB.ptr(), 0, sizeof(PostFxVertex));
    m_device->SetIndices(nullptr);

    auto setSamplerFilter = [&](DWORD sampler, DWORD filter) {
      m_device->SetSamplerState(sampler, D3DSAMP_MINFILTER, filter);
      m_device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, filter);
    };

    const float bloomThreshold = settings.bloom.threshold;
    const float bloomSoftKnee = settings.bloom.softKnee;
    const float bloomIntensity = settings.bloom.enabled ? settings.bloom.intensity : 0.0f;
    const float acesEnabled = settings.bloom.acesToneMap ? 1.0f : 0.0f;
    setBloomParams(bloomThreshold, bloomSoftKnee, bloomIntensity, acesEnabled);
    setExposure(settings.exposure, settings.useSrgb);

    // 1) Prefilter
    setSamplerFilter(0, D3DTEXF_LINEAR);
    setSamplerFilter(1, D3DTEXF_LINEAR);
    m_device->SetRenderTarget(0, m_bloomSurfaces[0].ptr());
    m_device->SetDepthStencilSurface(nullptr);
    m_device->SetViewport(&m_bloomViewports[0]);
    m_device->SetTexture(0, srcTexture);
    m_device->SetTexture(1, nullptr);
      setVSTexelSize(m_bloomViewports[0].Width, m_bloomViewports[0].Height);
      setPSTexelSize(srcDesc.Width, srcDesc.Height); // PS needs Source (Scene) texel size
      drawFullscreen(m_psPrefilter.ptr());

      // Downsample Loop
      for (uint32_t i = 1; i < kBloomLevels; ++i) {
        setSamplerFilter(0, D3DTEXF_LINEAR);
        setSamplerFilter(1, D3DTEXF_LINEAR);
        m_device->SetRenderTarget(0, m_bloomSurfaces[i].ptr());
        m_device->SetTexture(0, m_bloomTextures[i - 1].ptr());
        m_device->SetViewport(&m_bloomViewports[i]);

        setVSTexelSize(m_bloomViewports[i].Width, m_bloomViewports[i].Height);
        setPSTexelSize(m_bloomViewports[i - 1].Width, m_bloomViewports[i - 1].Height);
        drawFullscreen(m_psDownsample.ptr());
      }

      // Upsample Loop
      m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

      for (int i = kBloomLevels - 2; i >= 0; --i) {
        setSamplerFilter(0, D3DTEXF_LINEAR);
        setSamplerFilter(1, D3DTEXF_LINEAR);
        m_device->SetRenderTarget(0, m_bloomSurfaces[i].ptr());
        m_device->SetTexture(0, m_bloomTextures[i + 1].ptr());
        // Viewport is already set if we care, but safer to set
        m_device->SetViewport(&m_bloomViewports[i]);

        setVSTexelSize(m_bloomViewports[i].Width, m_bloomViewports[i].Height);
        setPSTexelSize(m_bloomViewports[i + 1].Width, m_bloomViewports[i + 1].Height);
        drawFullscreen(m_psUpsample.ptr());
      }

    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // 4) Composite + ToneMap
    setSamplerFilter(0, D3DTEXF_POINT);
    setSamplerFilter(1, D3DTEXF_LINEAR);
    D3DVIEWPORT9 fullVp = { 0, 0, srcDesc.Width, srcDesc.Height, 0.0f, 1.0f };
    m_device->SetRenderTarget(0, dstSurface);
    m_device->SetViewport(&fullVp);
    m_device->SetTexture(0, srcTexture);
    m_device->SetTexture(1, m_bloomTextures[0].ptr());
    // PS_Composite 不需要 Texel Size (它使用 tex2D 1:1 采样)，但 VS 需要 DstSize 做偏移
    // Dst is Backbuffer (srcDesc size)
    setVSTexelSize(srcDesc.Width, srcDesc.Height);
    setPSTexelSize(1, 1); // unused
    drawFullscreen(m_psComposite.ptr());

    stateBlock->Apply();

    // [Restore State]
    m_device->SetRenderTarget(0, oldRT.ptr());
    m_device->SetDepthStencilSurface(oldDS.ptr());
    m_device->SetViewport(&oldViewport);
    m_device->SetScissorRect(&oldScissor);
    return true;
  }

  bool War3PostProcess::ensureResources(uint32_t width, uint32_t height, D3DFORMAT format) {
    if (!m_device || width == 0 || height == 0)
      return false;
    if (!m_initialized || width != m_width || height != m_height || format != m_format) {
      OnResize(width, height, format);
    }
    return m_initialized;
  }

  bool War3PostProcess::compileShaders(const std::string& path) {
    // [Embedded] Use internal shader code to guarantee loading
    const std::string sourceStr = kWar3PostFxShader;
    std::vector<uint8_t> source(sourceStr.begin(), sourceStr.end());

    std::vector<uint32_t> tokens;
    std::string error;

    if (!CompileDxso(source, path, "VS_Fullscreen", "vs_3_0", tokens, error)) {
      Logger::err("War3PostProcess: VS 编译失败: " + error);
      return false;
    }
    if (FAILED(m_device->CreateVertexShader(reinterpret_cast<const DWORD*>(tokens.data()), &m_vsFullscreen))) {
      Logger::err("War3PostProcess: VS 创建失败");
      return false;
    }

    auto compilePs = [&](const char* entry, Com<IDirect3DPixelShader9>& outPs) {
      tokens.clear();
      error.clear();
      if (!CompileDxso(source, path, entry, "ps_3_0", tokens, error)) {
        Logger::err(std::string("War3PostProcess: PS 编译失败: ") + error);
        return false;
      }
      if (FAILED(m_device->CreatePixelShader(reinterpret_cast<const DWORD*>(tokens.data()), &outPs))) {
        Logger::err("War3PostProcess: PS 创建失败");
        return false;
      }
      return true;
    };

    if (!compilePs("PS_BloomPrefilter", m_psPrefilter))
      return false;
    if (!compilePs("PS_Downsample", m_psDownsample))
      return false;
    if (!compilePs("PS_Upsample", m_psUpsample))
      return false;
    if (!compilePs("PS_Composite", m_psComposite))
      return false;

    return true;
  }

  bool War3PostProcess::createFullscreenResources() {
    if (!m_device)
      return false;

    D3DVERTEXELEMENT9 elements[] = {
      { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
    };

    if (FAILED(m_device->CreateVertexDeclaration(elements, &m_decl))) {
      Logger::err("War3PostProcess: 创建顶点声明失败");
      return false;
    }

    if (FAILED(m_device->CreateVertexBuffer(sizeof(kFullscreenTri), D3DUSAGE_WRITEONLY, 0,
                                             D3DPOOL_DEFAULT, &m_fullscreenVB, nullptr))) {
      Logger::err("War3PostProcess: 创建全屏 VB 失败");
      return false;
    }

    void* data = nullptr;
    if (FAILED(m_fullscreenVB->Lock(0, sizeof(kFullscreenTri), &data, 0))) {
      Logger::err("War3PostProcess: 锁定 VB 失败");
      return false;
    }
    std::memcpy(data, kFullscreenTri, sizeof(kFullscreenTri));
    m_fullscreenVB->Unlock();
    return true;
  }

  bool War3PostProcess::createBloomResources(uint32_t width, uint32_t height, D3DFORMAT format) {
    if (!m_device)
      return false;

    if (FAILED(m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                        format, D3DPOOL_DEFAULT, &m_sceneTexture, nullptr))) {
      Logger::err("War3PostProcess: 创建场景纹理失败");
      return false;
    }
    if (FAILED(m_sceneTexture->GetSurfaceLevel(0, &m_sceneSurface))) {
      Logger::err("War3PostProcess: 获取场景纹理 surface 失败");
      return false;
    }

    uint32_t w = std::max(1u, width);
    uint32_t h = std::max(1u, height);
    for (uint32_t i = 0; i < kBloomLevels; ++i) {
      if (FAILED(m_device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET,
                                          format, D3DPOOL_DEFAULT, &m_bloomTextures[i], nullptr))) {
        Logger::err("War3PostProcess: 创建 Bloom 纹理失败");
        return false;
      }
      if (FAILED(m_bloomTextures[i]->GetSurfaceLevel(0, &m_bloomSurfaces[i]))) {
        Logger::err("War3PostProcess: 获取 Bloom surface 失败");
        return false;
      }

      m_bloomViewports[i] = { 0, 0, w, h, 0.0f, 1.0f };
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    return true;
  }

  bool War3PostProcess::copySurfaceToTexture(IDirect3DSurface9* srcSurface, IDirect3DTexture9* dstTexture) {
    if (!srcSurface || !dstTexture || !m_device)
      return false;

    Com<IDirect3DSurface9> dstSurface;
    if (FAILED(dstTexture->GetSurfaceLevel(0, &dstSurface))) {
      Logger::err("War3PostProcess: 获取拷贝目标 surface 失败");
      return false;
    }

    HRESULT hr = m_device->StretchRect(srcSurface, nullptr, dstSurface.ptr(), nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        Logger::err("War3PostProcess: 场景拷贝失败");
      }
      return false;
    }
    return true;
  }

  void War3PostProcess::drawFullscreen(IDirect3DPixelShader9* pixelShader) {
    if (!pixelShader || !m_device)
      return;

    m_device->SetPixelShader(pixelShader);
    m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
  }

  void War3PostProcess::setVSTexelSize(uint32_t width, uint32_t height) {
    if (!m_device || width == 0 || height == 0)
      return;
    const float texel[4] = {
      1.0f / float(width),
      1.0f / float(height),
      float(width),
      float(height)
    };
    m_device->SetVertexShaderConstantF(1, texel, 1);
  }

  void War3PostProcess::setPSTexelSize(uint32_t width, uint32_t height) {
    if (!m_device || width == 0 || height == 0)
      return;
    const float texel[4] = {
      1.0f / float(width),
      1.0f / float(height),
      float(width),
      float(height)
    };
    m_device->SetPixelShaderConstantF(1, texel, 1);
  }

  void War3PostProcess::setBloomParams(float threshold, float softKnee, float intensity, float acesEnabled) {
    if (!m_device)
      return;
    const float params[4] = { threshold, softKnee, intensity, acesEnabled };
    m_device->SetPixelShaderConstantF(0, params, 1);
  }

  void War3PostProcess::setExposure(float exposure, bool useSrgb) {
    if (!m_device)
      return;
    const float params[4] = { exposure, useSrgb ? 1.0f : 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(2, params, 1);
  }

  void War3PostProcess::setCommonRenderStates() {
    if (!m_device)
      return;

    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

    for (DWORD s = 0; s < 2; ++s) {
      m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      m_device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      m_device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
  }

} // namespace dxvk
