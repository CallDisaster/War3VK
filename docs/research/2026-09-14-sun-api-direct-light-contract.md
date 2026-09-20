# 太阳启停与体积效果执行证据合同

本轮基于 v1.21.00 独立集成树，不移植未完成 Water 分支。

## 太阳启停的独立实现依据

Microsoft 的 [D3DLIGHT9](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlight9)
将 Diffuse、Specular、Ambient 定义为独立光色；
[Ambient Lighting](https://learn.microsoft.com/en-us/windows/win32/direct3d9/ambient-lighting)
另行描述环境光贡献。关闭直接光不应顺带清除环境光或材质自发光。

本地已有 `D3D9DeviceEx::SetLight` 将原生 slot 0 作为主方向光入口。旧 `sun.enabled=false`
仅保留原生 `finalLight` 返回，而 JASS/GUI 声称关闭太阳，所以原生 diffuse/specular 仍然存在。
最小修复仅在该已选 slot 且原生类型为 DIRECTIONAL 时清零 Diffuse/Specular 的 RGB，保留
Ambient、alpha、方向及其它字段。POINT/SPOT 不抹除。开启路径不变，普通 native LightEnable
和 resource/sync 行为不改；自定义 WarVK 材质的 direct intensity 同步遵守开关。

正常太阳阴影强度与主阳光开关一致，点光总开关/阴影保持独立。调试 shadow modes 仍可显式
查看深度，不能把调试图当正常光照。不会将关闭太阳解释成关闭所有 ambient/emissive 或全屏变黑。
此为对原公开“关闭太阳”语义的修正，文档必须说明旧行为差异；没有修改 JASS wire/Shader API ABI。
手动方向与颜色仍分别受现有天体运动/时间色温 ownership 控制，不暗中关闭这些控制器。

## 实际执行与测试边界

Froxel 的旧日志在准入前打印 active backend，实际仅是 request。改为 requested，另由实际
Froxel/Legacy 分支在提交后报告 effective backend；只有 effect 与 composite 均成功才记组合提交。
缺场景、无光/介质、CSM未就绪、预算/资源失败分阶段报告；帧、地图/设备 epoch 与字段同一份快照。
这证明命令已录入，不是 GPU 已完成或画质已验收。

`DXVK_WAR3_VISUAL_API_DIAGNOSTICS=1` 才发布执行快照；默认路径不增加 mutex/atomic/QPC/日志。
执行栈上的少量诊断字段赋值仍存在，不把默认关闭称为完全零指令。
未抬高 watchdog 或更改 shader/积分/滤波算法，局部雾的固定 8 槽与生命周期合同保留。

内部 `jass.public_api` 只在既有受控游戏主线程测试队列执行，限定长度、公有命名空间、
三个 stock carrier，拒绝内嵌 NUL 和 carrier 所有权漂移；不向 Preloader 转发文件名。
它经过当前原生 bridge 与真实 backend，明确不等于执行地图 JASS 字节码。
`visual.snapshot` 的 author-pending 状态也不冒充已应用/已完成状态；必须结合新鲜执行帧和截图。

运行时要求非交互隔离桌面、2560×1440、零 global input、一个精确进程与已校验 candidate/backup。
测试使用 `E:/Work/War3` 并恢复原 A570 DLL；玩家 `E:/Work/Warcraft III` 的 B1FCC DLL 原样保留。
本轮功能测试不作为前台 FPS 或整个 v1.22 发布接受。

## 真实 JASS VM 暴露的独立缺陷

第一份 VM fixture 的 `WVKTypedReady()` 失败，但字符串回退/算式结果仍成功。其 DBWIN 证据：
`LoadInteger sig=(Hhashtable;II)I; argc=3 ret=4`、
`LoadReal sig=(Hhashtable;II)R; argc=3 ret=5`。产品期待字符串少了最后一个分号，
因此四个 typed carrier 的原子安装事务整体拒绝，公开函数静默转入兼容的字符串路径。
仅将这两个 1.27a 精确签名补为现场值；参数数 3、NUL-inclusive memcmp、可读区间、
其余 carrier 签名、事务回滚、非 capability 原样转发与 fallback 均不放宽。

修复后最终 VM 自己执行 typed 握手、point setters、real/int 返回，再显式选择字符串 fallback
比较同一标量结果；83 项断言全部通过。原生 carrier 自测本来不经过 JASS VM，不能发现这一层。
这项修复也让之前未启用的 Save/Load 拦截真正运行，仍需独立性能护栏；功能通过不推定性能收益。

## 作者最小用法（候选验证范围）

只创建局部雾不会自动打开整个体积 pass。地图进入且 `WarVKIsRuntimeReady()` 为 true 后，
可使用以下顺序；`fogId` 须保存在地图自己的变量中，结束使用时销毁：

```jass
call WarVKSetVolumetricEnabled(true)
call WarVKSetVolumetricBackend(2) // Froxel High request; still subject to admission
call WarVKSetGlobalVolumetricMediumEnabled(false)
call WarVKSetGlobalVolumetricFogEnabled(false)
call WarVKSetVolumetricDensity(0.0)
call WarVKSetVolumetricScattering(2.1, 0.96)
call WarVKSetVolumetricQuality(16, 1800.0)
set fogId = WarVKCreateSphereFogVolume(x, y, z, 650.0, 0.8, 0.2)
// Check fogId > 0 and WarVKGetLastErrorCode() == 0.
// Later: call WarVKSetFogVolumeEnabled(fogId, false)
// Finally: call WarVKDestroyFogVolume(fogId)
```

介质不是自发光物体；需要有效太阳或点光照明，本轮用太阳及有效 CSM。
太阳方向/颜色要手动固定时，分别关闭 `WarVKSetCelestialMotionEnabled` 和
`WarVKSetTimeColorGradingEnabled`；关闭太阳并不自动创建新的光源。
公开 void 包装不直接返回错误，作者应读 `WarVKGetLastErrorCode()`；18 是不支持的功能，
19 包含后端拒绝/无效句柄/容量等，不能把无报错弹窗理解为渲染已生效。
