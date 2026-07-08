#pragma once

#include "../../d3d9_include.h"

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

struct War3LightningSummary {
  uint32_t activeCount = 0;
  uint64_t createCount = 0;
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
    float curveAmplitude = 80.0f;
    float noiseAmplitude = 42.0f;
    float lifetimeSec = 0.0f;
    float fadeInSec = 0.08f;
    float fadeOutSec = 0.16f;
    float pulseAmplitude = 0.25f;
    float pulseFrequencyHz = 7.0f;
    uint32_t segments = 18u;
    uint32_t branchCount = 2u;
    uint32_t seed = 0u;
    double createdSec = 0.0;
    bool additive = true;
    bool depthTest = true;
  };

  static War3LightningRuntime& instance();

  void setDevice(IDirect3DDevice9* device);
  void reset();

  int32_t create(const War3LightningCreateDesc& desc);
  bool move(int32_t id, const War3LightningPoint& start,
            const War3LightningPoint& end);
  bool destroy(int32_t id);
  bool setColor(int32_t id,
                float r0, float g0, float b0, float a0,
                float r1, float g1, float b1, float a1);
  bool setWidth(int32_t id, float startWidth, float endWidth);
  bool setCurve(int32_t id, float curveAmplitude, float noiseAmplitude,
                uint32_t segments, uint32_t branchCount);
  bool setLifetime(int32_t id, float lifetimeSec, float fadeInSec,
                   float fadeOutSec);
  bool setPulse(int32_t id, float amplitude, float frequencyHz);

  bool hasActive() const;
  bool executePreparedFrame();
  War3LightningSummary snapshot() const;
  std::string statsString() const;
  void noteCommandFailure();

private:
  War3LightningRuntime() = default;

  bool executeLockedCopy(const std::vector<LightningRecord>& records,
                         double nowSec);
  void releaseTextureLocked();
  void ensureTexture(IDirect3DDevice9* device);

  mutable std::mutex m_mutex;
  IDirect3DDevice9* m_device = nullptr;
  IDirect3DTexture9* m_texture = nullptr;
  bool m_textureInitAttempted = false;
  bool m_textureFallback = false;
  std::string m_textureSource;
  int32_t m_nextId = 1;
  std::unordered_map<int32_t, LightningRecord> m_records;
  War3LightningSummary m_summary = {};
};

} // namespace dxvk::war3::render
