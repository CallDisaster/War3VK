#ifndef WAR3_WEATHER_DENSITY_GLSL
#define WAR3_WEATHER_DENSITY_GLSL

// Clip a forward camera ray to the weather box before bounded quadrature.
// Parallel axes are tested explicitly; division by zero never fabricates an
// infinite interval or hides a thin low-snow volume from the sample budget.
bool war3WeatherRayInterval(vec3 origin, vec3 direction, vec3 regionMin,
                           vec3 regionMax, inout vec2 interval) {
  if (any(isnan(origin)) || any(isinf(origin)) || any(isnan(direction)) ||
      any(isinf(direction)) || any(isnan(regionMin)) || any(isinf(regionMin)) ||
      any(isnan(regionMax)) || any(isinf(regionMax)) ||
      any(isnan(interval)) || any(isinf(interval)) ||
      any(lessThanEqual(regionMax, regionMin)))
    return false;
  for (int axis = 0; axis < 3; ++axis) {
    if (abs(direction[axis]) < 1.0e-7) {
      if (origin[axis] < regionMin[axis] || origin[axis] > regionMax[axis])
        return false;
      continue;
    }
    float a = (regionMin[axis] - origin[axis]) / direction[axis];
    float b = (regionMax[axis] - origin[axis]) / direction[axis];
    interval.x = max(interval.x, min(a, b));
    interval.y = min(interval.y, max(a, b));
    if (interval.y <= interval.x)
      return false;
  }
  return interval.y > interval.x;
}

// Z-up, bounded outdoor volume. Stable world coordinates are advected by the
// same cumulative wind used by the snow vertex shader; no camera history.
float war3WeatherSigma(vec3 p, vec4 regionMin, vec4 regionMax,
                      vec4 controls, vec4 motion) {
  if (any(isnan(p)) || any(isinf(p)) || any(isnan(controls)) ||
      any(isinf(controls)) || any(isnan(motion)) || any(isinf(motion)) ||
      any(isnan(regionMin)) || any(isinf(regionMin)) ||
      any(isnan(regionMax)) || any(isinf(regionMax)) ||
      any(lessThanEqual(regionMax.xyz, regionMin.xyz)))
    return 0.0;
  if (controls.w < 0.5 || any(lessThan(p, regionMin.xyz)) ||
      any(greaterThan(p, regionMax.xyz))) return 0.0;
  vec3 span = max(regionMax.xyz - regionMin.xyz, vec3(1.0));
  // Side edges and the top fade out. The bottom must not fade: low blowing
  // snow is densest there, so a symmetric Z feather erased its main layer.
  vec3 edge = vec3(min(p.xy - regionMin.xy, regionMax.xy - p.xy),
                   regionMax.z - p.z);
  float feather = clamp(regionMin.w, 0.0, 0.5);
  float mask = 1.0;
  if (feather > 0.0) {
    vec3 f = smoothstep(vec3(0.0), max(span * feather, vec3(1.0)), edge);
    mask = f.x * f.y * f.z;
  }
  // The CPU bounds accumulated wind by the same 8192 * 96 world-unit
  // period used for snow-cell identities. Integer wave numbers make each
  // axis exactly periodic, so reducing either displacement never jumps fog.
  const float WEATHER_WIND_PERIOD = 786432.0;
  const float WEATHER_TWO_PI = 6.283185307179586;
  vec2 q = (p.xy - motion.xy) * (WEATHER_TWO_PI / WEATHER_WIND_PERIOD);
  float band = 0.65 + 0.20 * sin(dot(q, vec2(375.0, 263.0))) +
               0.15 * sin(dot(q, vec2(-151.0, 711.0)) + motion.z * 0.4);
  float gust = mix(1.0, max(band, 0.1), clamp(controls.z, 0.0, 1.0));
  float low = exp(-max(p.z - regionMin.z, 0.0) / 96.0);
  return mask * gust * (clamp(controls.x, 0.0, 1.0) * 0.00055 +
                       clamp(controls.y, 0.0, 1.0) * low * 0.0015);
}

#endif
