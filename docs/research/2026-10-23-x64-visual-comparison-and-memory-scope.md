# 32 位本地与 64 位宿主：真实读回对比及内存收益边界

日期：2026-10-23。回应作者的“魔兽减少多少内存、两条路径对比图”请求。

## 结论

本轮同一 D3D9 测试程序、同一几何/纹理/Shader 分别运行两条路径，真实 GPU 读回的
4,096 个像素、16,384 个 BGRA 字节完全相同（包含 alpha）。这是 **64×64 合成测试场景**，
不是 Warcraft 地图，也不是两个真实呈现窗口的截图。

目前未把 Warcraft/Game.dll 的完整渲染工作负载接入该 x64 宿主，玩家 DLL 未变。
没有可认证的“魔兽节省 X MiB / X%”数字；这不是测量出零，而是游戏收益尚未测量。
本轮未测性能或内存，也不据旧微型实验宣称完整渲染负载已迁移。

## 真实图像与执行路径

展示图：
`AutoTest/artifacts/x64-visual-compare-20261023-r2/presentation/native32-vs-host64-comparison.png`

- 左：2026-10-22 保存的 32 位进程内 WarVK/DXVK 参考 DLL，不是 Windows 系统原生 D3D9。
- 右：同一 x86 API 测试程序 → 正式开发代理 → 当前 x64 DXSO/DXVK 开发宿主。
- 两者各自在真实 Restricted Medium 非输入 Win32 Desktop 顺序运行；不切换输入桌面、不发全局输入。
- 测试场景包括三个 indexed draw、纹理、深度保护及 alpha 混合；每条路径独立 oracle
  检查 2,548 个安全像素，均 0 失败、最大通道误差 0；完整图另做逐字节比较，差异 0。
- 正常退出、Job 为空、Desktop 关闭且输入桌面始终 Default。右侧 VVL 1.4.365
  实际启用及同步校验已验证，warning/error 均 0；不声称左侧 VVL 已验证。

正式 fixture 不变：测试专用副本只在成功 `GetRenderTargetData/LockRect` 后、
`UnlockRect` 前按实际 Pitch 导出所有 BGRA 字节。唯一插入块可逆变换回正式源码；
同一个编译产物跑两边。固定 `capture.bgra` 使用 CREATE_NEW，禁止覆盖已有证据。

原始尺寸 64×64。展示图使用最近邻 8 倍放大，并忽略 alpha 展示不透明 RGB；
原始 BGRA 和 `*-rgba.png` 保留 alpha（本例范围 191–255），PNG 回读逐字节校验。
展示图中的两块内容也与原始 RGB 最近邻放大结果逐字节对照。没有 AI 生成场景或假窗口边框。
视觉检查：已查看最终 1440×1040 并排图，标题/两侧标签/完整画面及证据限制均可读，无裁切。

重要限制：reference 不请求 Present，remote 的 `--present` 按 offscreen 合同检查
E_NOTIMPL。不要把它称为实际双窗口 Present、桌面合成器像素或游戏视觉验收。
已有独立 Presenter 门不与此离屏证据混为同一次验收。

## 回执与身份

主回执 `AutoTest/artifacts/x64-visual-compare-20261023-r2/receipt.json`：
744,647 B / `FCB3973B36B08CDB1A78AAF02AFB6379A8FDB4967A0C9ACC6DEB86E170FA9727`。

| 产物 | SHA-256 |
| --- | --- |
| 32 位参考 DLL | `6805CC0B07DF1D1F3314ACCC5A18CF5342823D705E02FCA367F16ED3D99DF3DE` |
| x86 proxy | `C588010C285D9F079A8A6C224F7365149A73231F1AA7B5F5771C6DA26B109A32` |
| x64 host | `C58F020DBFEF5EC2487DECAAC610AC8BBFBCB63037965CAA2E818B4BDEE19623` |
| 两个独立 capture.bgra（各 16,384 B） | `AA5812968D9AE2D5B2A1F0F2C092B4D1302661653026F7B978797ABFD7A1B0C3` |
| 并排 PNG | `9E05DA8B473E71CC85A3FD75C7371B6E2CCF3A0E6C019390B2CF0141F5B1A4D4` |

两条实际读回的 FNV 均为 `97937de3dc0c0c6d`，但本轮不只比较摘要，也比较所有原始字节。
独立只读审查重放了原始读方，复核 1,169 项冻结输入与 4 份 raw 清单，确认限定结论通过。
采集合同纯 Python 测试 20/20 通过。只编译测试副本，未重建或改动正式渲染器。

首轮 r1 必须保留失败：汇总器错误地要求 host 只有一条 JSON，当前 host 还会输出实际
Desktop 身份记录，因此 `ok=false / one complete host summary`。该轮原源码、输出和回执保留，
不追认通过。新增 r2 runner 明确要求两条固定顺序/schema，核对 host PID、peer PID、
WinSta0、非输入 Desktop、beforeGpu 及 offscreen windowThreadId；全新目录复跑后通过。

代码/纯测试/可重跑入口位于：
`AutoTest/artifacts/x64-visual-compare-contract-20261023/`。
主线程执行 `run_compare_r2.py --output <全新 AutoTest/artifacts 子目录> --execute-native`；
默认不加 execute-native 只绑定输入、不运行 native。生成图用 `render_compare.py --receipt <通过的回执>`。
继续保持唯一构建/GPU lease、BelowNormal、非输入 Desktop，不在玩家进程中运行。

## 内存现有实据，不能转成节省量

2026-10-22 r3 专门内存实验的当前回执 SHA：
`724468C3D4AE45651260D5F67F1C8BA4A166FD2F8ED0F80A5F34FCD779E6587C`，
路径 `AutoTest/artifacts/x64-memory-20261022-r3/receipt.json`。
该轮 96 次 COM 资源循环、256 次 Query、319,488 次像素检查；不是本轮截图的当前 host 内存样本。

| 进程指标（采样范围） | 32 位测试客户端 | 64 位渲染宿主 |
| --- | ---: | ---: |
| 私有提交量 | 1.375–2.234 MiB | 173.762–204.418 MiB |
| 工作集 | 7.574–10.918 MiB | 157.523–188.945 MiB |

两列是**同一个跨进程方案的两个组成部分**，不是旧/新方案。不能用 204−2 计算节省量。
该轮没有同输入 32 位本地渲染的配对内存基线，也没有游戏工作负载；x86-before、host、
x86-after 只是采样窗口，不是同时快照。OS 私有提交量、工作集、VA 和物理显存必须分开。
`memorySavingsProven=false / gameTouched=false / productIntegrated=false` 保持不变。

迁移优先目标是减轻 32 位进程的地址空间压力，不是保证系统 RAM 总量或显存必然下降。
要交付作者真正需要的数字和游戏双窗口图，仍须完整游戏接线、同图同路线/镜头配对 A/B，
分别记录 Warcraft private/working set/VA、x64 host 占用、GPU预算/驻留及真实帧像素。
此外产品异步取消、闲置 peer/window 故障通知、完整 D3D9/WarVK pass 覆盖仍按长期主线独立推进。

本轮未启动游戏、未替换玩家 DLL、未改全局设置、未提交/推送/发布。
