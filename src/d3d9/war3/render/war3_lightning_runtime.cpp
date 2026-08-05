#include "war3_lightning_runtime.h"

#include "../../d3d9_war3_debug.h"
#include "../../../util/com/com_pointer.h"
#include "../../../MemHack/3rd/stb/stb_image.h"
#include "../core/war3_file_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
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

float Length3D(const War3LightningPoint& value) {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z);
}

War3LightningPoint ResolveSideVector(const War3LightningPoint& start,
                                     const War3LightningPoint& end) {
  const auto d = Sub(end, start);
  const float len = Length2D(d.x, d.y);
  if (len <= 1e-4f)
    return {1.0f, 0.0f, 0.0f};
  return {-d.y / len, d.x / len, 0.0f};
}

float SmoothStep(float value) {
  const float clamped = Clamp01(value);
  return clamped * clamped * (3.0f - 2.0f * clamped);
}

float SmoothNoise(uint32_t seed, float coordinate, uint32_t salt) {
  // Keep the integer domain bounded so long-running maps do not lose all
  // fractional precision when a scrolling noise phase is sampled.
  const float wrapped = std::fmod(coordinate, 1048576.0f);
  const float floorValue = std::floor(wrapped);
  const auto first = static_cast<uint32_t>(static_cast<int32_t>(floorValue));
  const float fraction = wrapped - floorValue;
  const float a = HashSigned(seed, first, salt);
  const float b = HashSigned(seed, first + 1u, salt);
  return a + (b - a) * SmoothStep(fraction);
}

float ResolveFractalNoise(
    const War3LightningRuntime::LightningRecord& record,
    float t, double nowSec, uint32_t salt) {
  if (record.noiseAmplitude <= 0.0f || record.noiseFrequency <= 0.0f)
    return 0.0f;

  float coordinate =
      t * record.noiseFrequency + float(nowSec) * record.noiseScrollSpeed;
  float amplitude = 1.0f;
  float total = 0.0f;
  float weight = 0.0f;
  const uint32_t octaves = std::clamp(record.noiseOctaves, 1u, 4u);
  for (uint32_t octave = 0u; octave < octaves; ++octave) {
    total += SmoothNoise(record.seed ^ (octave * 0x9e3779b9u), coordinate,
                         salt ^ (octave * 0x85ebca6bu)) * amplitude;
    weight += amplitude;
    coordinate *= 2.0f;
    amplitude *= 0.5f;
  }
  return weight > 0.0f ? total / weight : 0.0f;
}

uint32_t ResolveSegmentCount(
    const War3LightningRuntime::LightningRecord& record,
    const War3LightningPoint& start, const War3LightningPoint& end) {
  const uint32_t minimum = std::clamp(record.minimumSegments, 2u, 64u);
  const uint32_t maximum = std::clamp(record.maximumSegments, minimum, 64u);
  if (record.averageSegmentLength <= 0.0f)
    return std::clamp(record.segments, minimum, maximum);

  const float length = Length3D(Sub(end, start));
  const uint32_t desired = static_cast<uint32_t>(std::lround(
      std::max(0.0f, length) / record.averageSegmentLength));
  return std::clamp(desired, minimum, maximum);
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
  if (record.flickerAmplitude > 0.0f && record.flickerFrequencyHz > 0.0f) {
    const float flicker = 0.5f + 0.5f * SmoothNoise(
        record.seed, age * record.flickerFrequencyHz, 0x71f1c4e5u);
    alpha *= 1.0f - record.flickerAmplitude * (1.0f - flicker);
  }
  return Clamp01(alpha);
}

float ResolvePulseAlpha(
    const War3LightningRuntime::LightningRecord& record,
    float t, double nowSec) {
  if (record.pulseAmplitude <= 0.0f || record.pulseFrequency <= 0.0f)
    return 1.0f;

  const float phase = record.pulseTravelSpeed == 0.0f
      ? float(nowSec) * record.pulseFrequency
      : t * record.pulseFrequency -
            float(nowSec) * record.pulseTravelSpeed;
  return std::clamp(1.0f + record.pulseAmplitude *
                               std::sin(phase * 2.0f * kPi),
                    0.0f, 2.0f);
}

War3LightningPoint ResolveRibbonCenterPoint(
    const War3LightningRuntime::LightningRecord& record,
    const War3LightningPoint& start,
    const War3LightningPoint& end,
    const War3LightningPoint& side,
    float t,
    double nowSec,
    uint32_t seedSalt,
    uint32_t segmentCount) {
  const float clampedT = Clamp01(t);
  if (record.formulaCurve.valid()) {
    math::CurveContext context;
    context.start = {start.x, start.y, start.z};
    context.end = {end.x, end.y, end.z};
    context.t = clampedT;
    context.time = float(std::max(0.0, nowSec - record.createdSec));
    context.seed = record.seed;
    context.index = static_cast<uint32_t>(std::lround(
        clampedT * float(std::max(segmentCount, 1u))));
    context.segments = std::max(segmentCount, 1u);
    context.branchIndex = seedSalt;
    context.branchDepth = seedSalt == 0u ? 0u : 1u;
    math::Vec3 evaluated;
    if (math::EvaluateCurveWorld(record.formulaCurve, context, evaluated))
      return {evaluated.x, evaluated.y, evaluated.z};
  }

  War3LightningPoint point = Lerp(start, end, clampedT);
  const float envelope = std::sin(clampedT * kPi);
  const float sideNoise =
      ResolveFractalNoise(record, clampedT, nowSec, 0x51f15eedu ^ seedSalt) *
      record.noiseAmplitude * envelope;
  const float zNoise =
      ResolveFractalNoise(record, clampedT, nowSec, 0x6ac690c5u ^ seedSalt) *
      record.noiseAmplitude * 0.35f * envelope;
  point.x += side.x * sideNoise;
  point.y += side.y * sideNoise;
  point.z += record.curveAmplitude * envelope + zNoise;
  return point;
}

War3LightningPoint ResolveRibbonLocalSide(
    const War3LightningRuntime::LightningRecord& record,
    const War3LightningPoint& start,
    const War3LightningPoint& end,
    const War3LightningPoint& parentSide,
    float t,
    double nowSec,
    uint32_t seedSalt,
    uint32_t segmentCount) {
  // Branches leave the actual deformed parent curve, not its undeformed
  // start/end chord.  A bounded central difference avoids a visible angular
  // jump when a main-bolt noise crest occurs at the branch anchor.
  const float step = std::clamp(1.0f / float(std::max(segmentCount, 8u)),
                                0.01f, 0.08f);
  const War3LightningPoint before = ResolveRibbonCenterPoint(
      record, start, end, parentSide, t - step, nowSec, seedSalt,
      segmentCount);
  const War3LightningPoint after = ResolveRibbonCenterPoint(
      record, start, end, parentSide, t + step, nowSec, seedSalt,
      segmentCount);
  return ResolveSideVector(before, after);
}

void BuildRibbonVertices(
    const War3LightningRuntime::LightningRecord& record,
    const War3LightningPoint& start,
    const War3LightningPoint& end,
    double nowSec,
    float widthScale,
    float alphaScale,
    uint32_t segmentCount,
    uint32_t seedSalt,
    float uOffset,
    std::vector<LightningVertex>& out) {
  if (segmentCount < 2u)
    segmentCount = 2u;
  segmentCount = std::min(segmentCount, 64u);

  const auto side = ResolveSideVector(start, end);
  out.clear();
  out.reserve(size_t(segmentCount + 1u) * 2u);

  for (uint32_t i = 0u; i <= segmentCount; ++i) {
    const float t = float(i) / float(segmentCount);
    const War3LightningPoint p = ResolveRibbonCenterPoint(
        record, start, end, side, t, nowSec, seedSalt, segmentCount);

    const float width =
        std::max(1.0f,
                 (record.startWidth * (1.0f - t) + record.endWidth * t) *
                     widthScale);
    const float r = record.startColor[0] * (1.0f - t) + record.endColor[0] * t;
    const float g = record.startColor[1] * (1.0f - t) + record.endColor[1] * t;
    const float b = record.startColor[2] * (1.0f - t) + record.endColor[2] * t;
    const float a =
        (record.startColor[3] * (1.0f - t) + record.endColor[3] * t) *
        alphaScale * ResolvePulseAlpha(record, t, nowSec);
    const DWORD color = PackColor(r, g, b, a);

    LightningVertex left = {};
    left.x = p.x - side.x * width * 0.5f;
    left.y = p.y - side.y * width * 0.5f;
    left.z = p.z;
    left.diffuse = color;
    left.u = uOffset + t * record.uvTiling +
             float(nowSec) * record.uvScrollSpeed;
    left.v = 0.0f;

    LightningVertex right = {};
    right.x = p.x + side.x * width * 0.5f;
    right.y = p.y + side.y * width * 0.5f;
    right.z = p.z;
    right.diffuse = color;
    right.u = uOffset + t * record.uvTiling +
              float(nowSec) * record.uvScrollSpeed;
    right.v = 1.0f;

    out.push_back(left);
    out.push_back(right);
  }
}

void BuildPolylineRibbonVertices(
    const War3LightningRuntime::LightningRecord& record,
    const math::PointCurveData& curve,
    double nowSec,
    float widthScale,
    float alphaScale,
    std::vector<LightningVertex>& out) {
  out.clear();
  if (curve.points.size() < math::kMinimumPointCurvePoints ||
      curve.points.size() != curve.cumulativeLengths.size() ||
      curve.totalLength <= 0.0f)
    return;

  out.reserve(curve.points.size() * 2u);
  War3LightningPoint previousSide = {1.0f, 0.0f, 0.0f};
  for (size_t index = 0u; index < curve.points.size(); ++index) {
    const size_t beforeIndex = index > 0u ? index - 1u : index;
    const size_t afterIndex =
        index + 1u < curve.points.size() ? index + 1u : index;
    const auto& source = curve.points[index];
    const auto& before = curve.points[beforeIndex];
    const auto& after = curve.points[afterIndex];
    const War3LightningPoint point{source.x, source.y, source.z};
    const float tangentX = after.x - before.x;
    const float tangentY = after.y - before.y;
    const float tangentLength = Length2D(tangentX, tangentY);
    War3LightningPoint side = tangentLength > 1.0e-4f
        ? War3LightningPoint{-tangentY / tangentLength,
                             tangentX / tangentLength, 0.0f}
        : previousSide;
    // Keep the ribbon orientation continuous across sharp turns.  Without
    // this sign qualification a 180-degree tangent crossing can swap left and
    // right vertices and twist the triangle strip.
    if (side.x * previousSide.x + side.y * previousSide.y < 0.0f)
      side = Scale(side, -1.0f);
    previousSide = side;

    const float t = Clamp01(curve.cumulativeLengths[index] /
                            curve.totalLength);
    const float width = std::max(
        1.0f,
        (record.startWidth * (1.0f - t) + record.endWidth * t) * widthScale);
    const float r = record.startColor[0] * (1.0f - t) + record.endColor[0] * t;
    const float g = record.startColor[1] * (1.0f - t) + record.endColor[1] * t;
    const float b = record.startColor[2] * (1.0f - t) + record.endColor[2] * t;
    const float a =
        (record.startColor[3] * (1.0f - t) + record.endColor[3] * t) *
        alphaScale * ResolvePulseAlpha(record, t, nowSec);
    const DWORD color = PackColor(r, g, b, a);
    const float u = t * record.uvTiling +
                    float(nowSec) * record.uvScrollSpeed;

    out.push_back({point.x - side.x * width * 0.5f,
                   point.y - side.y * width * 0.5f,
                   point.z, color, u, 0.0f});
    out.push_back({point.x + side.x * width * 0.5f,
                   point.y + side.y * width * 0.5f,
                   point.z, color, u, 1.0f});
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

bool DecodeStbImage(const uint8_t* bytes, size_t byteCount,
                    uint32_t& width, uint32_t& height,
                    std::vector<uint32_t>& argb) {
  if (bytes == nullptr || byteCount == 0u ||
      byteCount > static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;

  int decodedWidth = 0;
  int decodedHeight = 0;
  int components = 0;
  stbi_uc* const rgba = stbi_load_from_memory(
      bytes, static_cast<int>(byteCount), &decodedWidth, &decodedHeight,
      &components, 4);
  if (rgba == nullptr || decodedWidth <= 0 || decodedHeight <= 0 ||
      decodedWidth > 4096 || decodedHeight > 4096) {
    if (rgba != nullptr)
      stbi_image_free(rgba);
    return false;
  }

  const uint64_t pixelCount64 =
      uint64_t(decodedWidth) * uint64_t(decodedHeight);
  if (pixelCount64 > 16ull * 1024ull * 1024ull) {
    stbi_image_free(rgba);
    return false;
  }

  width = static_cast<uint32_t>(decodedWidth);
  height = static_cast<uint32_t>(decodedHeight);
  const size_t pixelCount = static_cast<size_t>(pixelCount64);
  argb.resize(pixelCount);
  for (size_t index = 0u; index < pixelCount; ++index) {
    const uint8_t* const pixel = rgba + index * 4u;
    argb[index] = D3DCOLOR_ARGB(pixel[3], pixel[0], pixel[1], pixel[2]);
  }
  stbi_image_free(rgba);
  return true;
}

void ApplyBlp1JpegSoftAlphaFromBorder(uint32_t width,
                                      uint32_t height,
                                      std::vector<uint32_t>& argb) {
  if (width == 0u || height == 0u ||
      argb.size() < size_t(width) * size_t(height))
    return;

  // Warcraft BLP1 JPEG commonly stores a fourth JPEG component containing the
  // authored alpha channel. Preserve it exactly when present. Older three-
  // component BLP1 assets have no alpha; for those only, recover a soft mask
  // from the nearly uniform border colour so their rectangular backdrop does
  // not become visible. PNG/TGA and paletted BLP alpha remain author-owned.
  const bool hasAuthoredAlpha = std::any_of(
      argb.begin(), argb.begin() + size_t(width) * size_t(height),
      [](uint32_t pixel) { return ((pixel >> 24u) & 0xffu) != 0xffu; });
  if (hasAuthoredAlpha)
    return;

  std::array<std::vector<uint8_t>, 3u> borderChannels = {};
  const size_t perimeter = width == 1u || height == 1u
      ? size_t(width) * size_t(height)
      : size_t(width) * 2u + size_t(height - 2u) * 2u;
  for (auto& channel : borderChannels)
    channel.reserve(perimeter);

  const auto sampleBorderPixel = [&borderChannels, &argb](size_t index) {
    const uint32_t pixel = argb[index];
    borderChannels[0u].push_back(uint8_t((pixel >> 16u) & 0xffu));
    borderChannels[1u].push_back(uint8_t((pixel >> 8u) & 0xffu));
    borderChannels[2u].push_back(uint8_t(pixel & 0xffu));
  };
  for (uint32_t x = 0u; x < width; ++x) {
    sampleBorderPixel(x);
    if (height > 1u)
      sampleBorderPixel(size_t(height - 1u) * width + x);
  }
  for (uint32_t y = 1u; y + 1u < height; ++y) {
    sampleBorderPixel(size_t(y) * width);
    if (width > 1u)
      sampleBorderPixel(size_t(y) * width + width - 1u);
  }

  const auto median = [](std::vector<uint8_t>& values) -> uint8_t {
    const auto middle = values.begin() + values.size() / 2u;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
  };
  const int32_t backgroundR = int32_t(median(borderChannels[0u]));
  const int32_t backgroundG = int32_t(median(borderChannels[1u]));
  const int32_t backgroundB = int32_t(median(borderChannels[2u]));

  // JPEG ringing around the background is normally below 6%, while the stock
  // bolt and its glow exceed 22% RGB distance.  SmoothStep retains a soft
  // halo rather than converting the image into a hard color key.
  constexpr float kMaskStart = 0.06f;
  constexpr float kMaskEnd = 0.22f;
  constexpr float kRgbDistanceScale =
      1.0f / (255.0f * 1.73205080756887729353f);
  for (uint32_t& pixel : argb) {
    const int32_t dr = int32_t((pixel >> 16u) & 0xffu) - backgroundR;
    const int32_t dg = int32_t((pixel >> 8u) & 0xffu) - backgroundG;
    const int32_t db = int32_t(pixel & 0xffu) - backgroundB;
    const float distance = std::sqrt(float(dr * dr + dg * dg + db * db)) *
        kRgbDistanceScale;
    const float coverage = SmoothStep((distance - kMaskStart) /
                                      (kMaskEnd - kMaskStart));
    const uint32_t sourceAlpha = (pixel >> 24u) & 0xffu;
    const uint32_t alpha = uint32_t(std::lround(coverage * sourceAlpha));
    pixel = (pixel & 0x00ffffffu) | (alpha << 24u);
  }
}

bool DecodeBlp1Jpeg(const std::vector<uint8_t>& data,
                    uint32_t& width,
                    uint32_t& height,
                    std::vector<uint32_t>& argb) {
  constexpr size_t kHeaderSize = 156u;
  constexpr size_t kJpegHeaderSizeOffset = kHeaderSize;
  constexpr size_t kJpegHeaderOffset = kHeaderSize + sizeof(uint32_t);
  if (data.size() < kJpegHeaderOffset)
    return false;

  const uint32_t jpegHeaderSize = ReadLe32(data.data() + kJpegHeaderSizeOffset);
  const uint32_t mipOffset = ReadLe32(data.data() + 28u);
  const uint32_t mipSize = ReadLe32(data.data() + 92u);
  if (jpegHeaderSize == 0u ||
      uint64_t(kJpegHeaderOffset) + jpegHeaderSize > data.size() ||
      mipOffset >= data.size() ||
      uint64_t(mipOffset) + mipSize > data.size())
    return false;

  std::vector<uint8_t> jpeg;
  jpeg.reserve(size_t(jpegHeaderSize) + size_t(mipSize));
  jpeg.insert(jpeg.end(), data.begin() + kJpegHeaderOffset,
              data.begin() + kJpegHeaderOffset + jpegHeaderSize);
  jpeg.insert(jpeg.end(), data.begin() + mipOffset,
              data.begin() + mipOffset + mipSize);
  if (!DecodeStbImage(jpeg.data(), jpeg.size(), width, height, argb))
    return false;
  ApplyBlp1JpegSoftAlphaFromBorder(width, height, argb);
  return true;
}

bool DecodeLightningTexture(const std::vector<uint8_t>& data,
                            uint32_t& width,
                            uint32_t& height,
                            std::vector<uint32_t>& argb) {
  if (data.size() >= 8u && std::memcmp(data.data(), "BLP1", 4u) == 0) {
    const uint32_t compression = ReadLe32(data.data() + 4u);
    if (compression == 0u)
      return DecodeBlp1Jpeg(data, width, height, argb);
    if (compression == 1u)
      return DecodeBlp1Paletted(data, width, height, argb);
    return false;
  }
  // stb_image gives the template layer TGA, PNG, JPEG, and BMP support
  // without adding another decoder dependency to the DXVK DLL.
  return DecodeStbImage(data.data(), data.size(), width, height, argb);
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
  // This world-stage callback can run after an arbitrary Warcraft material.
  // Blend factors alone are not enough: a retained SUBTRACT/REVSUBTRACT
  // operation turns an otherwise white/yellow source texture into the
  // complement-like green/magenta artifact seen on screen.  The captured
  // state block restores the game's state after this draw, so make every
  // color-output state that affects the ribbon explicit here.
  device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0fu);
  device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  device->SetRenderState(D3DRS_FOGENABLE, FALSE);
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

bool IsFiniteRange(float value, float minimum, float maximum) {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool IsValidColor(const std::array<float, 4u>& color) {
  return std::isfinite(color[0]) && color[0] >= 0.0f &&
         std::isfinite(color[1]) && color[1] >= 0.0f &&
         std::isfinite(color[2]) && color[2] >= 0.0f &&
         IsFiniteRange(color[3], 0.0f, 1.0f);
}

bool ApplyRenderMode(War3LightningRuntime::LightningRecord& record,
                     int32_t renderMode) {
  switch (renderMode) {
    case 0:
      record.additive = false;
      record.depthTest = false;
      return true;
    case 1:
      record.additive = false;
      record.depthTest = true;
      return true;
    case 2:
      record.additive = true;
      record.depthTest = false;
      return true;
    case 3:
      record.additive = true;
      record.depthTest = true;
      return true;
    default:
      return false;
  }
}

} // namespace

War3LightningRuntime& War3LightningRuntime::instance() {
  static War3LightningRuntime s_instance;
  return s_instance;
}

void War3LightningRuntime::setDevice(IDirect3DDevice9* device) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_device != device)
    releaseTexturesLocked();
  m_device = device;
  m_summary.hasDevice = m_device != nullptr;
}

void War3LightningRuntime::reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_records.clear();
  m_templates.clear();
  releaseTexturesLocked();
  m_nextId = 1;
  m_nextTemplateId = 1;
  m_summary = {};
  m_summary.hasDevice = m_device != nullptr;
}

void War3LightningRuntime::releaseTexturesLocked() {
  for (auto& entry : m_textureCache) {
    if (entry.second.texture != nullptr) {
      entry.second.texture->Release();
      entry.second.texture = nullptr;
    }
  }
  m_textureCache.clear();
  m_summary.textureLoaded = false;
  m_summary.textureFallback = false;
  m_summary.textureCacheEntryCount = 0u;
}

IDirect3DTexture9* War3LightningRuntime::acquireTexture(
    IDirect3DDevice9* device, const std::string& texturePath) {
  if (device == nullptr)
    return nullptr;

  std::lock_guard<std::mutex> lock(m_mutex);
  TextureCacheEntry& entry = m_textureCache[texturePath];
  if (entry.loadAttempted) {
    if (entry.texture != nullptr)
      entry.texture->AddRef();
    return entry.texture;
  }

  entry.loadAttempted = true;
  ++m_summary.textureLoadAttemptCount;

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
  const auto tryLoad = [&](const char* path) {
    std::vector<uint8_t> data;
    if (!fileManager.readFile(path, data, true))
      return false;
    if (!DecodeLightningTexture(data, width, height, pixels) ||
        !CreateTextureFromArgb(device, width, height, pixels, &texture))
      return false;
    source = path;
    return true;
  };

  if (!texturePath.empty()) {
    static_cast<void>(tryLoad(texturePath.c_str()));
  } else {
    for (const char* path : kCandidateBlpPaths) {
      if (tryLoad(path))
        break;
    }
  }

  if (texture == nullptr) {
    width = 128u;
    height = 16u;
    pixels = BuildProceduralLightningPixels(width, height);
    fallback = true;
    source = texturePath.empty()
        ? "procedural-warvk-lightning-strip"
        : "procedural-warvk-lightning-strip:" + texturePath;
    CreateTextureFromArgb(device, width, height, pixels, &texture);
  }

  if (texture != nullptr) {
    entry.texture = texture;
    entry.fallback = fallback;
    entry.source = source;
    if (fallback)
      ++m_summary.textureLoadFallbackCount;
    war3dbg::Print("DXVK War3Lightning: texture ready source=%s size=%ux%u "
                   "fallback=%d\n",
                   entry.source.c_str(), width, height, fallback ? 1 : 0);
  } else {
    ++m_summary.commandFailureCount;
    war3dbg::Print("DXVK War3Lightning: texture creation failed\n");
  }

  uint32_t loadedCount = 0u;
  uint32_t fallbackCount = 0u;
  for (const auto& cached : m_textureCache) {
    if (cached.second.texture == nullptr)
      continue;
    ++loadedCount;
    if (cached.second.fallback)
      ++fallbackCount;
  }
  m_summary.textureCacheEntryCount = uint32_t(m_textureCache.size());
  m_summary.textureLoaded = loadedCount != 0u;
  m_summary.textureFallback = fallbackCount != 0u;
  if (entry.texture != nullptr)
    entry.texture->AddRef();
  return entry.texture;
}

int32_t War3LightningRuntime::create(const War3LightningCreateDesc& desc) {
  std::lock_guard<std::mutex> lock(m_mutex);
  LightningRecord record = {};
  record.id = m_nextId++;
  if (m_nextId <= 0)
    m_nextId = 1;
  record.start = desc.start;
  record.end = desc.end;
  // Preserve the original direct-create shape rather than silently changing
  // old JASS effects to length-adaptive segmentation.
  record.averageSegmentLength = 0.0f;
  record.minimumSegments = record.segments;
  record.maximumSegments = record.segments;
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

int32_t War3LightningRuntime::createTemplate(const std::string& name) {
  if (name.empty())
    return 0;

  std::lock_guard<std::mutex> lock(m_mutex);
  TemplateRecord templateRecord = {};
  const int32_t assignedId = m_nextTemplateId++;
  templateRecord.id = assignedId;
  if (m_nextTemplateId <= 0)
    m_nextTemplateId = 1;
  templateRecord.name = name;
  templateRecord.values.templateId = templateRecord.id;
  if (!m_templates.emplace(templateRecord.id, std::move(templateRecord)).second)
    return 0;

  ++m_summary.templateCreateCount;
  if (m_summary.templateCreateCount <= 8u) {
    war3dbg::Print("DXVK War3Lightning: template create id=%d name=%s\n",
                   assignedId, name.c_str());
  }
  return assignedId;
}

bool War3LightningRuntime::setTemplateBasic(
    int32_t templateId, const War3LightningTemplateBasicDesc& desc) {
  if (desc.texturePath.empty() || !IsValidColor(desc.startColor) ||
      !IsValidColor(desc.endColor) ||
      !IsFiniteRange(desc.startWidth, 1.0f, 4096.0f) ||
      !IsFiniteRange(desc.endWidth, 1.0f, 4096.0f) ||
      !IsFiniteRange(desc.uvTiling, 0.01f, 128.0f) ||
      !IsFiniteRange(desc.uvScrollSpeed, -128.0f, 128.0f))
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  if (it == m_templates.end() || it->second.finalized ||
      !ApplyRenderMode(it->second.values, desc.renderMode))
    return false;

  LightningRecord& values = it->second.values;
  values.texturePath = desc.texturePath;
  std::copy(desc.startColor.begin(), desc.startColor.end(),
            std::begin(values.startColor));
  std::copy(desc.endColor.begin(), desc.endColor.end(),
            std::begin(values.endColor));
  values.startWidth = desc.startWidth;
  values.endWidth = desc.endWidth;
  values.uvTiling = desc.uvTiling;
  values.uvScrollSpeed = desc.uvScrollSpeed;
  it->second.basicConfigured = true;
  return true;
}

bool War3LightningRuntime::setTemplateAdvanced(
    int32_t templateId, const War3LightningTemplateAdvancedDesc& desc) {
  if (!IsFiniteRange(desc.averageSegmentLength, 4.0f, 8192.0f) ||
      desc.minimumSegments < 2u || desc.minimumSegments > 64u ||
      desc.maximumSegments < desc.minimumSegments ||
      desc.maximumSegments > 64u ||
      !IsFiniteRange(desc.curveAmplitude, -4096.0f, 4096.0f) ||
      !IsFiniteRange(desc.noiseAmplitude, 0.0f, 4096.0f) ||
      !IsFiniteRange(desc.noiseFrequency, 0.0f, 64.0f) ||
      !IsFiniteRange(desc.noiseScrollSpeed, -64.0f, 64.0f) ||
      desc.noiseOctaves < 1u || desc.noiseOctaves > 4u ||
      desc.branchCount > 8u ||
      !IsFiniteRange(desc.branchLengthScale, 0.0f, 8.0f) ||
      !IsFiniteRange(desc.branchWidthScale, 0.05f, 4.0f))
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  if (it == m_templates.end() || it->second.finalized)
    return false;

  LightningRecord& values = it->second.values;
  values.averageSegmentLength = desc.averageSegmentLength;
  values.minimumSegments = desc.minimumSegments;
  values.maximumSegments = desc.maximumSegments;
  values.curveAmplitude = desc.curveAmplitude;
  values.noiseAmplitude = desc.noiseAmplitude;
  values.noiseFrequency = desc.noiseFrequency;
  values.noiseScrollSpeed = desc.noiseScrollSpeed;
  values.noiseOctaves = desc.noiseOctaves;
  values.branchCount = desc.branchCount;
  values.branchLengthScale = desc.branchLengthScale;
  values.branchWidthScale = desc.branchWidthScale;
  it->second.advancedConfigured = true;
  return true;
}

bool War3LightningRuntime::setTemplateOptional(
    int32_t templateId, const War3LightningTemplateOptionalDesc& desc) {
  if (!IsFiniteRange(desc.lifetimeSec, 0.0f, 600.0f) ||
      !IsFiniteRange(desc.fadeInSec, 0.0f, 600.0f) ||
      !IsFiniteRange(desc.fadeOutSec, 0.0f, 600.0f) ||
      (desc.lifetimeSec > 0.0f &&
       (desc.fadeInSec > desc.lifetimeSec ||
        desc.fadeOutSec > desc.lifetimeSec)) ||
      !IsFiniteRange(desc.pulseAmplitude, 0.0f, 1.0f) ||
      !IsFiniteRange(desc.pulseFrequency, 0.0f, 120.0f) ||
      !IsFiniteRange(desc.pulseTravelSpeed, -120.0f, 120.0f) ||
      !IsFiniteRange(desc.flickerAmplitude, 0.0f, 1.0f) ||
      !IsFiniteRange(desc.flickerFrequencyHz, 0.0f, 120.0f) ||
      !IsFiniteRange(desc.glowWidthScale, 1.0f, 8.0f) ||
      !IsFiniteRange(desc.glowOpacity, 0.0f, 1.0f))
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  if (it == m_templates.end() || it->second.finalized)
    return false;

  LightningRecord& values = it->second.values;
  values.lifetimeSec = desc.lifetimeSec;
  values.fadeInSec = desc.fadeInSec;
  values.fadeOutSec = desc.fadeOutSec;
  values.pulseAmplitude = desc.pulseAmplitude;
  values.pulseFrequency = desc.pulseFrequency;
  values.pulseTravelSpeed = desc.pulseTravelSpeed;
  values.flickerAmplitude = desc.flickerAmplitude;
  values.flickerFrequencyHz = desc.flickerFrequencyHz;
  values.glowWidthScale = desc.glowWidthScale;
  values.glowOpacity = desc.glowOpacity;
  return true;
}

bool War3LightningRuntime::setTemplateFormulaCurve(
    int32_t templateId, const math::CurveSnapshot& curve) {
  if (!curve.isFormula() || !curve.renderable())
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  if (it == m_templates.end() || it->second.finalized)
    return false;
  it->second.values.formulaCurve = curve;
  return true;
}

bool War3LightningRuntime::finalizeTemplate(int32_t templateId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  if (it == m_templates.end() || it->second.finalized ||
      !it->second.basicConfigured || !it->second.advancedConfigured)
    return false;
  it->second.finalized = true;
  ++m_summary.templateFinalizeCount;
  war3dbg::Print("DXVK War3Lightning: template finalized id=%d name=%s\n",
                 templateId, it->second.name.c_str());
  return true;
}

bool War3LightningRuntime::isTemplateFinalized(int32_t templateId) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_templates.find(templateId);
  return it != m_templates.end() && it->second.finalized;
}

int32_t War3LightningRuntime::createFromTemplate(
    int32_t templateId, const War3LightningCreateDesc& desc, uint32_t seed) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto templateIt = m_templates.find(templateId);
  if (templateIt == m_templates.end() || !templateIt->second.finalized)
    return 0;

  LightningRecord record = templateIt->second.values;
  record.id = m_nextId++;
  if (m_nextId <= 0)
    m_nextId = 1;
  record.templateId = templateId;
  record.start = desc.start;
  record.end = desc.end;
  record.seed = seed != 0u
      ? seed
      : HashU32(uint32_t(record.id) * 0x45d9f3bu ^ uint32_t(templateId));
  record.createdSec = NowSeconds();
  record.enabled = true;
  if (!m_records.emplace(record.id, record).second)
    return 0;
  ++m_summary.createCount;
  m_summary.activeCount = uint32_t(m_records.size());
  return record.id;
}

int32_t War3LightningRuntime::createPolylineFromTemplate(
    int32_t templateId,
    std::shared_ptr<const math::PointCurveData> points,
    uint32_t seed) {
  if (points == nullptr || points->points.size() <
          math::kMinimumPointCurvePoints ||
      points->points.size() > math::kMaximumPointCurvePoints ||
      points->points.size() != points->cumulativeLengths.size() ||
      points->totalLength <= 0.0f)
    return 0;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto templateIt = m_templates.find(templateId);
  if (templateIt == m_templates.end() || !templateIt->second.finalized)
    return 0;

  LightningRecord record = templateIt->second.values;
  record.id = m_nextId++;
  const int32_t assignedId = record.id;
  if (m_nextId <= 0)
    m_nextId = 1;
  record.templateId = templateId;
  const auto& first = points->points.front();
  const auto& last = points->points.back();
  record.start = {first.x, first.y, first.z};
  record.end = {last.x, last.y, last.z};
  record.seed = seed != 0u
      ? seed
      : HashU32(uint32_t(record.id) * 0x45d9f3bu ^ uint32_t(templateId));
  record.createdSec = NowSeconds();
  record.enabled = true;
  record.formulaCurve = {};
  const uint64_t pointCount = points->points.size();
  record.polylineCurve = std::move(points);
  // A submitted polyline is already the complete authored centerline.
  // Template-generated branches remain reserved for endpoint/formula bolts.
  record.branchCount = 0u;
  if (!m_records.emplace(record.id, std::move(record)).second)
    return 0;
  ++m_summary.createCount;
  ++m_summary.polylineCreateCount;
  m_summary.lastPolylinePointCount = pointCount;
  m_summary.activeCount = uint32_t(m_records.size());
  return assignedId;
}

bool War3LightningRuntime::move(int32_t id,
                                const War3LightningPoint& start,
                                const War3LightningPoint& end) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_records.find(id);
  if (it == m_records.end() || it->second.polylineCurve != nullptr)
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

bool War3LightningRuntime::setEnabled(int32_t id, bool enabled) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.enabled = enabled;
  return true;
}

bool War3LightningRuntime::isAlive(int32_t id) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_records.find(id) != m_records.end();
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
  it->second.averageSegmentLength = 0.0f;
  it->second.minimumSegments = it->second.segments;
  it->second.maximumSegments = it->second.segments;
  it->second.branchCount = std::clamp(branchCount, 0u, 8u);
  return true;
}

bool War3LightningRuntime::setFormulaCurve(
    int32_t id, const math::CurveSnapshot& curve) {
  if (!curve.isFormula() || !curve.renderable())
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  it->second.formulaCurve = curve;
  it->second.polylineCurve.reset();
  return true;
}

bool War3LightningRuntime::setPolylineCurve(
    int32_t id,
    std::shared_ptr<const math::PointCurveData> points) {
  if (points == nullptr || points->points.size() <
          math::kMinimumPointCurvePoints ||
      points->points.size() > math::kMaximumPointCurvePoints ||
      points->points.size() != points->cumulativeLengths.size() ||
      points->totalLength <= 0.0f)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_records.find(id);
  if (it == m_records.end())
    return false;
  const auto& first = points->points.front();
  const auto& last = points->points.back();
  const uint64_t pointCount = points->points.size();
  it->second.start = {first.x, first.y, first.z};
  it->second.end = {last.x, last.y, last.z};
  it->second.formulaCurve = {};
  it->second.polylineCurve = std::move(points);
  it->second.branchCount = 0u;
  m_summary.lastPolylinePointCount = pointCount;
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
  it->second.pulseFrequency = std::clamp(frequencyHz, 0.0f, 120.0f);
  it->second.pulseTravelSpeed = 0.0f;
  return true;
}

bool War3LightningRuntime::hasActive() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return std::any_of(
      m_records.begin(), m_records.end(),
      [](const auto& entry) { return entry.second.enabled; });
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
      } else if (it->second.enabled) {
        records.push_back(it->second);
        ++it;
      } else {
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

  Com<IDirect3DStateBlock9> stateBlock;
  if (!CaptureStateBlock(device, stateBlock)) {
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

    IDirect3DTexture9* const texture = acquireTexture(device, record.texturePath);
    const bool isPolyline = record.polylineCurve != nullptr;
    const uint32_t segmentCount = isPolyline ? 0u :
        ResolveSegmentCount(record, record.start, record.end);

    if (record.glowOpacity > 0.0f && record.glowWidthScale > 1.0f) {
      if (isPolyline) {
        BuildPolylineRibbonVertices(record, *record.polylineCurve, nowSec,
                                    record.glowWidthScale,
                                    lifeAlpha * record.glowOpacity, vertices);
      } else {
        BuildRibbonVertices(record, record.start, record.end, nowSec,
                            record.glowWidthScale,
                            lifeAlpha * record.glowOpacity, segmentCount, 0u,
                            0.0f, vertices);
      }
      if (DrawRibbon(device, vertices, texture, record.additive,
                     record.depthTest)) {
        anyDrawn = true;
        vertexCount += vertices.size();
        primitiveCount += vertices.size() >= 3u ? vertices.size() - 2u : 0u;
      }
    }

    if (isPolyline) {
      BuildPolylineRibbonVertices(record, *record.polylineCurve, nowSec,
                                  1.0f, lifeAlpha, vertices);
    } else {
      BuildRibbonVertices(record, record.start, record.end, nowSec, 1.0f,
                          lifeAlpha, segmentCount, 0u, 0.0f, vertices);
    }
    if (DrawRibbon(device, vertices, texture, record.additive,
                   record.depthTest)) {
      anyDrawn = true;
      vertexCount += vertices.size();
      primitiveCount += vertices.size() >= 3u ? vertices.size() - 2u : 0u;
    }

    if (isPolyline) {
      if (texture != nullptr)
        texture->Release();
      continue;
    }

    const auto parentSide = ResolveSideVector(record.start, record.end);
    const uint32_t branchCount = std::min(record.branchCount, 8u);
    for (uint32_t b = 0u; b < branchCount; ++b) {
      // A zero length scale is an explicit authoring choice to suppress
      // branches.  Do not leave a vertical residual branch from the curve or
      // noise terms in that case.
      if (record.branchLengthScale <= 0.0f)
        continue;
      const float t = float(b + 1u) / float(branchCount + 1u);
      const float sign = (b & 1u) ? -1.0f : 1.0f;
      const War3LightningPoint base = ResolveRibbonCenterPoint(
          record, record.start, record.end, parentSide, t, nowSec, 0u,
          segmentCount);
      const War3LightningPoint side = ResolveRibbonLocalSide(
          record, record.start, record.end, parentSide, t, nowSec, 0u,
          segmentCount);
      const float branchLen =
          (std::abs(record.curveAmplitude) * 0.55f +
           record.startWidth * 3.0f + float(b) * 13.0f) *
          record.branchLengthScale * sign;
      War3LightningPoint branchEnd = {};
      branchEnd.x = base.x + side.x * branchLen;
      branchEnd.y = base.y + side.y * branchLen;
      branchEnd.z = base.z + record.curveAmplitude * 0.35f +
                    record.noiseAmplitude * 0.5f;

      BuildRibbonVertices(record, base, branchEnd, nowSec,
                          record.branchWidthScale, lifeAlpha * 0.55f,
                          std::max(3u,
                                   ResolveSegmentCount(record, base, branchEnd) /
                                       2u),
                          b + 1u, t * record.uvTiling, vertices);
      if (DrawRibbon(device, vertices, texture, record.additive,
                     record.depthTest)) {
        anyDrawn = true;
        vertexCount += vertices.size();
        primitiveCount += vertices.size() >= 3u ? vertices.size() - 2u : 0u;
      }
    }
    if (texture != nullptr)
      texture->Release();
  }

  if (stateBlock != nullptr)
    stateBlock->Apply();

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
  summary.polylineActiveCount = uint32_t(std::count_if(
      m_records.begin(), m_records.end(), [](const auto& entry) {
        return entry.second.polylineCurve != nullptr;
      }));
  summary.templateCount = uint32_t(m_templates.size());
  summary.finalizedTemplateCount = uint32_t(std::count_if(
      m_templates.begin(), m_templates.end(), [](const auto& entry) {
        return entry.second.finalized;
      }));
  summary.textureCacheEntryCount = uint32_t(m_textureCache.size());
  summary.textureLoaded = std::any_of(
      m_textureCache.begin(), m_textureCache.end(), [](const auto& entry) {
        return entry.second.texture != nullptr;
      });
  summary.textureFallback = std::any_of(
      m_textureCache.begin(), m_textureCache.end(), [](const auto& entry) {
        return entry.second.texture != nullptr && entry.second.fallback;
      });
  summary.hasDevice = m_device != nullptr;
  return summary;
}

std::string War3LightningRuntime::statsString() const {
  const War3LightningSummary summary = snapshot();
  char buffer[512] = {};
  std::snprintf(
      buffer, sizeof(buffer),
      "active=%u polylines=%u templates=%u finalizedTemplates=%u textureCache=%u "
      "creates=%llu polylineCreates=%llu templateCreates=%llu templateFinalizes=%llu destroys=%llu "
      "drawAttempts=%llu drawSuccess=%llu "
      "noDevice=%llu noActive=%llu lastVertices=%llu lastPrimitives=%llu "
      "hasDevice=%d textureLoaded=%d textureFallback=%d textureAttempts=%llu "
      "textureFallbacks=%llu failures=%llu",
      summary.activeCount,
      summary.polylineActiveCount,
      summary.templateCount,
      summary.finalizedTemplateCount,
      summary.textureCacheEntryCount,
      static_cast<unsigned long long>(summary.createCount),
      static_cast<unsigned long long>(summary.polylineCreateCount),
      static_cast<unsigned long long>(summary.templateCreateCount),
      static_cast<unsigned long long>(summary.templateFinalizeCount),
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
      static_cast<unsigned long long>(summary.commandFailureCount));
  return buffer;
}

void War3LightningRuntime::noteCommandFailure() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_summary.commandFailureCount;
}

} // namespace dxvk::war3::render
