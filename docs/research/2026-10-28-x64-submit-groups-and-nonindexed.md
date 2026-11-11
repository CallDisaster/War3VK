# x64 提交合并、非索引绘制与成本对照

2026-10-28。继续作者的性能与覆盖工作。这里只记录受限公共 COM → x64 原 DXSO/DXVK
开发路径；没有接管 Warcraft 整幅画面，没有修改玩家 DLL，没有提交或发布。

## 判断

合并 GPU 提交有收益，定宽 wire 编解码也已收紧；仍未达到本地 x86 性能。
批内重复状态省略通过正确性测试，但没有稳定性能收益，保持默认关闭，不能计为性能成果。
所有模式均是开发 profile；这些数字不代表游戏 FPS、完整迁移或玩家视觉验收。

## 本轮实现与所有权

- `WARVK_D3D9_GPU_GROUP=4` 在现有四份常量 backing 内，把最多四个 32-draw 消息录入
  同一原 DXVK command list。1,025 draw 的绘制提交从每帧 33 次减至 9 次。
  仍是四槽 CPU 队列、最多四个 GPU 常量块，实测上限 1 MiB，没有扩大预算。
- `EndScene` 通过新的 `FlushDraws` 操作提交不足四批的尾部。Query/Readback/上传/
  销毁/Finish 的原有有序路径也先提交尾部。冷诊断遇到尚未提交的 GPU 组直接拒绝，
  不得隐式改变执行进度。
- 组提交的 CPU 成功见证只在原提交线程实际返回成功后传播；GPU timeline 与 DXVK
  resource-use retirement 仍分别核验。下一组不能覆盖上一组仍被使用的 backing。
- 新增真实 `DrawPrimitive(TRIANGLELIST)`：规范的空 IB 身份只表示非索引绘制，
  VB/纹理/VS/PS/声明的身份与代际仍必须完整。按初始化 VB 范围验证后，调用原 DXVK
  非索引 draw；没有生成伪 IB，也不依赖当前绑定的 IB。
- Draw/DrawBatch 复用同一个宿主资源解析入口。整批预检成功前不记录前缀命令；
  非索引首顶点溢出、错误 base/min/count 等同样按整批拒绝。
- 固定 32/64 位 wire 读写改为内联 little-endian 字节操作，保留未对齐支持和原字节布局。
  用旧变量宽度 codec 对 4,096 个全宽值、7 种偏移、边界 guard 做独立对拍。
- `WARVK_D3D9_BATCH_STATE=1` 是保留的默认关闭对照实验：只比较同一已验证批次内
  相邻 draw 的输入布局、光栅、深度与混合等状态。每批第一条完整设置；常量、资源、
  viewport 和逐 draw 校验不省略。没有跨帧缓存或新生命周期所有者。

相关实现集中在 `tools/render_host/d3d9_{connection,proxy,wire,draw_wire}` 与
`tools/render_host/dxvk_core/d3d9_{backend,host_main,batch_state}`。未改共享渲染核心或
游戏 DLL 的当前产品实现，因此本轮不继承或重新声称产品 Meson/真实地图通过。

## 对照数据和测量边界

计时是 64×64 目标、每轮 4 个预热帧加 40 个测量帧、Clear → EVENT GPU 完成。
逐帧读回与像素检查在该计时窗口外。另采整个 44 帧工作量的父/宿主 CPU 时间和
`QueryProcessCycleTime`（包含预热、读回和检查），绑定宿主 PID + 创建时间及保留句柄。
周期计数不换算成固定频率时间。`GetProcessTimes` 观察到 15.625 ms 台阶，短窗口的
零差分不等于零 CPU 成本。系统 busy 也记录，但没有证明整台机器无其他竞争负载。

最终 r3 宿主、整面 Clear 初始化修正后的提交合并对照（高 draw r6 / 低 draw r7）：

| 每帧 draw | 本地参考中位数 ms | 逐批 GPU 提交 ms | 合并 GPU 提交 ms | 双进程 CPU 周期变化 |
|---|---:|---:|---:|---:|
| 97 | 0.161 / 0.132 | 0.598 / 0.577 | 0.456 / 0.463 | 213,775,618→151,071,019 / 200,745,174→149,178,738 |
| 1,025 | 0.717 / 0.850 | 4.170 / 4.054 | 3.612 / 3.758 | 1,074,281,161→756,236,330 / 1,082,273,203→753,378,935 |

相邻配对：97 draw 中位数约减少 20–24%，1,025 draw 约减少 7–13%；CPU 周期分别约
减少 26–29% / 30%。尾部没有全面改善：97 draw 第一合并轮 p99 2.498 ms（对应控制
1.182 ms）；1,025 draw 第一合并轮 p99 6.082 ms（控制 5.783 ms）。不晋升性能接受。
这一组选择 `BATCH_STATE` 关闭，未把无稳定收益的状态实验计入优化结果。

以下保留定宽 codec 候选 r2 的较早同轮 1,025-draw 对照，不能与上表拼成一次测试：

| 模式 | 完成中位数，两轮 ms | 完成 p99，两轮 ms | 44 帧双进程 CPU 周期总数 |
|---|---:|---:|---:|
| 原四批在途、逐批 GPU 提交 | 4.322 / 4.374 | 6.005 / 8.140 | 1,086,486,100 / 1,129,707,117 |
| 合并四批 GPU 提交 | 3.889 / 3.799 | 5.511 / 4.698 | 788,956,960 / 767,981,121 |

相邻配对中位数约减少 10% / 13%，CPU 周期总数约减少 27% / 32%。这是同轮受限
工作量的观测，不能用不同日期、不同系统负载的旧 20–50 ms 数字计算累计提升。

r3 宿主、修正 Clear 用例后的批内状态省略对照（r5）：

| 每帧 draw | 本地参考中位数 ms | 合并提交、状态省略关 ms | 合并提交、状态省略开 ms |
|---|---:|---:|---:|
| 97 | 0.150 / 0.154 | 0.545 / 0.494 | 0.457 / 0.530 |
| 1,025 | 0.672 / 0.777 | 3.696 / 3.682 | 4.117 / 3.764 |

1,025-draw 开启状态省略实际跳过 306,744 次设置，但没有变快，尾延迟也未改善。
**不会因“少做了调用”就认定优化成立**。暂不启用该实验。97-draw 控制组还出现一次
8.662 ms 尾部样本；40 样本 p99 实际是最大样本，不是长时尾延迟认证。

## 失败回执与覆盖修正

`x64-group-state-stress-20261028-r3/r4` 的本地对照像素失败原样保留。新用例期待整面
Clear，却沿用了上一 tile 的小 viewport。D3D9 Clear 仍受当前 viewport 限制，因而
深度/混合/写掩码保留的是旧内容，而非期待的清屏色。r4 增加有界像素差异记录，
r5 在 Clear 前显式设置整个目标 viewport，本地参考与远端均通过，未放宽像素容差。

本轮验证的开发宿主仍只覆盖整面 Clear；当前宿主 Clear 没有传入 viewport/矩形域。
**子视口/矩形 Clear 的完整等价仍是已知缺口**，不能把修正测试初始化写成实现了它。
这是接入通用游戏前必须补齐的一项 API 语义；当前 Game.dll 保护继续保留。

CPU reader 的固定 worker 清单曾未列入新增 EndScene 用例，产生 1 个失败；补入确切
用例名后 21/21 通过，未删除严格清单检查。没有用后来绿色追认之前失败回执。

`x64-group-final-gpu-20261028-r6` 的本地程序 exit 0、像素通过，但 reader 把短窗口
`GetSystemTimes` 无增量误当失败。回执保留；reader 改为明确输出 unavailable/null 和
`counterDidNotAdvanceInShortWindow`，新增对应测试。没有把它解释成系统空闲，也不
允许伪造 CPU 周期或忽略非单调/负数。r7 全部通过。

## 当前回执

根目录 `AutoTest/artifacts/`：

- `x64-group-state-build-20261028-r3/receipt.json`：fresh x64 build/no-work，BelowNormal/-j2。
- `x64-group-state-cpu-20261028-r3/receipt.json`：x86/x64 各 210,608 Connection 检查、
  各 116,079 wire/批内状态判定检查。CPU fake 回复不当作 IPC/GPU 完成证明。
- `x64-group-state-gpu-20261028-r5/receipt.json`：97 draw，9 个实际 GPU 场景通过。
- `x64-group-state-stress-20261028-r5/receipt.json`：1,025 draw，9 个实际 GPU 场景通过。
- `x64-group-final-stress-20261028-r6/receipt.json` 与 `x64-group-final-gpu-20261028-r7/receipt.json`：
  上表最终提交合并对照，高/低 draw 各 9 场景通过，状态实验关闭，采样像素本地/远端一致。
- `x64-group-vvl-20261028-r5/receipt.json`：固定 VVL365 的 3 个 GPU 场景通过，不能用于性能结论。
- `x64-group-event-fault-20261028-r3/receipt.json` 与 `x64-group-batch-fault-20261028-r3/receipt.json`：
  实际 x86→x64 管道的 S_FALSE/非法 payload/nonce/peer exit/半 header，两个模式各 5/5 通过。
- 三个纯 reader：批次/CPU 成本 9/9、CPU 进程证据 21/21、IPC 故障 32/32。
- 对象与像素：32 个非索引 draw、64 个邻接深度/混合/写掩码切换；129 个独立可见
  tile 覆盖四槽循环后的老批数据，纹理更新前后分开检查。29 组原错误批次、六类 ID
  销毁换代、12 组非索引错误位置和后续合法恢复都经过真实 GPU 检查。

最终宿主：`build64_render_host_groups_state_20261028_r3/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`，
5,558,272 B，SHA-256 `3ACCB3567E9711EE611EB2540F25F8619F26850DF3DBC70CD2F92F9A2FC5666C`。
这是实验宿主，不是可替换玩家 `d3d9.dll` 的发布包。

## 接下来的门

1. 优先减少每帧 IPC 事务及重复状态载荷。GPU 提交合并没有减少 1,025 draw 的
   33 个 DrawBatch 消息及逐消息往返；按值交接、身份与原有 GPU 退休证明不能删。
   下一方案应在固定上传窗口内编码重复状态/常量，并保持整批原子预检和错误后恢复。
2. 在同一确定工作量下同时验证 CPU 周期、完整完成延迟、背压和长时 p95/p99；
   不只观察“提交次数”，不把 GPU submit ACK 当 GPU 完成，也不靠增大无限在途量掩盖成本。
3. 完成功能主线：Factory 能力协商、原 FF 完整状态/着色器、纹理多 mip/RT/DS、
   StateBlock、子视口/矩形 Clear。非索引 triangle-list 的本轮通过不代表所有拓扑与 UP 路径。
4. 上述边界闭合后才进入真实游戏 x86/x64 同场景等价、画质/内存/性能和退出换图验证。

玩家 `E:/Work/Warcraft III/d3d9.dll` 保持 SHA-256
`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。
