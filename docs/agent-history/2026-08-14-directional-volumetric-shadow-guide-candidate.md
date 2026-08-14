# 2026-08-14 独立方向体积阴影 Guide 候选

## 玩家回归与源码结论

玩家确认 Comparison PCF 后移动 Caster 仍会产生整格阶梯。对当前合并提交 `8502f29`
复核后确认：太阳阴影只在当帧二维 Integrate 中出现，High effect 为 `1/4` 且预算可降
`1/8`；Composite 只有 raw-depth range，不能识别同一接收面上的体积阴影边界；旧
可读性指数又在低分辨率阶段放大小覆盖。优化线程的 DXVK/Vulkan/StormBreaker 工作树
修改被明确排除，本候选只叠加在已经提交的体积光基线上。

## 修改

- Integrate 在写 RGBA16F effect 的同时写独立 `R16_SFLOAT` base directional guide，
  内容是受真实 CSM 区间与光学证据约束的物理遮挡；低覆盖区保持近似线性。
- 准入预算允许时创建 `1/2` refined guide。shader 先检查 base guide 3x3 min/max，
  平滑像素只重建，边缘像素才重新进行方向阴影区间积分；单 ray DDA 耗尽时回退完整
  base guide，不发布半条 ray。
- 可读性底图衰减从低分辨率 Integrate 移到 full-resolution Composite，仍以 24% 为
  上限；移除小于 1 的遮挡指数和 peak 主导的低覆盖扩张。
- Composite 由 full-resolution depth 反投影中心/四邻域接收面，以样本到接收切平面的
  距离保护真实几何断层，并用独立 refined guide 阻止跨体积阴影边缘混合；删除未消费的
  `referenceIndex`。
- Shadow DDA 改在 `textureGather` footprint 真正变化的 half-texel center 边界拆段，
  保持四个原始深度先比较、后合并。
- `R16F` sampled/storage 格式不满足时 Froxel fail-closed 回 Legacy；2D guide 的 cell、
  traversal、尺寸和单 ray 回退均有界。日志新增 `guide=WxH refineSteps=N`。

公式、同步和一手来源映射见
`docs/research/2026-08-14-warvk-directional-volumetric-shadow-guide.md`。

## 验证与边界

- Froxel 定向静态/数值合同：28/28。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Meson Win32 runnable：21/21。
- GLSL/SPIR-V 与 Win32 DLL 已由 build32 实际编译链接；`ninja -C build32 -n` no-work。
- DLL：34,299,295 bytes；SHA-256
  `7E383843E93A0695A893B8260457F16A42EB7D866DB96AB06E0D305BE6F035B3`。
- 未部署、未启动游戏、未完成玩家前台物理或性能门。候选也尚未实现三级 Volume Sun
  clipmap 或方向 guide 的独立 2D temporal；`10000/1536` 的远覆盖精度仍可能限制极小
  Caster。玩家应先在相同地图检查移动小单位、静止大 Caster、侧视/最低俯角、1080p/4K、
  日志 guide 分辨率/回退，以及关闭体积光后表面 CSM 完全不变。

## 玩家回归后的可读性修订

玩家确认首版 guide 候选的阴影柱几乎不可见。复核发现 Composite 在计算重建后可读性项前
沿用了“无散射且无消光即原图”的提前返回，因而 clear-air 场景会直接丢弃有效 guide；同时
仅保留整条视线平均遮挡使小 Caster 的长距离阴影被过度稀释。

当前修订把可读性项移到提前返回判定之前，只有散射、消光和 guide 衰减三者均为空才走原图
快路；Integrate 恢复旧路径已验证的光学峰值证据（Volume Sun 0.74、普通太阳 0.68），但仍只
写入独立 guide，并先经过 `1/2` 边缘细化与最终重建。默认权重、完整证据下可提供约 19% 的
底图衰减，硬上限仍为 24%；没有 CSM 光学证据时严格为零。

修订已重新通过 29/29 Froxel 定向合同、78/78 静态脚本、21/21 Win32 runnable、GLSL/SPIR-V
生成、32 位 DLL link 与 `ninja -C build32 -n` no-work。新 DLL 为 34,299,295 bytes，SHA-256
`D983C4887338B3D07D7312301183ACBB044941F1534E50592D10994D68596D71`。本线程未部署、未启动
游戏；这些离线结果不能替代玩家前台对清晰度、移动阶梯和性能的物理 A/B。上一节的
`7E3838...` SHA 仅保留为首版被否决候选的历史身份。
