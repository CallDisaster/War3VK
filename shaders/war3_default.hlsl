// war3_default.hlsl - War3 默认世界材质（可被外部覆盖）
// 目标：vs_3_0 / ps_3_0

#include "war3_common.hlsl"

struct VS_INPUT {
    float4 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float3 normalW  : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float  fogDist  : TEXCOORD3;
};

// VS 常量
float4x4 gWorld    : register(c0); // 世界矩阵
float4x4 gViewProj : register(c4); // 视图投影矩阵

// PS 常量
float4 gLightDir     : register(c8);  // xyz=方向光方向（世界空间），w=强度
float4 gLightColor   : register(c9);  // rgb=方向光颜色，w=高光指数
float4 gAmbientColor : register(c10); // rgb=环境光颜色，w=环境光强度
float4 gCameraPos    : register(c11); // xyz=相机位置
float4 gFogParams    : register(c12); // x=start y=end z=density w=mode(0/1/2/3)
float4 gFogColor     : register(c13); // rgb=雾颜色，w=是否启用(0/1)
float4 gTeamColor    : register(c14); // rgba=队伍色（a 作为混合权重）
float4 gTime         : register(c15); // x=游戏时间(0..24)

sampler2D gDiffuse : register(s0);

VS_OUTPUT main_vs(VS_INPUT input) {
    VS_OUTPUT output;

    float4 worldPos = mul(gWorld, input.position);
    output.position = mul(gViewProj, worldPos);

    float3 n = mul((float3x3)gWorld, input.normal);
    output.normalW = normalize(n);
    output.worldPos = worldPos.xyz;
    output.uv = input.uv;
    output.color = input.color;
    output.fogDist = length(gCameraPos.xyz - worldPos.xyz);
    return output;
}

float ComputeFogFactor(float dist, float4 fogParams) {
    float mode = fogParams.w;
    if (mode < 0.5) return 0.0;

    if (mode < 1.5) {
        float denom = max(fogParams.y - fogParams.x, 1e-4);
        return saturate((dist - fogParams.x) / denom);
    }

    if (mode < 2.5) {
        return saturate(1.0 - exp(-fogParams.z * dist));
    }

    return saturate(1.0 - exp(-fogParams.z * fogParams.z * dist * dist));
}

float4 main_ps(VS_OUTPUT input) : COLOR {
    float4 baseColor = tex2D(gDiffuse, input.uv) * input.color;

    float3 N = normalize(input.normalW);
    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gCameraPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float ndotl = max(dot(N, L), 0.0);
    float specPow = max(gLightColor.w, 1.0);
    float spec = pow(max(dot(N, H), 0.0), specPow);

    float3 ambient = gAmbientColor.rgb * gAmbientColor.w;
    float3 diffuse = gLightColor.rgb * gLightDir.w * ndotl;
    float3 specular = gLightColor.rgb * gLightDir.w * spec;

    float3 litColor = baseColor.rgb * (ambient + diffuse) + specular;

    float fogFactor = (gFogColor.w > 0.5) ? ComputeFogFactor(input.fogDist, gFogParams) : 0.0;
    litColor = ApplyFog(litColor, fogFactor, gFogColor.rgb);

    // 队伍色：使用顶点颜色的 alpha 作为混合权重
    float teamMask = baseColor.a;
    litColor = ApplyTeamColor(litColor, gTeamColor.rgb, teamMask);

    return float4(litColor, baseColor.a);
}
