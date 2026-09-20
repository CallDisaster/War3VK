# Backbuffer LockRect 行为归因合同

只形成诊断证据，不宣称稳定优化。用户授权后台测试，但要求非交互隔离桌面与实际
2560×1440；当前前台另有负载，毫秒和百分比都受 GPU 竞争影响，不以此签收 FPS 收益。

## 本轮边界

- 保留 LockRect 参数/返回码/读回、GPU fence 与所有同步；不假返回成功，不跳过截图。
- `DXVK_WAR3_FRAME_TIMELINE=1` 下记录实际 backbuffer 创建 Flags、尺寸/格式，
  调用返回地址、锁定 Flags/HRESULT；所有配置/调用站点表32槽，溢出即分析失败。
- 逐完整 Present 间隔记录调用人口和 LockImage 墙钟；调用站点总表是进程累计，不与选中
  2000帧人口混用。Game+ED0F6 是已反汇编核对的调用站点，不授权更改其行为。
- 新建观察构建，Below Normal/-j2 exact DLL。先 full_default 观察；恢复后再运行
  同 DLL 的 render-disabled 对照（保留生命周期/诊断，关闭 hook.render、render.queue、
  shadow.capture/map/receiver/taa、postfx、ssao、aa、semantic.data）。不称后者原生D3D9。
- 每轮一个 fresh process、60秒、零全局输入、不切桌面；只在测量前内部捕获一次验证尺寸；
  不调用 camera.snapshot/JASS 摄像机查询，不挂起线程采样。
- 保留原有 ready、named-pipe、HASH身份、启动HANDLE清场和 video恢复合同。
  失败立即清场并恢复，不扩大为盲目重试；每轮新artifact目录，不覆盖旧证据。
- 每次部署前核 live 139B、candidate、新旧备份及用户地图11376D身份，结束恢复139B，
  不能恢复成旧055F或其它Water候选。遇到外部身份漂移停止、不覆盖。

## 根因判定

Microsoft规定未请求LOCKABLE_BACKBUFFER的后缓冲不可锁定，而本树因BlitGDI内部兼容
固定IsLockable=true。先确定游戏实际Flags；若实际已请求flag，此差异不能解释当前案例。
若完整/最小两组都有成功锁定，再区分增强工作的排队延迟与读回自身成本；不可把整个wait
算成可删除CPU开销。真正的原生D3D9 Flags/HRESULT仍需要独立取证，不能由最小DXVK替代。

来源：
- https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresentflag
- https://learn.microsoft.com/en-us/windows/win32/direct3d9/locking-resources

## 执行中证据与分支修订

R11 full_default完成并恢复：2000/2000帧从Game+ED0F6成功LockRect，Flags=1，
实际2560×1440。这与0EC6B0原生函数a2[11]=1（Present参数+0x2C）吻合，
不支持“游戏没申请lockable”这一假设。原生D3D9实际返回值仍未测试。

R12 render-disabled没有通过ready，perf recording=false/runtimeReady=false，
未形成性能样本；已恢复139B、关闭桌面、恢复video、零进程且无新增dump。
hook.render包含WorldFramePrepare/RenderState边界，禁用后原ready检测链不成立。
不修改ready标准、不冒充R12成功：另设R13 --consumers-disabled，仅从该禁用列表
移除hook.render，保留边界hook以维持真实ready证明；其余增强消费和semantic.data
依旧关闭。仍是诊断对照，不称纯DXVK、原生或完整的collection-only实验。

R13也未通过ready且已完整恢复。上一段仅是当时的待验证修订，不是根因接受。
本阶段停止更多模块组合实机。独立native probe创建设备INVALIDCALL，DXVK probe
像素循环后异常退出并有Vulkan swapchain OOM日志；移除FreeLibrary仍失败。
所有probe性能数据不接受，空dump无法支持栈归因；不向前台回退，不禁用用户负载。
后续先解决诊断环境/ready独立性和probe失败原因，再继续有效对照。
