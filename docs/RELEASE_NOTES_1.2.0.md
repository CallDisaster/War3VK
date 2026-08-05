# WarVK v1.2.0 Release

这是 WarVK 新版语义化渲染架构的首个正式公开版本，重点更新太阳/点光阴影、GPU 资源生命周期、
后处理和地图作者 JAPI。

## 主要更新

- 四级联太阳阴影默认使用 4096 分辨率，并采用锁存式 2048 分配失败回退。
- 收紧 PCF 过滤，改善过度模糊的阴影边缘。
- 加强树木透明度、蒙皮单位、刚体、建筑、地形和路径阻断器的 caster 正确性。
- 点光源、点阴影、体积太阳光、体积点光和全局高度雾接入正式运行时。
- 新增可选 TAA v2；发布默认仍为 DirectInline。
- Shadow Arena 使用事务式 bundle、GPU completion fence、map/device epoch 和最终 replay 验证。
- exact-index 有界批量读取修复了 write-combined 索引缓冲造成的明显 CPU 性能回归。
- WarVK JAPI 内置于 `d3d9.dll`，新增强类型数值通道、点光/光照时钟、闪电、数学表达式和曲线接口。
- YDWE Catalog 按功能分类，并为模式参数提供可点击选择项。

完整变更见 [CHANGELOG.md](../CHANGELOG.md)，安装与使用说明见
[README_CN.md](../README_CN.md) / [README.md](../README.md)。

## 运行要求

- Warcraft III 1.27a（32 位）
- Windows 10 或 Windows 11
- 支持 Vulkan 1.3 的显卡与最新官方驱动

当前构建实际请求 Vulkan 1.3。只暴露 Vulkan 1.2、但不支持 Vulkan 1.3 的旧驱动不能运行本版本。

## 已知问题

- 点光源开启点阴影后，部分地面和观察角度仍可能出现摩尔纹或带状伪影。遇到明显伪影时，
  可以保留点光源并关闭该光源的点阴影。
- 同一 Warcraft III 进程中退出地图后再进入其他地图，可能造成持续性能下降、阴影异常或
  其他资源生命周期问题。1.2.0 建议一次启动只游玩一张地图；换图前请完整退出并重启游戏。

以上两项问题延期到后续版本继续修复。
