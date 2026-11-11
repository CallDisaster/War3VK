#version 460
#extension GL_EXT_scalar_block_layout : require

// Matches SnowPushConstants exactly (128 bytes). Positions are world-space;
// the camera only selects nearby cells and projects them. No particle history,
// storage buffers, CPU simulation, or geometry-capture pages are needed.
layout(push_constant, scalar, row_major) uniform SnowPush {
  mat4 viewProj;
  vec4 regionXY;
  vec4 heightTime;
  vec4 windAnchor;
  uint seed;
  uint depthSampler;
  float cameraZ;
  uint packedMedia;
} pc;

layout(location = 0) out vec2 v_corner;
layout(location = 1) out vec3 v_world;
layout(location = 2) out float v_alpha;

const int CELL_COUNT = 32;
const int CELL_PERIOD = 8192;
const float CELL_SIZE = 96.0;

uint hash32(uint v) {
  v ^= v >> 16;
  v *= 0x7feb352du;
  v ^= v >> 15;
  v *= 0x846ca68bu;
  return v ^ (v >> 16);
}

float random01(uint v) {
  return float(hash32(v) >> 8) * (1.0 / 16777216.0);
}

void main() {
  const vec2 corners[6] = vec2[6](
    vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(-1.0,1.0),
    vec2(-1.0,1.0), vec2(1.0,-1.0), vec2(1.0,1.0));
  v_corner = corners[gl_VertexIndex];

  int particle = gl_InstanceIndex;
  ivec2 origin = ivec2(floor(
      (pc.windAnchor.zw - pc.windAnchor.xy) / CELL_SIZE)) - ivec2(16);
  ivec2 cell = origin + ivec2(particle % CELL_COUNT,
      (particle / CELL_COUNT) % CELL_COUNT);
  uint lane = uint(particle / (CELL_COUNT * CELL_COUNT)); // 16 per cell
  // Matching the CPU displacement period makes wrap preserve identities.
  uvec2 stableCell = uvec2(cell & ivec2(CELL_PERIOD - 1));
  uint identity = hash32(stableCell.x ^ hash32(stableCell.y) ^
                        hash32(lane + pc.seed));
  float r0 = random01(identity);
  float r1 = random01(identity + 1u);
  float r2 = random01(identity + 2u);
  float r3 = random01(identity + 3u);
  float r4 = random01(identity + 4u);
  float time = pc.heightTime.z;
  float height = max(pc.heightTime.y - pc.heightTime.x, 1.0);
  float phase = fract((float(lane) + r2) / 16.0 -
                      time * mix(65.0, 130.0, r3) / height);
  float gust = float((pc.packedMedia >> 24) & 255u) / 255.0;
  // Gust displacement is bounded. Persistent transport is supplied by the
  // runtime's continuous wind integral, not time * the current wind speed.
  vec2 wobble = vec2(sin(time * 0.7 + r3 * 6.2831853),
                    cos(time * 0.9 + r4 * 6.2831853)) * mix(3.0, 26.0, gust);
  vec3 position = vec3(vec2(cell) * CELL_SIZE +
                       vec2(r0, r1) * CELL_SIZE + pc.windAnchor.xy + wobble,
                       pc.heightTime.x + phase * height);
  v_world = position;

  vec2 regionSize = max(pc.regionXY.zw - pc.regionXY.xy, vec2(1.0));
  vec2 edge = min(position.xy - pc.regionXY.xy,
                  pc.regionXY.zw - position.xy);
  float feather = float(pc.packedMedia & 255u) * (0.5 / 255.0);
  vec2 regionFade = feather > 0.0
      ? smoothstep(vec2(0.0), max(regionSize * feather, vec2(1.0)), edge)
      : step(vec2(0.0), edge);
  float radius = length(position.xy - pc.windAnchor.zw);
  float distanceFade = 1.0 - smoothstep(1152.0, 1450.0, radius);
  float phaseFade = smoothstep(0.0, 0.035, phase) *
                    (1.0 - smoothstep(0.965, 1.0, phase));
  float threshold = random01(identity + 5u);
  float amount = smoothstep(threshold, min(threshold + 0.04, 1.0),
                            clamp(pc.heightTime.w, 0.0, 1.0));
  v_alpha = regionFade.x * regionFade.y * distanceFade * phaseFade *
            amount * mix(0.30, 0.65, r4);

  vec4 clip = vec4(position, 1.0) * pc.viewProj;
  // A world-size camera-facing flake: projected coefficients determine size;
  // particle centers remain anchored to world cells, never to screen pixels.
  vec2 projectedScale = vec2(length(pc.viewProj[0].xyz),
                              length(pc.viewProj[1].xyz));
  // Keep near flakes readable as snow rather than large soft cotton discs.
  // This is a world-size adjustment, not a change in density or GPU budget.
  float size = mix(1.0, 3.0, r4);
  float angle = r2 * 6.2831853 + time * mix(-0.4, 0.4, r1);
  vec2 rotated = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) *
                 v_corner;
  clip.xy += rotated * size * projectedScale;
  clip.y = -clip.y; // D3D world clip to Vulkan's positive viewport Y.
  // Masked instances are degenerate and produce no fragments.
  if (v_alpha <= 0.0001 || clip.w <= 0.0001)
    clip = vec4(2.0, 2.0, 2.0, 1.0);
  gl_Position = clip;
}
