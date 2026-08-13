# 2026-08-13 Froxel 全视域研究修复候选

## 输入与接入

作者提供研究模型直接修复的五个文件：`d3d9_war3_volumetric_light.cpp`、
`war3_volumetric_froxel_integrate.comp`、`war3_japi_v1.cpp`、
`d3d9_war3_shadow.cpp` 和 `d3d9_war3_settings.h`。接入前逐文件与共享脏工作树
比较，只应用这些精确增量；没有覆盖另一线程的 DXVK 设备优化文件。接入后源码
与输入文件的文本差异为零，项目大文件保留原有 CRLF/LF 结构。

公式和一手依据更新在
`docs/research/2026-08-13-warvk-common-froxel-and-shadow-intervals.md`。

## 研究修复内容

- 公共 Froxel 域由 `[20,6000]` 扩为 `[20,10000]`；volume-sun far ortho 同步为
  10000，旧 JASS quality distance 固定不能改写 Froxel far。
- Medium/High 深度由 48/64 调整为 64/128，grid cell budget 从 210 万调整为
  450 万；4K High 为 4,147,200 cells。
- grid XY 继续采用 32/16 tile，但最终 effect 采用 `1/8`/`1/4`，从首表面深度
  终止中解除 16×16/32×32 block 共享；High 在 4K 按主 DDA budget 自动降为 `1/8`。
- 输出 effect 像素先映射到完整 backing image，再映射到实际 D3D9 viewport；viewport
  外像素 fail-closed，避免非全屏 viewport 时错误拉伸 ray/depth guide。
- volume-sun 每个完整段优先 near layer，失败再用 far；相机 CSM 会继续查找更远层的
  完整覆盖。
- 阴影区间按 Beer-Lambert 光学积分加权；只在深度 transition 附近使用四点比较。
- 整 ray 主 DDA 上限调整为 Medium 1024 / High 2048；耗尽后每个 slice 以 4–64 个、
  约 32 世界单位的子段继续，不再退回每 slice 单中点。
- 主 DDA admission 上限为 350,000,000 pixel-steps；该数字不包含有界 fallback 和
  transition taps，仍要求玩家前台 1080p/4K GPU 时间门。

## 验证与部署边界

- `war3_volumetric_froxel_integrate.comp` 由 Vulkan SDK `glslangValidator` 编译通过。
- Froxel 静态合同：19/19。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- Win32 DLL 构建通过；`ninja -C build32 -n` no-work。
- DLL 为 PE32 x86，34,270,447 bytes，SHA-256
  `42438A7C60357E84E793DC9A2ACE331ED49B3B6B55C2CE3BF9A84013506DC394`。
- 部署前确认 Warcraft/YDWE/WorldEdit 均未运行；旧目标 SHA-256 为
  `974295790A87CAAE73D903C4D4450EA9297672C6263FCCFD08C204F23394BF01`。
- 旧 DLL 已备份为
  `E:/Work/War3/d3d9.dll.pre-froxel-full-view-20260813-191100-974295790A87.bak`，
  新 DLL 已部署到 `E:/Work/War3/d3d9.dll`，目标回读 SHA 与构建产物一致。
- 没有启动 Warcraft/YDWE；未完成玩家前台视觉或性能验收。
