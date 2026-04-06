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
  float3 color = tex2D(s_source, input.uv).rgb;
  if (g_exposure.y > 0.5) {
    color = SRGBToLinear(color);
  }
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
  float3 baseColor = tex2D(s_source, input.uv).rgb;
  float3 bloom = tex2D(s_bloom, input.uv).rgb;
  if (g_exposure.y > 0.5) {
    baseColor = SRGBToLinear(baseColor);
  }
  float3 color = baseColor + bloom * g_bloomParams.z;
  color *= max(g_exposure.x, 0.0);
  if (g_bloomParams.w > 0.5) {
    color = ACESFitted(color);
  } else {
    color = saturate(color);
  }
  if (g_exposure.y > 0.5) {
    color = LinearToSRGB(color);
  }
  return float4(color, 1.0);
}
