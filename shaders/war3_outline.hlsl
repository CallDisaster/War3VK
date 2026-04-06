// war3_outline.hlsl - War3 轮廓描边材质
// 目标：vs_3_0 / ps_3_0

struct VS_INPUT {
    float4 position : POSITION;
    float3 normal   : NORMAL;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float4 color    : COLOR0;
};

float4x4 gWorld    : register(c0); // 世界矩阵
float4x4 gViewProj : register(c4); // 视图投影矩阵

float4 gOutlineColor  : register(c8); // 描边颜色 RGBA
float4 gOutlineParams : register(c9); // x=描边宽度（世界空间）

VS_OUTPUT main_vs(VS_INPUT input) {
    VS_OUTPUT output;

    float4 worldPos = mul(gWorld, input.position);
    float3 n = mul((float3x3)gWorld, input.normal);
    float3 normalW = normalize(n);

    worldPos.xyz += normalW * gOutlineParams.x;
    output.position = mul(gViewProj, worldPos);
    output.color = gOutlineColor;
    return output;
}

float4 main_ps(VS_OUTPUT input) : COLOR {
    return input.color;
}
