#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

layout(set = 0, binding = 0) uniform sampler samplers[];
layout(set = 1, binding = 0) uniform texture2D waterDepth;
layout(set = 1, binding = 1) uniform texture2D waterInfo;
layout(set = 1, binding = 2) uniform texture2DArray sceneColor;
layout(set = 1, binding = 3) uniform texture2DArray sceneDepth;

layout(set = 1, binding = 4, scalar, row_major) uniform WaterUniform {
  mat4 invViewProj, viewProj;
  vec4 viewport, viewportZ, depthContract;
  vec4 camera, sunDir, sunColor, ambient;
} u;

layout(push_constant, scalar) uniform WaterPush {
  uint pointSampler, colorSampler;
  float waveTime, waveScale, reflection, specular, clarity;
} pc;

layout(location = 0) out vec4 o_color;

#define POINT samplers[nonuniformEXT(pc.pointSampler)]
#define LINEAR samplers[nonuniformEXT(pc.colorSampler)]

bool finite(float v) { return !isnan(v) && !isinf(v); }
bool finite(vec2 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool finite(vec3 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool finite(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }

vec3 unitVector(vec3 v, vec3 fallback) {
  float len2 = dot(v, v);
  return finite(len2) && len2 > 1e-10 ? v * inversesqrt(len2) : fallback;
}

bool usableDepth(float raw, float epsilon) {
  return finite(raw) && abs(raw - u.depthContract.x) > epsilon &&
      raw >= min(u.viewportZ.x, u.viewportZ.y) - 1e-5 &&
      raw <= max(u.viewportZ.x, u.viewportZ.y) + 1e-5;
}

bool worldPosition(ivec2 pixel, float raw, out vec3 world) {
  float span = u.viewportZ.y - u.viewportZ.x;
  float z = abs(span) > 1e-6 ? (raw - u.viewportZ.x) / span : raw;
  // Same integer pixel centre, D3D +Y up and row vectors as the receiver.
  vec2 uv = (vec2(pixel) - u.viewport.xy) / max(u.viewport.zw, vec2(1.0));
  vec4 clip = vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, clamp(z, 0.0, 1.0), 1.0);
  vec4 h = clip * u.invViewProj;
  if (!finite(h) || abs(h.w) < 1e-6)
    return false;
  world = h.xyz / h.w;
  return finite(world);
}

float groundDepth(ivec2 pixel) {
  return texelFetch(sampler2DArray(sceneDepth, POINT), ivec3(pixel, 0), 0).r;
}

vec3 rippleNormal(vec2 xy, float footprint) {
  // Crossed wave trains with unrelated world wavelengths and a common,
  // seamless time wrap. Attenuate subpixel slopes at distant grazing angles.
  float phase = pc.waveTime * (6.28318530718 / 256.0);
  vec4 angles = vec4(dot(xy, vec2(0.034, 0.013)), dot(xy, vec2(-0.015, 0.049)),
                     dot(xy, vec2(0.068, -0.037)), dot(xy, vec2(0.043, 0.117)));
  vec4 slopes = cos(angles + phase * vec4(31.0, -43.0, 59.0, -71.0));
  slopes *= 1.0 - smoothstep(vec4(0.45), vec4(1.8), footprint * vec4(0.036, 0.051, 0.077, 0.125));
  vec2 grad = slopes.x * vec2(0.076, 0.029) + slopes.y * vec2(-0.020, 0.064)
            + slopes.z * vec2(0.037, -0.020) + slopes.w * vec2(0.010, 0.028);
  return unitVector(vec3(-grad * pc.waveScale, 1.0), vec3(0, 0, 1));
}

// Classify a point on the reflected ray against layer zero of the scene.
// Return -1 outside the frustum, 0 in front or clear, and 1 behind geometry.
int probe(vec4 clip, vec2 imageSize, float epsilon, out vec2 pixel, out float sceneRaw) {
  if (!finite(clip) || clip.w <= 1e-6)
    return -1;
  vec3 ndc = clip.xyz / clip.w;
  if (!finite(ndc) || any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0)
    return -1;
  pixel = u.viewport.xy + (ndc.xy * vec2(0.5, -0.5) + 0.5) * u.viewport.zw;
  vec2 nearest = floor(pixel + 0.5);
  if (!finite(nearest) || any(lessThan(nearest, max(u.viewport.xy, vec2(0.0)))) ||
      any(greaterThanEqual(nearest, min(u.viewport.xy + u.viewport.zw, imageSize))))
    return -1;
  sceneRaw = groundDepth(ivec2(nearest));
  float span = u.viewportZ.y - u.viewportZ.x;
  float rayRaw = abs(span) > 1e-6 ? u.viewportZ.x + ndc.z * span : ndc.z;
  return usableDepth(sceneRaw, epsilon) &&
      abs(rayRaw - u.depthContract.x) <= abs(sceneRaw - u.depthContract.x) ? 1 : 0;
}

vec3 traceReflection(vec3 surface, vec3 ray, vec3 sky, float epsilon) {
  const int steps = 12;
  const float reach = 720.0;
  vec3 origin = surface + vec3(0, 0, 2.0);
  vec4 start = vec4(origin, 1.0) * u.viewProj;
  vec4 dir = vec4(ray, 0.0) * u.viewProj;
  vec2 size = vec2(textureSize(sampler2DArray(sceneDepth, POINT), 0).xy);
  float lo = 0.0;
  for (int i = 1; i <= steps; ++i) {
    float f = float(i) / float(steps);
    float hi = reach * f * f;
    vec2 hitPixel;
    float hitRaw;
    int side = probe(start + hi * dir, size, epsilon, hitPixel, hitRaw);
    if (side < 0)
      break;
    if (side == 0) {
      lo = hi;
      continue;
    }
    for (int j = 0; j < 4; ++j) {
      float mid = 0.5 * (lo + hi);
      vec2 pixel;
      float raw;
      if (probe(start + mid * dir, size, epsilon, pixel, raw) == 1) {
        hi = mid;
        hitPixel = pixel;
        hitRaw = raw;
      } else {
        lo = mid;
      }
    }
    vec3 hit;
    if (!worldPosition(ivec2(floor(hitPixel + 0.5)), hitRaw, hit))
      break;
    // Reject jumps behind foreground silhouettes, submerged terrain, and long
    // or off-screen paths smoothly. Compare thickness in Warcraft world units.
    float gap = length(hit - (origin + hi * ray));
    vec2 edge = min(hitPixel - max(u.viewport.xy, vec2(0.0)),
                    min(u.viewport.xy + u.viewport.zw, size) - hitPixel) / max(u.viewport.zw, vec2(1.0));
    float confidence = (1.0 - smoothstep(4.0, 18.0, gap)) * smoothstep(0.0, 8.0, hit.z - surface.z)
                     * (1.0 - smoothstep(420.0, reach, hi)) * smoothstep(0.0, 0.06, min(edge.x, edge.y));
    if (!(confidence > 0.0))
      break;
    vec2 uv = (hitPixel + 0.5) / max(size, vec2(1.0));
    vec3 color = textureLod(sampler2DArray(sceneColor, LINEAR), vec3(uv, 0), 0.0).rgb;
    if (finite(color))
      return mix(sky, color, confidence);
    break;
  }
  return sky;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  float surfaceRaw = texelFetch(sampler2D(waterDepth, POINT), pixel, 0).r;
  float epsilon = max(0.5 * u.depthContract.z, 1e-7);
  // The water image always clears to 1. The scene's far endpoint can reverse.
  if (surfaceRaw == 1.0 || !usableDepth(surfaceRaw, epsilon))
    discard;
  float groundRaw = groundDepth(pixel);
  if (!usableDepth(groundRaw, epsilon) ||
      abs(surfaceRaw - u.depthContract.x) <= abs(groundRaw - u.depthContract.x) + epsilon)
    discard;
  vec2 info = texelFetch(sampler2D(waterInfo, POINT), pixel, 0).rg;
  vec3 surface, ground;
  if (!finite(info) || !worldPosition(pixel, surfaceRaw, surface) || !worldPosition(pixel, groundRaw, ground))
    discard;
  float shore = smoothstep(1.0, 9.0, min(info.y, surface.z - ground.z));
  if (!(shore > 0.0))
    discard;

  vec3 V = unitVector(u.camera.xyz - surface, vec3(0, 0, 1));
  float footprint = length(u.camera.xyz - surface) / max(u.viewport.w * max(V.z, 0.05), 1.0);
  vec3 N = rippleNormal(surface.xy, footprint);
  vec3 R = reflect(-V, N);
  // The receiver rotates sunDir into lightDirV; CSM places its light camera
  // upstream along -sunDir. The half-vector needs surface-to-sun.
  vec3 L = unitVector(-u.sunDir.xyz, vec3(0));
  float visibility = clamp(info.x, 0.0, 1.0);
  float ndotl = max(dot(N, L), 0.0);
  vec3 sunlight = clamp(u.sunColor.rgb, 0.0, 4.0);
  vec3 ambient = clamp(u.ambient.rgb, 0.0, 1.0);
  vec3 sky = (vec3(0.07, 0.115, 0.16) + 0.22 * ambient + 0.065 * sunlight)
           * mix(1.0, 0.7, clamp(R.z, 0.0, 1.0));
  float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 5.0);
  float reflection = pc.reflection * fresnel;
  vec3 reflected = sky;
  if (reflection > 0.0 && R.z > 0.0)
    reflected = mix(sky, traceReflection(surface, R, sky, epsilon), smoothstep(0.0, 0.08, R.z));

  float body = 0.22 * (1.0 - exp(-max(info.y, 0.0) / mix(55.0, 200.0, pc.clarity)));
  vec3 authored = texelFetch(sampler2DArray(sceneColor, POINT), ivec3(pixel, 0), 0).rgb;
  float direct = clamp(dot(sunlight, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
  vec3 tint = authored * vec3(0.46, 0.62, 0.70) * (0.72 + 0.28 * ndotl * visibility * direct);
  float highlight = pow(clamp(dot(N, unitVector(V + L, vec3(0))), 0.0, 1.0), 96.0);
  vec3 glint = sunlight * (0.55 * pc.specular * visibility * ndotl * highlight);

  float bodyWeight = (1.0 - reflection) * body;
  vec4 contribution = shore * vec4(reflected * reflection + tint * bodyWeight + glint,
                                    reflection + bodyWeight);
  if (!finite(contribution))
    discard;
  o_color = contribution;
}
