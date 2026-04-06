// War3 重制版 示例着色器 (War3 Reforge Sample Shader)
// 目标: vs_3_0, ps_3_0

struct VS_INPUT {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 uv       : TEXCOORD0;
};

// 矩阵通常由游戏通过寄存器或 Uniform Buffer 提供
// 此示例假设单位矩阵或稍后处理
// float4x4 mWorldViewProj : register(c0);

VS_OUTPUT main_vs(VS_INPUT input) {
    VS_OUTPUT output;
    // 开始调试: 直接透传坐标 (适用于全屏四边形，3D 物体会变形但可见)
    output.position = input.position;
    // 结束调试
    output.uv = input.uv;
    return output;
}

float4 main_ps(VS_OUTPUT input) : COLOR {
    // 为了调试，返回纯红色
    return float4(1.0, 0.0, 0.0, 1.0); 
}
