# x64 D3D9 代理：StateBlock 交接

2026-10-29，继续“先完整接入 Warcraft，再检查 Storm 和客户端 OFF/ON 内存”的顺序。
本批不启动游戏、不部署玩家 DLL、不进行新的内存测量。

本批收口后，作者转交10-29评审，已把下一阶段改为先T1收益边界/T2传输可行性，
暂停下文原计划的RT/DS扩展；后续实际内存/等待测量见
[评审后回执](2026-10-29-x64-review-t1-t2-baseline.md)。本文件的StateBlock验收不变。

## 数据链与所有者

状态仍由 x86 Device 的唯一线程拥有；Begin/End 产生选择性状态集合，Apply 更新同一份
Device 状态，已有 draw/state 事务再把最终输入交给 x64 原 DXVK。没有新增宿主状态块、
wire opcode、第二套 GPU 退休或每个 setter 的同步 IPC。资源使用原 Child 私有引用，
公开 StateBlock 引用保活 Device；Device 内部未结束录制不反向持有公开 Device 引用。
Device 析构先清理未结束录制与绑定，再发送 Finish。

语义源为当前 `src/d3d9/d3d9_stateblock.cpp/.h` 和 `d3d9_device.cpp`。有限开发 profile
之外的接口继续拒绝，不能把整个 Device 内存快照当作 D3D9 捕获语义：

- PIXEL、VERTEX、ALL 的 render/sampler/shader/常量选择不同；PIXEL 不捕获纹理绑定。
- 纹理绑定、VB/IB、材质、变换、viewport/scissor 在预定义块中仅属于 ALL。
- 后续 Capture 更新 VB/stride，却保留块原有 offset；首次创建和显式录制保存 offset。
- 录制期 setter 更新所选字段，getters 仍返回 live 状态。原 MultiplyTransform 不经过
  recorder，本代理保留这个行为；SetTransform 则录制。
- null declaration 的 Apply 不修改当前声明；SetFVF(0) 保持 no-op，而相同非零 FVF
  仍要记入录制，不能被快路径吞掉。
- light 定义和 enable 变更分别选择；Capture 会复制目前所有已定义 light，但 enable
  只更新原选择的 bits。初次录制 false 不会变成禁用当前 live light 的指令。
- 只支持 stream 0、frequency 1；不把新增 StateBlock 说成已支持 instancing。

## 检查进度

生产候选已落盘；代理语法预检 r2/r3、初版夹具语法预检通过。r1 语法失败是缺少
`Unsupported<IDirect3DStateBlock9>` 特化，已补齐，失败日志保留。r3 的夹具预检命令
误把代理的 `-DNOMINMAX` 参数也传入自带宏定义的夹具，触发宏重定义；不是运行门通过。

`x64-stateblock-gpu-20261029-r1` 的 native-before 完成 245 检查 / 8 张新增图像，
但 remote-sync 在新增 private VB 用例失败：夹具误用了开发 profile 不支持的
managed VB。现改为已支持的 DEFAULT + WRITEONLY，保留整轮红门，不能据此接受 x64。
额外增加实际 shader/常量恢复及 SM3→FF 后，GPU r2 的 native-before 完成 246 检查 /
296 图，remote-sync 在无纹理常量色 shader draw 被拒。源码确认代理、宿主 resolver、
backend 三层仍要求 programmable draw 必须有 stage 0 纹理，这是原有限 profile 留下的
错误前置条件。现三层允许规范的 `{id=0,generation=0}`，由原 DXSO SpecSamplerNull
提供空采样值；非零资源仍检验完整 live/代际/upload 身份。新增同时采样空 s0/s7 图像。
GPU r3 又在相同 draw 失败：上一次修订遗漏 `DecodeDraw` 最前层仍要求非空纹理，
因此三处修订不等于完整接线。现解码器同样仅允许规范空纹理，增加两位宽所有 64 种
空引用组合 × programmable/fixed 两种解码模式的接受及不发布检查（额外 256 检查）。
VB/声明始终必需，programmable shader 仍必需；半空身份仍拒绝。
最终预期总体人口为 297 图 / 302 draws；r1/r2/r3 均保留失败状态。
GPU r4 的 remote-sync 已完成 11 张新增图像，但随后负例准备失败：夹具错误假定复用
VB 大于 4096 B。现用 GetDesc 取得真实 extent，在合法边界创建块，再 Capture 到
更短 VB；未修改生产范围检查。r4 仍是失败回执，r5 必须完整重跑。
## 两种管线 GPU 与生命周期验收

`x64-stateblock-build-20261029-r4` 构建/no-work、sources/protected 不变通过，BelowNormal/-j2。
宿主 `build64_render_host_stateblock_20261029_r4/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe`：
5,869,568 B，SHA-256 `80287B6368F54B5554970C0CD783FF01514A9D33D2DDB4112742F66898F3D7CE`。

`x64-stateblock-gpu-20261029-r5` 强制优化与 `r6` 默认管线均完整通过。每种模式原 x86
前/后、x64 同步/批量四端
各 297 张完整图 / 4,866,048 通道，hash 都为 `0627ed73c4fa582e`；远端各 302 draws。
StateBlock 246 项 native 检查、247 项 remote 检查，11 张新增图像；额外 remote 检查
为有限 profile 的 recaptured shorter VB 拒绝及合法恢复，不代表原生也拒绝相同输入。
纹理/VB/shader 只剩状态块私有引用、Capture 替换资源、未结束录制析构均有真实调用。
两位宽 wire 各 1,454,644 检查，shader token 各 6,560，原专门化与 sampler key 差分
各 200,000。既有 raw 非法事务/后续合法恢复仍通过，VVL365 六类检查激活，非状态
warning/error 为 0；资源归零、submitted=completed=retired、宿主自然退出通过。
两模式各自的 raw 门均为 97 draws / 72 rejected / 342 submitted=completed=retired。

`x64-stateblock-lifetime-20261029-r1` 通过：COM 96 轮 / 576 资源 / 256 Query /
319,488 像素，失败 0，最终 673 submitted=completed=retired、liveResources=0、
宿主自然退出。576 指公开 COM 引用释放，不冒充每次都立即物理回收；最后六个私有绑定
随 Device 正常清理。独立 raw 门为 61 个拒绝、48 次像素恢复、13 次 Query 恢复，
169,728 像素失败 0，162 submitted=completed=retired、liveResources=0。
source/reference/player 保持；无新增游戏、内存或性能测量。

这三个最终回执接受同一源身份和 80287B63 宿主；旧红门不追认。未改原 x86 参考 DLL
`26B411E99AAEEB2A53F064E1D221202915CE995C95742C1B9891659FD4C55A69`，本批没有冒用其
此前 119 项回归为一次新的全量运行。玩家 DLL 保持
`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。

复核还发现原代理 null VB 绑定会覆盖 offset/stride，与原实现不符；Capture 换成
较短 VB 后仍保留旧 offset，也必须在 Apply 前检查，避免 setter 拒绝后块却返回成功。
这些反例已写入同一 native/remote 夹具；候选已保留 null 绑定偏移、在修改任何状态前
预检较短缓冲，并逐项传播 Apply setter 的失败。修复和最终回执必须一起收口。

## 下一条交接边界

StateBlock 本身不保存 RT/DS，后续不能把它扩成万能快照。下一批以同一 Device/host
资源注册表承载具名 surface 和 texture mip 表面：先补 Get/Set/Create RT/DS，再补
StretchRect 与动态尺寸。绑定、Clear、Draw、读回必须使用同一目标身份；尺寸/格式/
采样兼容检查在整组绑定发布前完成，沿用 DXVK 在途资源跟踪。SetRenderTarget 导致的
viewport 变化也要与本地源实现对照。先验证 A→B→A 目标切换、纹理目标再次采样、深度
隔离和非法绑定后的合法恢复，再接 Reset/地图生命周期，不能只把固定 64 常量调大。

## 不外推范围

仍是 64×64、有限资源和状态的开发 profile。Game.dll guard 保持；真实 RT/DS、
StretchRect、动态分辨率、Reset/换图、更多拓扑/UP/完整 caps、WarVK 增强 pass 和
完整游戏仍未闭合。没有测量本批的完整帧时间或尾延迟，不能宣称已经追平原生性能。
最终 Storm requested/live/reserved 峰值、最高地址和客户端/宿主/GPU 内存对照，
继续等待完整接入，不用旧 TLS 减量替代迁移收益。
