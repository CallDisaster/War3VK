# “生与死”单地图夜间稳定性与性能门（2026-08-07）

## 交付边界

- 只验证冷启动后进入单张地图，不覆盖同进程跨地图。
- 默认可见桌面运行；未使用 isolated desktop。
- CSM 保持 4096，TAA 发布默认保持 DirectInline，Arena 容量不变。
- 点阴影摩尔纹、CPU-MT 蒙皮、canonical queue takeover 不在本轮实施范围。
- 全夜未发生 TDR，允许的 3 次 TDR 配额实际使用 0 次。

## 安全基线

- 夜间开始前部署 DLL：`249166E6F464FBB17089CC2CEEEFB0EF928A2D7213FC0A9C2443FB84049262A5`
- 回退副本：`E:\Work\War3\d3d9.dll.bak_20260806_pre_night_249166E6`
- 测试地图 SHA-256：`548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F`
- `War3.exe` SHA-256：`EA8F5192EFEDE23F84BC60140C2D4A0085EA68B86F430C1D60C354A555922DEF`

## 本轮默认生效的修复

1. AutoTest 的 `life_and_death_tdr` 改为严格绑定 AutoTest 自有的默认可见桌面进程/窗口；isolated desktop 继续拒绝。
2. receiver pass 使用独立的 terminal publication。后续 pre-receiver 或 command-tail 的不可变场景副本只能更新 producer 统计，不能把已完成的 receiver/CSM 状态覆盖为零。
3. `ResetShadowRuntimeBridgeState` 同时清理 receiver terminal diagnostic state，避免会话状态残留。
4. Direct packet geoset 快取命中必须匹配当前 `mapEpoch` 和权威 `immutableModelGeneration`；旧地图或同地址替换不能命中旧快照。

## 性能路线裁决

- Compact WorkTable：Observe mismatch 为 0，但 Consume A-B-B-A 没有稳定达到 `0.15 ms/frame`，保持 Off。
- Persistent Package：约 195 万次观察中约 98.8% 为非 rigid/static，exact CSM 使用机会约 0.59%，远低于 95% 静态命中门，保持 Off。
- 联合消费者剔除：5,564 个观察帧、22,216 个严格候选，C2/C3 `wouldCull=0`，机会率 0%，保持 Off。
- generation-backed CPU index-slice cache：命中约由 1--2 次/帧提升到约 54 次/帧，`AllocateCopy` 由约 0.026--0.029 ms 降至约 0.005--0.006 ms，但总净收益只有约 0.02--0.025 ms，低于产品门；代码保留同 DLL A/B 能力，默认 Off。
- “生与死”没有点光阴影，不能证明 point-shadow worker 转正，保持现有默认。

## 验证结果

- 静态合同：504/504。
- Win32 Meson runnable：16/16。
- Win32 DLL 构建通过；`ninja -C build32 -n` no-work；`git diff --check` 通过。
- receiver terminal 修复短门：172 个样本中 receiver 全零由此前观察到的瞬时值降为 0。
- DirectInline 三轮 10 分钟：
  - `AutoTest/artifacts/life_and_death_tdr/20260807_020537`：601.779 秒，1,132 样本。
  - `AutoTest/artifacts/life_and_death_tdr/20260807_021653`：601.962 秒，1,133 样本。
  - `AutoTest/artifacts/life_and_death_tdr/20260807_022803`：602.675 秒，1,134 样本。
- TAA v2 一轮：`AutoTest/artifacts/life_and_death_tdr/20260807_023956`，602.040 秒，1,132 样本；终态 requested/effective=`2/2`、shader mode=`3`、history generation=`17045`、valid/readable=`1/1`。
- 四轮长门的 receiver 全零、replay/Arena/frame-incomplete/cross-epoch 异常、GPU incident、Event 153/4101 均为 0；每轮记录 12 个截图采样点。

## 阶段候选

- build32 与已部署 DLL SHA-256：`9FE2F6132015D6BF5413B844915187F80D9F53E14F204564890CD9E71E12AED3`
- DLL 大小：33,851,470 bytes。
- 该结果不证明跨地图问题或点阴影摩尔纹已经修复，也不把 AutoTest 的后台/窗口化 FPS 当作玩家前台发布性能。
