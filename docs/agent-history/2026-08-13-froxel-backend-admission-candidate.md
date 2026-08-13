# 2026-08-13 Froxel 后端实际准入候选

## 玩家回归推翻的前提

玩家部署 SHA-256
`E14F7C60F904E6A89FFA03E2145F956C5E2E45E1A9AFE7463B28C0F4F7042D19`
后反馈画面完全没有改善。随后核对本次测试产生的
`E:/Work/Warcraft III/war3_d3d9.log` 与 YDWE 编译输出：

- 游戏目录和编辑器目录的 DLL 都是上述新哈希；不是替换错误。
- 日志中没有任何 `froxel submitted`、Froxel budget fallback 或 volume-sun
  提交记录。
- `outputwar3map.j` 只在第 1525 行调用
  `WarVKSetVolumetricEnabled(true)`。
- `WarVKSetVolumetricBackend` 在整份输出中只出现一次，且只是函数定义；调用数为零。
- 源码默认仍为 `War3VolumetricQuality::LegacyRayMarch`。

因此此前玩家画面仍由旧 fragment RayMarch 产生；扩大共同 Froxel Z、DDA、fallback
相位与 volume-sun bias 的所有修正实际上都没有执行。该证据否定的是“此前已经验收过
新路径”的前提，不否定玩家对画面问题的描述。

## 修改

- Volumetric Lighting 2.0 验收候选默认改为 `FroxelHigh`。地图作者只调用既有
  `WarVKSetVolumetricEnabled(true)` 时也会实际进入新路径。
- `LegacyRayMarch/FroxelMedium/FroxelHigh` 的 0/1/2 显式 JASS、ImGui 和环境变量
  选择仍保留，可用于 A/B 与兼容回退。
- pass 在体积光启用且相机/资源入口有效时记录实际 backend；玩家运行日志必须出现
  `DXVK War3Volumetric: active backend=2 (froxel-high)`，实际提交后还必须出现
  `DXVK War3Volumetric: froxel submitted backend=2 ... volumeSun=...`。

这是让前台 A/B 真正执行新实现的候选，不等于已经通过发布默认门；低视角、移动 Caster、
局部 Sphere/Box/Cylinder、1080p/1440p/4K GPU 时间、Reset 等物理门仍需实测。

## 验证与边界

- Froxel 静态合同：20/20。
- WarVK JAPI 集成静态合同：19/19。
- settings mailbox 静态合同：7/7。
- 全部 `AutoTest/test_*_static.py`：78/78 scripts。
- Win32 DLL 构建通过；`ninja -C build32 -n` no-work。
- DLL：34,270,585 bytes，SHA-256
  `66919DA545E0B55F625A728466E1119216C3A7F68438F215060F2F75DAF9F541`。
- 产物位于 `build32/src/d3d9/d3d9.dll`。
- World Editor 仍在运行；本候选未自动部署、未启动游戏、未声称完成视觉验收。
