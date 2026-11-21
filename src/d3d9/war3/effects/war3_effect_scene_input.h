#pragma once

// From the data layer's state to the effect records of a frame
// (war3_effect_scene.h): the producer side. The in-process passes build their
// executor's record with these, and in proxy mode the data layer sends the
// same records to the x64 host, so both run an effect from one mapping.
//
// Values only; no device, no DXVK type.

#include "war3_effect_scene.h"

#include "../../d3d9_war3_settings.h"
#include "../render/war3_weather_runtime.h"

namespace dxvk::war3::effects {

inline scene::Ssao SsaoFromSettings(const War3SsaoSettings& ssao) {
  scene::Ssao record;
  record.radiusPx = ssao.radiusPx;
  record.strength = ssao.strength;
  record.bias = ssao.bias;
  record.power = ssao.power;
  record.fadeNear = ssao.fadeNear;
  record.fadeFar = ssao.fadeFar;
  return record;
}

// For a mode other than None: the record has no "off".
inline scene::Aa AaFromSettings(const War3AASettings& aa) {
  scene::Aa record;
  record.mode = uint32_t(aa.mode);
  record.fxaaQualitySubpix = aa.fxaaQualitySubpix;
  record.fxaaQualityEdgeThreshold = aa.fxaaQualityEdgeThreshold;
  record.fxaaQualityEdgeThresholdMin = aa.fxaaQualityEdgeThresholdMin;
  record.smaaThreshold = aa.smaaThreshold;
  record.smaaMaxSearchSteps = aa.smaaMaxSearchSteps;
  record.smaaMaxSearchStepsDiag = aa.smaaMaxSearchStepsDiag;
  return record;
}

// The post-processing chain. Bloom off is intensity 0: the composite still
// applies the exposure and the tone map.
inline scene::Bloom BloomFromSettings(const War3PostFxSettings& settings) {
  scene::Bloom record;
  record.threshold = settings.bloom.threshold;
  record.softKnee = settings.bloom.softKnee;
  record.intensity = settings.bloom.enabled ? settings.bloom.intensity : 0.0f;
  record.exposure = settings.exposure;
  record.acesToneMap = settings.bloom.acesToneMap ? 1u : 0u;
  record.useSrgb = settings.useSrgb ? 1u : 0u;
  return record;
}

// What the snow draw reads from the weather runtime's frozen snapshot.
inline scene::Snow SnowFromWeather(const war3::render::WeatherFrameSnapshot& weather) {
  scene::Snow record;
  record.snowfall = weather.params.snowfall;
  record.gust = weather.params.gust;
  record.haze = weather.params.haze;
  record.lowSnow = weather.params.lowSnow;
  record.minX = weather.region.minX;
  record.minY = weather.region.minY;
  record.maxX = weather.region.maxX;
  record.maxY = weather.region.maxY;
  record.bottomZ = weather.region.bottomZ;
  record.topZ = weather.region.topZ;
  record.edgeFeather = weather.region.edgeFeather;
  record.timeSeconds = weather.timeSeconds;
  record.windDisplacementX = weather.windDisplacementX;
  record.windDisplacementY = weather.windDisplacementY;
  record.seed = weather.seed;
  record.id = weather.id;
  return record;
}

} // namespace dxvk::war3::effects
