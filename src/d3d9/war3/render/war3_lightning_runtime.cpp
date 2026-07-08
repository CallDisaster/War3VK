#include "war3_lightning_runtime.h"

#include "../../d3d9_war3_debug.h"
#include "../../../util/com/com_pointer.h"
#include "../core/war3_file_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dxvk::war3::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct LightningVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  DWORD diffuse = 0xffffffffu;
  float u = 0.0f;
  float v = 0.0f;
};

double NowSeconds() {
  using Clock = std::chrono::steady_clock;
  static const auto s_start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - s_start).count();
}

float Clamp01(float v) {
  return std::clamp(v, 0.0f, 1.0f);
}

DWORD PackColor(float r, float g, float b, float a) {
  const auto toByte = [](float v) -> DWORD {
    return DWORD(std::lround(Clamp01(v) * 255.0f));
  };
  return D3DCOLOR_ARGB(toByte(a), toByte(r), toByte(g), toByte(b));
}

uint32_t HashU32(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

float HashSigned(uint32_t seed, uint32_t index, uint32_t salt) {
  const uint32_t h = HashU32(seed ^ (index * 0x9e3779b9u) ^ salt);
  const float unit = float(h & 0x00ffffffu) / float(0x00ffffffu);
  return unit * 2.0f - 1.0f;
}

War3LightningPoint Add(const War3LightningPoint& a,
                       const War3LightningPoint& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

War3LightningPoint Sub(const War3LightningPoint& a,
                       const War3LightningPoint& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

War3LightningPoint Scale(const War3LightningPoint& p, float s) {
  return {p.x * s, p.y * s, p.z * s};
}

War3LightningPoint Lerp(const War3LightningPoint& a,
                        const War3LightningPoint& b,
                        float t) {
  return Add(Scale(a, 1.0f - t), Scale(b, t));
}

float Length2D(float x, float y) {
  return std::sqrt(x * x + y * y);
}

War3LightningPoint ResolveSideVector(const War3LightningPoint& start,
                                     const War3LightningPoint& end) {
  const auto d = Sub(end, start);
  const float len = Length2D(d.x, d.y);
  if (len <= 1e-4f)
    return {1.0f, 0.0f, 0.0f};
  return {-d.y / len, d.x / len, 0.0f};
}

float ResolveRecordLifeAlpha(const War3LightningRuntime::LightningRecord& record,
                             double nowSec) {
  const float age = float(std::max(0.0, nowSec - record.createdSec));
  if (record.lifetimeSec > 0.0f && age >= record.lifetimeSec)
    return 0.0f;

  float alpha = 1.0f;
  if (record.fadeInSec > 0.0f)
    alpha = std::min(alpha, Clamp01(age / record.fadeInSec));
  if (record.lifetimeSec > 0.0f && record.fadeOutSec > 0.0f) {
    const float remaining = record.lifetimeSec - age;
    alpha = std::min(alpha, Clamp01(remaining / record.fadeOutSec));
  }
  if (record.pulseAmplitude > 0.0f && record.pulseFrequencyHz > 0.0f) {
    const float pulse =
        1.0f + record.pulseAmplitude *
                   std::sin(age * record.pulseFrequencyHz * 2.0f * kPi);
    alpha *= std::clamp(pulse, 0.0f, 2.0f);
  }
  return Clamp01(alpha);
}

void BuildRibbonVertices(
    const War3LightningRuntime::LightningRecord& record,
    const War3LightningPoint& start,
    const War3LightningPoint& end,
    float widthScale,
    float alphaScale,
    uint32_t segmentCount,
    uint32_t seedSalt,
    float uvOffset,
    std::vector<LightningVertex>& out) {
  if (segmentCount < 2u)
    segmentCount = 2u;
  segmentCount = std::min(segmentCount, 64u);

  const auto side = ResolveSideVector(start, end);
  out.clear();
  out.reserve(size_t(segmentCount + 1u) * 2u);

  for (uint32_t i = 0u; i <= segmentCount; ++i) {
    const float t = float(i) / float(segmentCount);
    War3LightningPoint p = Lerp(start, end, t);
    const float envelope = std::sin(t * kPi);
    const float sideNoise =
        HashSigned(record.seed ^ seedSalt, i, 0x51f15eedu) *
        record.noiseAmplitude * envelope;
    const float zNoise =
        HashSigned(record.seed ^ seedSalt, i, 0x6ac690c5u) *
        record.noiseAmplitude * 0.35f * envelope;
    p.x += side.x * sideNoise;
    p.y += side.y * sideNoise;
    p.z += record.curveAmplitude * envelope + zNoise;

    const float width =
        std::max(1.0f,
                 (record.startWidth * (1.0f - t) + record.endWidth * t) *
                     widthScale);
    const float r = record.startColor[0] * (1.0f - t) + record.endColor[0] * t;
    const float g = record.startColor[1] * (1.0f - t) + record.endColor[1] * t;
    const float b = record.startColor[2] * (1.0f - t) + record.endColor[2] * t;
    const float a =
        (record.startColor[3] * (1.0f - t) + record.endColor[3] * t) *
        alphaScale;
    const DWORD color = PackColor(r, g, b, a);

    LightningVertex left = {};
    left.x = p.x - side.x * width * 0.5f;
    left.y = p.y - side.y * width * 0.5f;
    left.z = p.z;
    left.diffuse = color;
    left.u = t * 3.0f + uvOffset;
    left.v = 0.0f;

    LightningVertex right = {};
    right.x = p.x + side.x * width * 0.5f;
    right.y = p.y + side.y * width * 0.5f;
    right.z = p.z;
    right.diffuse = color;
    right.u = t * 3.0f + uvOffset;
    right.v = 1.0f;

    out.push_back(left);
    out.push_back(right);
  }
}

bool CaptureStateBlock(IDirect3DDevice9* device,
                       Com<IDirect3DStateBlock9>& outStateBlock) {
  outStateBlock = nullptr;
  if (device == nullptr)
    return false;

  IDirect3DStateBlock9* stateBlock = nullptr;
  if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) ||
      stateBlock == nullptr) {
    return false;
  }

  if (FAILED(stateBlock->Capture())) {
    stateBlock->Release();
    return false;
  }

  outStateBlock = stateBlock;
  stateBlock->Release();
  return true;
}

uint32_t ReadLe32(const uint8_t* data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
         (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

bool DecodeBlp1Paletted(const std::vector<uint8_t>& data,
                        uint32_t& width,
                        uint32_t& height,
                        std::vector<uint32_t>& argb) {
  constexpr size_t kHeaderSize = 156u;
  constexpr size_t kPaletteOffset = kHeaderSize;
  constexpr size_t kPaletteSize = 256u * 4u;
  if (data.size() < kHeaderSize + kPaletteSize)
    return false;
  if (std::memcmp(data.data(), "BLP1", 4u) != 0)
    return false;

  const uint32_t compression = ReadLe32(data.data() + 4u);
  if (compression != 1u)
    return false;

  width = ReadLe32(data.data() + 12u);
  height = ReadLe32(data.data() + 16u);
  const uint32_t mipOffset = ReadLe32(data.data() + 28u);
  const uint32_t mipSize = ReadLe32(data.data() + 92u);
  if (width == 0u || height == 0u || width > 4096u || height > 4096u)
    return false;

  const uint64_t pixelCount64 = uint64_t(width) * uint64_t(height);
  if (pixelCount64 > 16ull * 1024ull * 1024ull)
    return false;
  const size_t pixelCount = size_t(pixelCount64);
  if (mipOffset >= data.size() || uint64_t(mipOffset) + mipSize > data.size())
    return false;
  if (mipSize < pixelCount)
    return false;

  const uint8_t* indices = data.data() + mipOffset;
  const uint8_t* alpha8 =
      mipSize >= pixelCount * 2u ? indices + pixelCount : nullptr;
  const uint8_t* alpha4 =
      !alpha8 && mipSize >= pixelCount + ((pixelCount + 1u) / 2u)
          ? indices + pixelCount
          : nullptr;
  const uint8_t* palette = data.data() + kPaletteOffset;

  argb.resize(pixelCount);
  for (size_t i = 0; i < pixelCount; ++i) {
    const uint8_t paletteIndex = indices[i];
    const uint8_t* p = palette + size_t(paletteIndex) * 4u;
    const uint8_t b = p[0];
    const uint8_t g = p[1];
    const uint8_t r = p[2];
    uint8_t a = 255u;
    if (alpha8) {
      a = alpha8[i];
    } else if (alpha4) {
      const uint8_t packed = alpha4[i / 2u];
      const uint8_t nibble = (i & 1u) ? uint8_t(packed >> 4) :
                                        uint8_t(packed & 0x0fu);
      a = uint8_t(nibble * 17u);
    } else if (p[3] != 0u) {
      a = p[3];
    }
    argb[i] = D3DCOLOR_ARGB(a, r, g, b);
  }
  return true;
}

std::vector<uint32_t> BuildProceduralLightningPixels(uint32_t width,
                                                     uint32_t height) {
  std::vector<uint32_t> pixels(size_t(width) * size_t(height));
  const float midY = (float(height) - 1.0f) * 0.5f;
  for (uint32_t y = 0u; y < height; ++y) {
    for (uint32_t x = 0u; x < width; ++x) {
      const float u = float(x) / float(std::max(1u, width - 1u));
      const float yNorm = (float(y) - midY) / std::max(1.0f, midY);
      const float wave = std::sin(u * 42.0f) * 0.18f +
                         std::sin(u * 91.0f + 1.7f) * 0.08f;
      const float coreDist = std::abs(yNorm - wave);
      const float core = std::exp(-coreDist * coreDist * 54.0f);
      const float glow = std::exp(-coreDist * coreDist * 9.0f) * 0.55f;
      const float spark =
          (HashSigned(0x6c696768u, x + y * width, 0x74424c50u) > 0.78f)
              ? 0.35f
              : 0.0f;
      const float a = Clamp01(core + glow + spark);
      const uint8_t alpha = uint8_t(std::lround(a * 255.0f));
      pixels[size_t(y) * width + x] =
          D3DCOLOR_ARGB(alpha, 255u, 255u, 255u);
    }
  }
  return pixels;
}

bool CreateTextureFromArgb(IDirect3DDevice9* device,
                           uint32_t width,
                           uint32_t height,
                           const std::vector<uint32_t>& pixels,
                           IDirect3DTexture9** outTexture) {
  if (device == nullptr || outTexture == nullptr ||
      pixels.size() < size_t(width) * size_t(height))
    return false;

  *outTexture = nullptr;
  IDirect3DTexture9* texture = nullptr;
  HRESULT hr = device->CreateTexture(width, height, 1u, 0u, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &texture, nullptr);
  if (FAILED(hr) || texture == nullptr)
    return false;

  D3DLOCKED_RECT rect = {};
  hr = texture->LockRect(0u, &rect, nullptr, 0u);
  if (FAILED(hr)) {
    texture->Release();
    return false;
  }

  for (uint32_t y = 0u; y < height; ++y) {
    auto* dst = reinterpret_cast<uint8_t*>(rect.pBits) + rect.Pitch * y;
    const auto* src = pixels.data() + size_t(y) * width;
    std::memcpy(dst, src, size_t(width) * sizeof(uint32_t));
  }
  texture->UnlockRect(0u);
  *outTexture = texture;
  return true;
}

bool DrawRibbon(IDirect3DDevice9* device,
                const std::vector<LightningVertex>& vertices,
                IDirect3DTexture9* texture,
                bool additive,
                bool depthTest) {
  if (device == nullptr || vertices.size() < 4u)
    return false;

  device->SetVertexShader(nullptr);
  device->SetPixelShader(nullptr);
  device->SetTexture(0, texture);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ZENABLE, depthTest ? TRUE : FALSE);
  device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  device->SetRenderState(D3DRS_DESTBLEND,
                         additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  if (texture != nullptr) {
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
  } else {
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  }
  device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

  const UINT primitiveCount = UINT(vertices.size() - 2u);
  const HRESULT hr =
      device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, primitiveCount,
                              vertices.data(), sizeof(LightningVertex));
  return SUCCEEDED(hr);
}

} // namespace

War3LightningRuntime& War3LightningRuntime::instance() {
  static War3LightningRuntime s_instance;
  return s_instance;
}

void War3LightningRuntime::setDevice(IDirect3DDevice9* device) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_device != device) {
    releaseTextureLocked();
    m_textureInitAttempted = false;
    m_textureFallback = false;
    m_textureSource.clear();
  }
  m_device = device;
  m_summary.hasDevice = m_device != nullptr;
  m_summary.textureLoaded = m_texture != nullptr;
  m_summary.textureFallback = m_textureFallback;
}

void War3LightningRuntime::reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_records.clear();
  releaseTextureLocked();
  m_textureInitAttempted = false;
  m_textureFallback = false;
  m_textureSource.clear();
  m_nextId = 1;
  m_summary = {};
  m_summary.hasDevice = m_device != nullptr;
}

void War3LightningRuntime::releaseTextureLocked() {
  if (m_texture != nullptr) {
    m_texture->Release();
    m_texture = nullptr;
  }
}

void War3LightningRuntime::ensureTexture(IDirect3DDevice9* device) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_textureInitAttempted || m_texture != nullptr || device == nullptr)
      return;
    m_textureInitAttempted = true;
    ++m_summary.textureLoadAttemptCount;
  }

  IDirect3DTexture9* texture = nullptr;
  bool fallback = false;
  std::string source;
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<uint32_t> pixels;

  static constexpr const char* kCandidateBlpPaths[] = {
      "war3mapImported\\Lightning.blp",
      "Textures\\Lightning.blp",
      "Textures\\LightningBolt.blp",
      "ReplaceableTextures\\Weather\\Lightning.blp",
      "Abilities\\Weapons\\Bolt\\Bolt.blp",
      "Abilities\\Weapons\\ChimaeraLightningMissile\\ChimaeraLightningMissile.blp",
  };

  auto& fileManager = dxvk::war3::FileManager::get();
  for (const char* path : kCandidateBlpPaths) {
    std::vector<uint8_t> data;
    if (!fileManager.readFile(path, data, true))
      continue;
    if (DecodeBlp1Paletted(data, width, height, pixels) &&
        CreateTextureFromArgb(device, width, height, pixels, &texture)) {
      source = path;
      break;
    }
  }

  if (texture == nullptr) {
    width = 128u;
    height = 16u;
    pixels = BuildProceduralLightningPixels(width, height);
    fallback = true;
    source = "procedural-warvk-lightning-strip";
    CreateTextureFromArgb(device, width, height, pixels, &texture);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (texture != nullptr) {
    m_texture = texture;
    m_textureFallback = fallback;
    m_textureSource = source;
    m_summary.textureLoaded = true;
    m_summary.textureFallback = fallback;
    if (fallback)
      ++m_summary.textureLoadFallbackCount;
    war3dbg::Print("DXVK War3Lightning: texture ready source=%s size=%ux%u "
                   "fallback=%d\n",
                   m_textureSource.c_str(), width, height, fallback ? 1 : 0);
  } else {
    m_summary.textureLoaded = false;
    m_summary.textureFallback = false;
    ++m_summary.commandFailureCount;
    war3dbg::Print("DXVK War3Lightning: texture creation failed\n");
  }
}

int32_t War3LightningRuntime::create(const War3LightningCreateDesc& desc) {
  std::lock_guard<std::mutex> lock(m_mutex);
  LightningRecord record = {};
  record.id = m_nextId++;
  if (m_nextId <= 0)
    m_nextId = 1;
  record.start = desc.start;
  record.end = desc.end;
  record.seed = uint32_t(HashU32(uint32_t(record.id) * 0x45d9f3bu));
  record.createdSec = NowSeconds();
  m_records.emplace(record.id, record);
  ++m_summary.createCount;
  m_summary.activeCount = uint32_t(m_records.size());
  if (m_summary.createCount <= 4u) {
    war3dbg::Print(
        "DXVK War3Lightning: create id=%d start=(%.1f,%.1f,%.1f) "
        "end=(%.1f,%.1f,%.1f) active=%u\n",
        record.id, double(record.start.x), double(record.start.y),
        double(record.start.z), double(record.end.x), double(record.end.y),
        double(record.end.z), m_summary.activeCount);
  }
  return record.id;
}

bool War3LightningRuntime::move(int32_t id,
                                const War3LightningPoint& start,
                                const War3LightningPoint& end) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.start = start;
  it->second.end = end;
  return true;
}

bool War3LightningRuntime::destroy(int32_t id) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const size_t erased = m_records.erase(id);
  if (erased == 0u)
    return false;
  ++m_summary.destroyCount;
  m_summary.activeCount = uint32_t(m_records.size());
  return true;
}

bool War3LightningRuntime::setColor(int32_t id,
                                    float r0, float g0, float b0, float a0,
                                    float r1, float g1, float b1, float a1) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.startColor[0] = r0;
  it->second.startColor[1] = g0;
  it->second.startColor[2] = b0;
  it->second.startColor[3] = a0;
  it->second.endColor[0] = r1;
  it->second.endColor[1] = g1;
  it->second.endColor[2] = b1;
  it->second.endColor[3] = a1;
  return true;
}

bool War3LightningRuntime::setWidth(int32_t id,
                                    float startWidth,
                                    float endWidth) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.startWidth = std::clamp(startWidth, 1.0f, 4096.0f);
  it->second.endWidth = std::clamp(endWidth, 1.0f, 4096.0f);
  return true;
}

bool War3LightningRuntime::setCurve(int32_t id,
                                    float curveAmplitude,
                                    float noiseAmplitude,
                                    uint32_t segments,
                                    uint32_t branchCount) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.curveAmplitude = std::clamp(curveAmplitude, -4096.0f, 4096.0f);
  it->second.noiseAmplitude = std::clamp(noiseAmplitude, 0.0f, 4096.0f);
  it->second.segments = std::clamp(segments, 2u, 64u);
  it->second.branchCount = std::clamp(branchCount, 0u, 8u);
  return true;
}

bool War3LightningRuntime::setLifetime(int32_t id,
                                       float lifetimeSec,
                                       float fadeInSec,
                                       float fadeOutSec) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.lifetimeSec = std::max(0.0f, lifetimeSec);
  it->second.fadeInSec = std::max(0.0f, fadeInSec);
  it->second.fadeOutSec = std::max(0.0f, fadeOutSec);
  it->second.createdSec = NowSeconds();
  return true;
}

bool War3LightningRuntime::setPulse(int32_t id,
                                    float amplitude,
                                    float frequencyHz) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.pulseAmplitude = std::clamp(amplitude, 0.0f, 2.0f);
  it->second.pulseFrequencyHz = std::clamp(frequencyHz, 0.0f, 120.0f);
  return true;
}

bool War3LightningRuntime::hasActive() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return !m_records.empty();
}

bool War3LightningRuntime::executePreparedFrame() {
  std::vector<LightningRecord> records;
  IDirect3DDevice9* device = nullptr;
  const double nowSec = NowSeconds();

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_records.begin(); it != m_records.end();) {
      if (ResolveRecordLifeAlpha(it->second, nowSec) <= 0.0f &&
          it->second.lifetimeSec > 0.0f) {
        it = m_records.erase(it);
      } else {
        records.push_back(it->second);
        ++it;
      }
    }

    m_summary.activeCount = uint32_t(m_records.size());
    device = m_device;

    if (records.empty()) {
      ++m_summary.drawSkippedNoActiveCount;
      m_summary.lastDrawVertexCount = 0u;
      m_summary.lastDrawPrimitiveCount = 0u;
      return false;
    }

    ++m_summary.drawAttemptCount;
    if (device == nullptr) {
      ++m_summary.drawSkippedNoDeviceCount;
      m_summary.hasDevice = false;
      return false;
    }
    m_summary.hasDevice = true;
  }

  return executeLockedCopy(records, nowSec);
}

bool War3LightningRuntime::executeLockedCopy(
    const std::vector<LightningRecord>& records,
    double nowSec) {
  IDirect3DDevice9* device = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    device = m_device;
  }
  if (device == nullptr)
    return false;
  ensureTexture(device);

  IDirect3DTexture9* texture = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    texture = m_texture;
    if (texture != nullptr)
      texture->AddRef();
  }

  Com<IDirect3DStateBlock9> stateBlock;
  if (!CaptureStateBlock(device, stateBlock)) {
    if (texture != nullptr)
      texture->Release();
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_summary.commandFailureCount;
    return false;
  }

  uint64_t vertexCount = 0u;
  uint64_t primitiveCount = 0u;
  bool anyDrawn = false;
  std::vector<LightningVertex> vertices;

  for (const auto& record : records) {
    const float lifeAlpha = ResolveRecordLifeAlpha(record, nowSec);
    if (lifeAlpha <= 0.0f)
      continue;

    const float uvOffset =
        float(nowSec * (0.72 + double((record.seed & 0xffu)) / 255.0 * 0.35));
    BuildRibbonVertices(record, record.start, record.end, 1.0f, lifeAlpha,
                        record.segments, 0u, uvOffset, vertices);
    if (DrawRibbon(device, vertices, texture, record.additive,
                   record.depthTest)) {
      anyDrawn = true;
      vertexCount += vertices.size();
      primitiveCount += vertices.size() >= 3u ? vertices.size() - 2u : 0u;
    }

    const auto side = ResolveSideVector(record.start, record.end);
    const uint32_t branchCount = std::min(record.branchCount, 8u);
    for (uint32_t b = 0u; b < branchCount; ++b) {
      const float t = float(b + 1u) / float(branchCount + 1u);
      const float sign = (b & 1u) ? -1.0f : 1.0f;
      const War3LightningPoint base = Lerp(record.start, record.end, t);
      const float branchLen =
          (record.curveAmplitude * 0.55f + record.startWidth * 3.0f +
           float(b) * 13.0f) *
          sign;
      War3LightningPoint branchEnd = {};
      branchEnd.x = base.x + side.x * branchLen;
      branchEnd.y = base.y + side.y * branchLen;
      branchEnd.z = base.z + record.curveAmplitude * 0.35f +
                    record.noiseAmplitude * 0.5f;

      BuildRibbonVertices(record, base, branchEnd, 0.45f, lifeAlpha * 0.55f,
                          std::max(3u, record.segments / 3u), b + 1u,
                          uvOffset + float(b + 1u) * 0.37f, vertices);
      if (DrawRibbon(device, vertices, texture, record.additive,
                     record.depthTest)) {
        anyDrawn = true;
        vertexCount += vertices.size();
        primitiveCount += vertices.size() >= 3u ? vertices.size() - 2u : 0u;
      }
    }
  }

  if (stateBlock != nullptr)
    stateBlock->Apply();
  if (texture != nullptr)
    texture->Release();

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_summary.lastDrawVertexCount = vertexCount;
    m_summary.lastDrawPrimitiveCount = primitiveCount;
    if (anyDrawn) {
      ++m_summary.drawSuccessCount;
      if (m_summary.drawSuccessCount <= 4u) {
        war3dbg::Print(
            "DXVK War3Lightning: draw success records=%zu vertices=%llu "
            "primitives=%llu\n",
            records.size(), static_cast<unsigned long long>(vertexCount),
            static_cast<unsigned long long>(primitiveCount));
      }
    }
  }
  return anyDrawn;
}

War3LightningSummary War3LightningRuntime::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  War3LightningSummary summary = m_summary;
  summary.activeCount = uint32_t(m_records.size());
  summary.hasDevice = m_device != nullptr;
  summary.textureLoaded = m_texture != nullptr;
  summary.textureFallback = m_textureFallback;
  return summary;
}

std::string War3LightningRuntime::statsString() const {
  War3LightningSummary summary = {};
  std::string textureSource;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    summary = m_summary;
    summary.activeCount = uint32_t(m_records.size());
    summary.hasDevice = m_device != nullptr;
    summary.textureLoaded = m_texture != nullptr;
    summary.textureFallback = m_textureFallback;
    textureSource = m_textureSource;
  }
  char buffer[512] = {};
  std::snprintf(
      buffer, sizeof(buffer),
      "active=%u creates=%llu destroys=%llu drawAttempts=%llu drawSuccess=%llu "
      "noDevice=%llu noActive=%llu lastVertices=%llu lastPrimitives=%llu "
      "hasDevice=%d textureLoaded=%d textureFallback=%d textureAttempts=%llu "
      "textureFallbacks=%llu failures=%llu textureSource=%s",
      summary.activeCount,
      static_cast<unsigned long long>(summary.createCount),
      static_cast<unsigned long long>(summary.destroyCount),
      static_cast<unsigned long long>(summary.drawAttemptCount),
      static_cast<unsigned long long>(summary.drawSuccessCount),
      static_cast<unsigned long long>(summary.drawSkippedNoDeviceCount),
      static_cast<unsigned long long>(summary.drawSkippedNoActiveCount),
      static_cast<unsigned long long>(summary.lastDrawVertexCount),
      static_cast<unsigned long long>(summary.lastDrawPrimitiveCount),
      summary.hasDevice ? 1 : 0, summary.textureLoaded ? 1 : 0,
      summary.textureFallback ? 1 : 0,
      static_cast<unsigned long long>(summary.textureLoadAttemptCount),
      static_cast<unsigned long long>(summary.textureLoadFallbackCount),
      static_cast<unsigned long long>(summary.commandFailureCount),
      textureSource.empty() ? "none" : textureSource.c_str());
  return buffer;
}

void War3LightningRuntime::noteCommandFailure() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_summary.commandFailureCount;
}

} // namespace dxvk::war3::render
