// A held projection can cover a different part of the current view. Test
// the actual clip volume of each published layer before sampling it.
float availableShadowVisibility(int mask, int count, vec3 worldPos, vec3 dx, vec3 dy,
                                float depth, float biasExtra, vec2 rot, float wallWeight) {
  int primary = count - 1;
  for (int c = 0; c < count; ++c) {
    if (depth <= ubo.u_splitFar[c]) { primary = c; break; }
  }
  float blend = max(ubo.u_params2.y, 0.0);
  float w = 0.0;
  if (blend > 0.0 && primary + 1 < count)
    w = smoothstep(ubo.u_splitFar[primary] - blend, ubo.u_splitFar[primary], depth);

  float sum = 0.0, weight = 0.0, fallback = 1.0, coverage = 0.0;
  for (int c = count - 1; c >= 0; --c) {
    if ((mask & (1 << c)) == 0) continue;
    vec4 light = vec4(worldPos, 1.0) * ubo.u_lightViewProj[c];
    if (!validVec4(light) || light.w <= 0.0) continue;
    vec3 p = light.xyz / light.w;
    if (!validVec3(p)) continue;
    float edge = min(min(1.0 - abs(p.x), 1.0 - abs(p.y)), min(p.z, 1.0 - p.z) * 2.0);
    if (edge <= 0.0) continue;
    float inside = smoothstep(0.0, max(8.0 * ubo.u_params.z, 0.015625), edge);
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5;
    float bias = (max(ubo.u_params2.x, 0.0) + max(biasExtra, 0.0)) *
        computeCascadeBiasScale(c, count, max(ubo.u_params4.w, 0.0));
    float ref = clamp(p.z - bias, 0.0, 1.0);
    float vis = 1.0;
    if (!isTerrainMaskedOccluder(uint(c), uv, ref)) {
      vec2 gradient = vec2(0.0);
      bool validPlane = computeReceiverPlaneDepthGradient(light, ubo.u_lightViewProj[c], dx, dy, gradient);
      float radius = computePcssRadius(uint(c), uv, ref, gradient, validPlane);
      radius = computeCascadePcfRadius(radius, c, count, max(ubo.u_params6.w, 0.0));
      radius = mix(radius, max(radius, 1.50), wallWeight);
      vis = sampleShadowPcf(uint(c), uv, ref, radius, rot, gradient, validPlane);
    }
    // Fill the part of the footprint absent at a finer layer with the
    // coarser sample. Only a boundary with no covering layer fades to lit.
    fallback = mix(fallback, vis, inside);
    coverage = max(coverage, inside);
    float nominal = c == primary ? 1.0 - w : (c == primary + 1 ? w : 0.0);
    sum += nominal * inside * vis;
    weight += nominal * inside;
  }
  return weight > 0.0 ? mix(fallback, sum / weight, min(weight, coverage)) : fallback;
}
