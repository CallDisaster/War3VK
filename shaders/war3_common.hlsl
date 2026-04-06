// war3_common.hlsl - War3 通用 HLSL 工具库
// 目标：提供可复用的颜色/雾效辅助函数

float3 ApplyFog(float3 color, float fogFactor, float3 fogColor) {
    return lerp(color, fogColor, fogFactor);
}

float3 ApplyTeamColor(float3 color, float3 teamColor, float mask) {
    return lerp(color, color * teamColor, mask);
}
