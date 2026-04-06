// war3_test_triangle.hlsl - 屏幕空间旋转三角形测试
// 目标：vs_3_0 / ps_3_0

struct VS_INPUT {
    float4 position : POSITION;
    float4 color    : COLOR0;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 ndc      : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 gTime : register(c15); // x=时间（秒）

VS_OUTPUT main_vs(VS_INPUT input) {
    VS_OUTPUT output;
    output.position = input.position;
    float invW = (abs(input.position.w) > 1e-5) ? (1.0 / input.position.w) : 0.0;
    output.ndc = input.position.xy * invW;
    output.color = input.color;
    return output;
}

float2 Rotate2D(float2 p, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float Edge(float2 a, float2 b, float2 p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

float3 HueShift(float t) {
    return 0.5 + 0.5 * cos(t + float3(0.0, 2.094, 4.188));
}

float4 main_ps(VS_OUTPUT input) : COLOR {
    const float angle = gTime.x * 1.6;
    float2 p = Rotate2D(input.ndc, angle);

    const float2 v0 = float2(0.0, 0.55);
    const float2 v1 = float2(0.48, -0.28);
    const float2 v2 = float2(-0.48, -0.28);

    const float e0 = Edge(v0, v1, p);
    const float e1 = Edge(v1, v2, p);
    const float e2 = Edge(v2, v0, p);
    const float minEdge = min(e0, min(e1, e2));
    const float mask = (minEdge >= 0.0) ? 1.0 : 0.0;

    const float3 triColor = HueShift(angle + p.x * 2.2 + p.y * 2.2);
    return float4(triColor, mask);
}
