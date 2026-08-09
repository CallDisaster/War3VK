#pragma once

#include "../../d3d9_include.h"
#include "../math/war3_curve_runtime.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::render {

struct War3LightningPoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct War3LightningCreateDesc {
  War3LightningPoint start = {};
  War3LightningPoint end = {};
};

// JASS authors build a template before creating any instances.  The renderer
// owns GPU resources, while these descriptors stay CPU-only and are therefore
// safe to prepare during map initialization before a D3D9 device is available.
struct War3LightningTemplateBasicDesc {
  std::string texturePath;
  std::array<float, 4u> startColor = {0.35f, 0.75f, 1.0f, 0.95f};
  std::array<float, 4u> endColor = {0.95f, 0.95f, 1.0f, 0.75f};
  float startWidth = 26.0f;
  float endWidth = 10.0f;
  float uvTiling = 3.0f;
  float uvScrollSpeed = 0.85f;
  int32_t renderMode = 3;
};

struct War3LightningTemplateAdvancedDesc {
  float averageSegmentLength = 80.0f;
  uint32_t minimumSegments = 4u;
  uint32_t maximumSegments = 32u;
  float curveAmplitude = 80.0f;
  float noiseAmplitude = 42.0f;
  float noiseFrequency = 8.0f;
  float noiseScrollSpeed = 0.0f;
  uint32_t noiseOctaves = 1u;
  uint32_t branchCount = 2u;
  float branchLengthScale = 1.0f;
  float branchWidthScale = 0.45f;
};

struct War3LightningTemplateOptionalDesc {
  float lifetimeSec = 0.0f;
  float fadeInSec = 0.08f;
  float fadeOutSec = 0.16f;
  float pulseAmplitude = 0.25f;
  float pulseFrequency = 7.0f;
  float pulseTravelSpeed = 0.0f;
  float flickerAmplitude = 0.0f;
  float flickerFrequencyHz = 0.0f;
  float glowWidthScale = 1.0f;
  float glowOpacity = 0.0f;
};

struct War3LightningSummary {
  uint32_t activeCount = 0;
  uint32_t polylineActiveCount = 0;
  uint32_t templateCount = 0;
  uint32_t finalizedTemplateCount = 0;
  uint32_t textureCacheEntryCount = 0;
  uint64_t createCount = 0;
  uint64_t polylineCreateCount = 0;
  uint64_t templateCreateCount = 0;
  uint64_t templateFinalizeCount = 0;
  uint64_t destroyCount = 0;
  uint64_t commandFailureCount = 0;
  uint64_t drawAttemptCount = 0;
  uint64_t drawSuccessCount = 0;
  uint64_t drawSkippedNoDeviceCount = 0;
  uint64_t drawSkippedNoActiveCount = 0;
  uint64_t textureLoadAttemptCount = 0;
  uint64_t textureLoadFallbackCount = 0;
  uint64_t lastDrawVertexCount = 0;
  uint64_t lastDrawPrimitiveCount = 0;
  uint64_t lastPolylinePointCount = 0;
  bool hasDevice = false;
  bool textureLoaded = false;
  bool textureFallback = false;
};

class War3LightningRuntime {
public:
  struct LightningRecord {
    int32_t id = 0;
    War3LightningPoint start = {};
    War3LightningPoint end = {};
    float startColor[4] = {0.35f, 0.75f, 1.0f, 0.95f};
    float endColor[4] = {0.95f, 0.95f, 1.0f, 0.75f};
    float startWidth = 26.0f;
    float endWidth = 10.0f;
    float uvTiling = 3.0f;
    float uvScrollSpeed = 0.85f;
    float averageSegmentLength = 80.0f;
    uint32_t minimumSegments = 4u;
    uint32_t maximumSegments = 32u;
    float curveAmplitude = 80.0f;
    float noiseAmplitude = 42.0f;
    float noiseFrequency = 8.0f;
    float noiseScrollSpeed = 0.0f;
    uint32_t noiseOctaves = 1u;
    float lifetimeSec = 0.0f;
    float fadeInSec = 0.08f;
    float fadeOutSec = 0.16f;
    float pulseAmplitude = 0.25f;
    float pulseFrequency = 7.0f;
    float pulseTravelSpeed = 0.0f;
    float flickerAmplitude = 0.0f;
    float flickerFrequencyHz = 0.0f;
    float glowWidthScale = 1.0f;
    float glowOpacity = 0.0f;
    uint32_t segments = 18u;
    uint32_t branchCount = 2u;
    float branchLengthScale = 1.0f;
    float branchWidthScale = 0.45f;
    uint32_t seed = 0u;
    int32_t templateId = 0;
    math::CurveSnapshot formulaCurve = {};
    std::shared_ptr<const math::PointCurveData> polylineCurve;
    std::string texturePath;
    double createdSec = 0.0;
    bool enabled = true;
    bool additive = true;
    bool depthTest = true;
  };

  static War3LightningRuntime& instance();

  void setDevice(IDirect3DDevice9* device);
  // Game/JASS threads may retire map-authored CPU descriptors, but must not
  // release the renderer-owned texture cache. The Present owner calls reset()
  // for the full CPU + GPU transition.
  void resetAuthorState();
  void reset();

  int32_t create(const War3LightningCreateDesc& desc);
  int32_t createTemplate(const std::string& name);
  bool setTemplateBasic(int32_t templateId,
                        const War3LightningTemplateBasicDesc& desc);
  bool setTemplateAdvanced(int32_t templateId,
                           const War3LightningTemplateAdvancedDesc& desc);
  bool setTemplateOptional(int32_t templateId,
                           const War3LightningTemplateOptionalDesc& desc);
  bool setTemplateFormulaCurve(int32_t templateId,
                               const math::CurveSnapshot& curve);
  bool finalizeTemplate(int32_t templateId);
  bool isTemplateFinalized(int32_t templateId) const;
  int32_t createFromTemplate(int32_t templateId,
                             const War3LightningCreateDesc& desc,
                             uint32_t seed);
  int32_t createPolylineFromTemplate(
      int32_t templateId,
      std::shared_ptr<const math::PointCurveData> points,
      uint32_t seed);
  bool move(int32_t id, const War3LightningPoint& start,
            const War3LightningPoint& end);
  bool destroy(int32_t id);
  bool setEnabled(int32_t id, bool enabled);
  bool isAlive(int32_t id) const;
  bool setColor(int32_t id,
                float r0, float g0, float b0, float a0,
                float r1, float g1, float b1, float a1);
  bool setWidth(int32_t id, float startWidth, float endWidth);
  bool setCurve(int32_t id, float curveAmplitude, float noiseAmplitude,
                uint32_t segments, uint32_t branchCount);
  bool setFormulaCurve(int32_t id, const math::CurveSnapshot& curve);
  bool setPolylineCurve(
      int32_t id,
      std::shared_ptr<const math::PointCurveData> points);
  bool setLifetime(int32_t id, float lifetimeSec, float fadeInSec,
                   float fadeOutSec);
  bool setPulse(int32_t id, float amplitude, float frequencyHz);

  bool hasActive() const;
  bool executePreparedFrame();
  War3LightningSummary snapshot() const;
  std::string statsString() const;
  void noteCommandFailure();

private:
  struct TemplateRecord {
    int32_t id = 0;
    std::string name;
    LightningRecord values = {};
    bool basicConfigured = false;
    bool advancedConfigured = false;
    bool finalized = false;
  };

  struct TextureCacheEntry {
    IDirect3DTexture9* texture = nullptr;
    bool loadAttempted = false;
    bool fallback = false;
    std::string source;
  };

  War3LightningRuntime() = default;

  bool executeLockedCopy(const std::vector<LightningRecord>& records,
                         double nowSec);
  void resetAuthorStateLocked();
  void releaseTexturesLocked();
  IDirect3DTexture9* acquireTexture(IDirect3DDevice9* device,
                                     const std::string& texturePath);

  mutable std::mutex m_mutex;
  IDirect3DDevice9* m_device = nullptr;
  int32_t m_nextId = 1;
  int32_t m_nextTemplateId = 1;
  std::unordered_map<int32_t, LightningRecord> m_records;
  std::unordered_map<int32_t, TemplateRecord> m_templates;
  std::unordered_map<std::string, TextureCacheEntry> m_textureCache;
  War3LightningSummary m_summary = {};
};

} // namespace dxvk::war3::render
