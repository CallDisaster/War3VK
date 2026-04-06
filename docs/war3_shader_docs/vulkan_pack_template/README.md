# War3 Vulkan ShaderPack Template

这是一个基础的 ShaderPack 模板工程，展示了如何自定义魔兽争霸3的后处理效果和阴影接收器。

## 📁 目录结构

- **pack.json**: ShaderPack 的配置文件，定义 passes、参数和元数据。
- **build.bat**: 一键编译脚本（需要 Vulkan SDK `glslc`）。
- **composite.frag**: 演示 Pass 0（包含简单的暗角/Vignette 效果）。
- **final.frag**: 演示 Pass 1（最终输出）。
- **shadow_receiver.frag**: (高级) 自定义阴影接收器。

## 🚀 快速开始

1. **安装工具**: 确保安装了 [Vulkan SDK](https://vulkan.lunarg.com/)，并将 `glslc` 添加到系统 PATH 环境变量。
2. **编译 Shader**: 双击运行 `build.bat`，生成 `.spv` 文件。
3. **安装**: 将本文件夹复制到游戏目录下的 `shaderpacks/` 子目录中（例如 `_retail_/shaderpacks/my_pack`）。
4. **加载**: 在游戏内按 `F7` 打开调试面板，在 "Vulkan ShaderPack" 中输入路径并点击加载。

## ⚙️ 参数配置 (pack.json)

`params` 字段允许您定义运行时可调参数（在 F7 调试面板中动态修改）。

```json
"params": {
  "0": [1.0, 0.5, 0.0, 0.0]  // 对应 Shader 中的 pc.u_params0
}
```

- `pc.u_params0.x`: 暗角强度 (Vignette Strength)
- `pc.u_params0.y`: 混合系数 (Mix Factor)

## 🎨 Shader 编写指南

Shader 主要通过 `u_colorTex` 获取当前画面，处理后写入 `o_color`。

```glsl
// 获取上一帧/上一Pass的结果
layout(set = 1, binding = 7) uniform texture2D u_prevPass; 

// 采样颜色
vec4 color = texture(sampler2D(u_colorTex, s_samplers[pc.u_samplerColor]), uv);
```
