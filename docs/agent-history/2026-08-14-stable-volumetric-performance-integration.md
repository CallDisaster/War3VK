# 2026-08-14 体积光与生产者性能稳定整合候选

## 交付身份

- 分支：`codex/stable-optimization-integration-20260814`
- 体积光父提交：`8502f298436261b6ed503225aed3adf57d3edc13`
- 性能父提交：`01996de613ea3c1f613a900f3d156a2e530c03c9`
- 合并提交：`0fd853883456c999e6a2a0efedcb34483b7c6bcd`
- AutoTest exact-owner publication 修正：`8a9c2df24b915dbb0d1a19dfa20db288d55077de`
- x86 性能历史上限修正：`01f0e73`
- Release DLL：`build32_release_final/src/d3d9/d3d9.dll`
- DLL SHA-256：`7305E21A55BD939C932FDDDFC4E162101B86E36E00A6021C41D10C4B0B9DBDA3`

该候选将 2026-08-13 的 Froxel/局部体积雾线与经过逐提交验证的阴影生产者 CPU 优化线合并；所有
实验性剔除 Consume、Persistent Package Consume、ReBAR、RTS shadow candidate 与开发 observer
在默认 Release 构建中保持关闭。

## 性能证据

同一 `ShadowTest/光影测试.w3x`、隔离桌面、High priority、`full_default`、每轮 30 秒执行
`A(volume-only)-B(integrated)-B-A`。A/B 每帧语义工作量接近：约 91--92 个 submitted draw、
39--40 个 skinned draw、52 个 terrain fallback；Arena 平均值仅相差 0.08%。两轮均值如下：

| 指标 | A 体积光线 | B 整合候选 | 变化 |
| --- | ---: | ---: | ---: |
| 主线程 CPU | 6.135 ms | 5.778 ms | **-0.357 ms / -5.82%** |
| Populate | 1.026 ms | 0.244 ms | **-76.27%** |
| DirectGrouped | 0.911 ms | 0.115 ms | **-87.42%** |
| BuildEligible | 0.760 ms | 0.028 ms | **-96.38%** |
| SnapshotPreselect | 0.067 ms | 0.054 ms | -19.55% |
| Arena 平均 | 27.361 MiB | 27.339 MiB | -0.08% |

这证明生产者 CPU 减税成立。不能把本组数据写成完整帧率提升：B 的总帧时间为 14.196 ms，A 为
13.719 ms，且 B 的 GPU 统计高 0.475 ms。两条分支不仅相差 CPU 优化，还相差 compare-first PCF、
receiver-plane per-tap 比较深度、Vulkan 生命周期与完整 publication 等正确性/安全修复；这些工作
不能为了 FPS 数字回退。前台绝对 FPS 仍需用户在同一物理相机下验收。

## 32 位诊断地址空间修复

首次整合候选在 `DXVK_WAR3_PERF_HISTORY_FRAMES=12000/45000` 时会在约 6--8 秒退出，而 4000
帧历史及合并前 DLL 可以持续运行。`FrameSnapshot` 除固定计数外还拥有动态 section/timestamp
容器；性能线扩大每帧诊断后，环境变量原先允许最多 200000 帧，会持续侵占 Warcraft III 32 位
地址空间。

修复在生产端统一将帧历史钳制到 4000，程序内 setter、frame 与 seconds 两种环境入口均不能绕过；
AutoTest 默认也由 7200 调整为 4000。累计 workload/预算统计仍覆盖完整运行时间。用修复 DLL
显式传入 `12000` 再跑 120 秒成功，报告恰保留 4000 帧，证明钳制在 DLL 内而非测试脚本绕过。

## 稳定性与正确性门

### 体积光测试图

- 120 秒，报告 4000 帧 / 57.743 秒窗口；
- 70.340 FPS，主线程 5.680 ms，GPU 3.503 ms；
- Arena 平均/峰值 27.347/29.531 MiB；
- incomplete、budget exceeded、last-complete reuse、partial、device lost 全为 0。

### Lost Temple 原版图

- 120 秒，报告 4000 帧 / 45.031 秒窗口；
- 90.608 FPS，主线程 4.703 ms，GPU 3.190 ms；
- Arena 平均/峰值 54.079/57.932 MiB；
- incomplete、budget exceeded、last-complete reuse、partial、device lost 全为 0。

### “生与死”低视角压力门

- 603.709 秒，1115 次 runtime sample、141 个 5x5 蛇形 waypoint、7 张内部截图；
- 峰值 `1196` caster、`1196` replay、`4784` 四级联 draw；
- Arena 峰值 `344633088` bytes（约 328.7 MiB），未提高 384 MiB/代际硬上限；
- Arena overflow、partial transaction、CSM fallback-to-last-good、日志 device lost 均为 0；
- 新 Windows GPU Event 153/4101 为 0，新 GPU incident 为 0；
- 精确句柄收尾、隔离桌面关闭与部署 DLL SHA 恢复均成功。

该地图的难度对话框没有被隔离桌面 `PostMessage` 真正关闭，因此本门证明 JASS 相机巡航下的
Caster/Arena/GPU 压力稳定，不等同于完整玩法或肉眼 UI 验收。

## 离线门

- 合并后全量离线：215/215 static、50/50 Win32 runnable、fresh Release `-j2` build 通过；
- x86 history 修复后 13 个定向 static 脚本全部通过；
- 定向 runnable：6/6 通过；
- `ninja -C build32_release_final -n src/d3d9/d3d9.dll` 为 no-work；
- `git diff --check` 通过。

## 发布边界

- 已证明生产者 CPU 显著下降和三类场景稳定性；未证明前台绝对 FPS 提升。
- Froxel/局部雾最终视觉、树叶细影抖动与跨地图完整发布仍需用户物理验收；不能由隔离桌面代替。
- 本候选没有启用 Issue #5 剔除 Consume；现有 observer 尚不足以授权前端 skip/freeze/Arena 省略。
- 本阶段不推送 GitHub，等待用户确认后再准备正式小版本发布。
