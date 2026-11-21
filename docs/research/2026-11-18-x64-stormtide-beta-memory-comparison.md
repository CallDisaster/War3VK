# WarVK x64 Beta (2026-11-18b) + StormTide v0.40 高清资源包内存与压力转移实验报告

- 日期：2026-11-18
- 实验数据目录：[x64-va-stormtide-beta-20261118](file:///e:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/artifacts/x64-va-stormtide-beta-20261118)
- 实验环境：Warcraft III 1.27a 隔离桌面环境（`IsolatedRenderDesktop`，受限 Medium token，RTX 4060 Ti；基准对比基于干净沙盒 `E:\Work\War3_AutoTestSandbox` 补齐原版 `redist\miles`，并另跑实装目录 `E:\Work\Warcraft III` 复核）
- 前置对照基线：[2026-11-18-x64-opengl-vs-proxy-memory-comparison.md](file:///e:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/docs/research/2026-11-18-x64-opengl-vs-proxy-memory-comparison.md)

---

## 1. 部署状态与共存机制验证

### 1.1 二进制与资源包核验

| 组件 | 路径（相对 [E:\Work\Warcraft III](file:///E:/Work/Warcraft%20III)） | 大小 (Bytes) | SHA-256 | 说明 |
| --- | --- | ---: | --- | --- |
| WarVK Beta 前端主 DLL | `d3d9.dll` | 21,594,112 | `D504047A1F024138C33120DBC47D675325B8BD9676CF02819B8F6452DF34C303` | 解压并核对 `WarVK\x64\SHA256SUMS.txt` 15/15 一致 |
| WarVK 代理桥接 | `WarVK\x64\warvk-d3d9-front.dll` | 897,024 | `13EE141C2C2148357CE89FE8850714079FFDCDF9EAEBE13334AC7952E6C33967` | 自动拉起 64 位渲染宿主 |
| WarVK 64 位渲染宿主 | `WarVK\x64\warvk-d3d9-host.exe` | 7,073,792 | `6FCB914B7410705373001B78D5136C8F483C322EA5C47F36171E84E5AF7C2924` | 独立 64 位 DXVK/Vulkan 渲染进程 |
| 原版备份（保留） | `WarVK\Backups\d3d9.dll.C7F16DBC_backup_20261118` | 22,855,680 | `C7F16DBCE34944CD104DDD71AC04CF0D58DD4875A8B199AB456CE332415FCF8A` | 原 `f8 d3d9.dll` 备份未改动 |
| StormTide 加载器 | `StormTide.asi` | 49,152 | — | 复制自 `StormTide\data\StormTide.asi`，由 `Mss32.dll` 自动载入 |
| StormTide 活跃高清包 | `StormTide\installed\*.mix` (9个) | 790,858,763 (754.22 MiB) | — | NTFS 硬链接（`nlink=2`，零额外磁盘拷贝）至 `StormTide\data\` |

### 1.2 StormTide 与 WarVK 共存加载链（实机验证发现）

1. **安装器 GUI 限制与绕过说明**：`StormTide_v0.40_Setup.exe` 解包后（安装包记录成功将注册表 `HKCU\Software\Blizzard Entertainment\Warcraft III\Allow Local Files` 设为 `1`），其配置器 `StormTide.exe` 启动时因 WebView2 初始化失败退出（`StormTide.log` 记录 `WEBVIEW_ERROR_INVALID_STATE (-3)`），导致 `StormTide\installed\` 初始为空。此外 `StormTide\data\d3d9.dll`（888,832 B）是硬编码链式调用系统 `System32\d3d9.dll` 的 ASI 加载器，不可覆盖 WarVK 的 `d3d9.dll`。
2. **无冲突自动挂载路径**：反汇编验证 `StormTide.asi` 仅导入 `KERNEL32` 与 MSVC 运行库，不触碰图形 API；`DllMain` 检查 `Game.dll`/`Storm.dll` 已加载后，遍历 `<StormTide.asi所在目录>\StormTide\installed\*.mix|*.asi` 调用 `LoadLibraryA`。每个 `.mix` 头部是 84,992 字节的 PE `DllMain` 加载桩，向 `Storm.dll` 导出序号 266（`SFileOpenArchive`）以优先级 `10` 挂载自身尾部附加的高清 `MPQ` 档案。因此将 `StormTide.asi` 置于游戏根目录、在 `StormTide\installed\` 激活 9 个核心高清包，即可在 **WarVK x64 proxy / inproc / `-opengl` 三种渲染模式下完全等价地生效**。
3. **本轮激活的 9 个核心高清 `.mix` 包（总计 754.22 MiB）**：`Storm.mix`（145.28 MiB / 723 文件）、`StormUnits.mix`（64.53 MiB / 709 文件）、`StormDoodads.mix`（218.16 MiB / 1,520 文件）、`StormAbilities.mix`（13.51 MiB / 316 文件）、`StormIcon.mix`（19.76 MiB / 516 文件）、`StormZhCN.mix`（284.82 MiB / 4,522 文件）、`StormCombat.mix`（2.82 MiB）、`StormCursor.mix`（0.09 MiB）、`StormBackground01.mix`（5.26 MiB / 11 文件）。已明确跳过外部第三方阴影补丁 `StormTideShadow.asi`（354,816 B）与冲突的多选英雄替身皮肤。
4. **`redist\miles` 与沙盒修正**：首次冒烟（`_smokeA`）证实 `E:\Work\War3_AutoTestSandbox` 原本缺少原版 `redist\miles\`（仅 373 KiB），导致 `Game.dll` 在 `0x6f1423ca` 检查目录失败并直接跳过 `_AIL_startup@0`，使 `Mp3dec.asi` 与 `StormTide.asi` 均未加载。因此在后续所有沙盒对照测试中（无论 `hd_off` 还是 `hd_on`）均补回原版 `redist\miles\` 以保证唯一变量干净。
5. **高清资源生效实证**：在补回 `redist\miles` 后的 45 秒 LostTemple 冒烟（`_smokeA2`）与所有 `hd_on` 运行中，`EnumProcessModulesEx` 均直接观测到 `stormtide.asi` 与全部 9 个 `storm*.mix` 驻留进程；`War3_d3d9.log` 中送往渲染器的每帧动态顶点数据量从 `0.337 MB/帧`（`_smokeA`）升至 `3.195 MB/帧`（`_smokeA2`，约 9.5 倍），`RenderQueueTracker` 峰值从 97 升至 127；64 位宿主提交内存同步大涨 `311~530 MiB`。

---

## 2. 三张地图全对比实测数据

### 2.1 实验设计说明

- **组别定义**：
  - `proxy`：`WarVK x64 Beta 2026-11-18b`（`d3d9.dll` + `WarVK\x64\*`，自动拉起 64 位 `warvk-d3d9-host.exe`）。
  - `inproc`：同一份 Beta `d3d9.dll`，移除 `WarVK\x64` 目录使渲染回落至 32 位进程内 DXVK/Vulkan（作同版本单进程对照）。
  - `gl`：原版 `-opengl`（完全移除 `d3d9.dll`，走 `opengl32.dll` + NVIDIA `nvoglv32.dll`）。
- **版本变量**：`hd_off` = 原版 1.27a + `redist\miles`；`hd_on` = 同上加装 `StormTide.asi` + 9 个核心高清 `.mix`。`stock` 清除 PE `LARGE_ADDRESS_AWARE` 标志（2 GiB 上限），`laa` 保留（4 GiB 上限）。
- **观测口径**：外部 `VirtualQueryEx` 按低 2 GiB 分区统计 `lowUsable`（可用空闲）、`minLowUsable`（全过程最低）、`lowLargest`（最大连续空闲块）、`P`（`low.private.committed`）、`R`（`low.private.reserved`）、`I`（`low.image.committed`）、`M`（`low.mapped.committed`）；`ReadProcessMemory` 读取 `Storm.dll+0x5738C`（`strm`）及 `Storm.dll+0x56F80` 256 桶原生堆表汇总（`hReq`/`hCom`/`hRsv`）；同时跟踪并合计存活 64 位 `warvk-d3d9-host.exe` 的 `PrivateUsage`（`hP`）与 `WorkingSetSize`（`hWS`）。

### 2.2 全量测试汇总表（稳态窗口中位数）

| 地图 (时长) | 组别 | 状态 (退出/超时/句柄) | `lowUsable` (MiB) | `minLowUsable` (MiB) | `lowLargest` (MiB) | `highUsable` (MiB) | 32位 `priv` (MiB) | 32位 `P / R / I / M` (MiB) | `Storm` 计数 (MiB) | `Storm` 堆 `Req/Com/Rsv` (MiB) | 64位 `hP / hWS` (MiB) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | --- | --- |
| Lost Temple (70s) | `proxy` `hd_off` `stock` | `0 / F / 3644` | 1527.7 | 1521.6 | 906.6 | 0.0 | 151.3 | `145.9 / 182.5 / 128.7 / 32.5` | 37.5 | `37.6 / 45.4 / 111.8` | `840.5 / 293.6` |
| Lost Temple (70s) | **`proxy` `hd_on` `stock`** | `0 / F / 3642` | **1507.2** | **1501.4** | **906.6** | 0.0 | **170.2** | `164.5 / 181.8 / 130.4 / 32.5` | **54.0** | `54.2 / 61.3 / 131.1` | **`1161.9 / 501.6`** |
| Lost Temple (70s) | `proxy` `hd_on` `laa` | `0 / F / 3644` | 1514.6 | 1508.6 | 906.6 | 2040.4 | 170.7 | `165.1 / 180.8 / 130.4 / 26.9` | 54.1 | `54.2 / 61.3 / 131.1` | `1114.2 / 440.8` |
| Lost Temple (70s) | `inproc` `hd_off` `stock` | `0 / F / 3644` | 1010.6 | 1003.2 | 564.4 | 0.0 | 326.8 | `316.7 / 331.8 / 260.0 / 94.6` | 37.5 | `37.6 / 45.4 / 111.8` | — |
| Lost Temple (70s) | `inproc` `hd_on` `stock` | `0 / F / 3644` | 951.4 | 934.2 | 491.8 | 0.0 | 366.0 | `354.7 / 329.4 / 261.6 / 116.2` | 54.1 | `54.2 / 61.3 / 131.1` | — |
| Lost Temple (70s) | `gl` `hd_off` `stock` | `0 / F / 3644` | 1300.8 | 1292.5 | 735.2 | 0.0 | 156.8 | `147.3 / 306.0 / 227.4 / 30.0` | 217.5 | `75.4 / 45.8 / 113.2` | — |
| Lost Temple (70s) | `gl` `hd_on` `stock` | `0 / F / 3644` | 1254.4 | 1247.2 | 735.2 | 0.0 | 204.1 | `194.7 / 301.4 / 229.1 / 30.1` | **648.1** | `99.3 / 63.3 / 132.8` | — |
| EchoIsles (70s) | `proxy` `hd_off` `stock` | `0 / F / 1985` | 1537.7 | 1531.3 | 906.6 | 0.0 | 144.2 | `138.8 / 179.7 / 128.7 / 32.5` | 33.2 | `32.9 / 40.0 / 103.1` | `837.1 / 292.8` |
| EchoIsles (70s) | **`proxy` `hd_on` `stock`** | `0 / F / 1985` | **1526.4** | **1518.8** | **906.6** | 0.0 | **153.1** | `147.5 / 179.7 / 130.4 / 32.5` | **38.7** | `38.3 / 45.4 / 110.8` | **`1148.6 / 485.1`** |
| EchoIsles (70s) | `proxy` `hd_on` `laa` (r4) | `0 / F / 1985` | 1532.5 | 1526.2 | 906.6 | 2040.4 | 153.3 | `147.7 / 180.3 / 130.4 / 26.9` | 38.6 | `38.3 / 45.5 / 110.8` | `1104.3 / 434.2` |
| EchoIsles (70s) | `inproc` `hd_off` `stock` | `0 / F / 1985` | 1018.4 | 1010.3 | 563.2 | 0.0 | 323.9 | `312.7 / 327.0 / 260.0 / 94.7` | 33.2 | `32.9 / 40.0 / 103.1` | — |
| EchoIsles (70s) | `inproc` `hd_on` `stock` (r4) | `0 / F / 1985` | 961.3 | 950.5 | 502.3 | 0.0 | 354.0 | `344.1 / 322.1 / 261.6 / 125.8` | 38.7 | `38.3 / 45.5 / 110.8` | — |
| EchoIsles (70s) | `gl` `hd_off` `stock` | `0 / F / 1985` | 1315.1 | 1305.3 | 735.2 | 0.0 | 144.9 | `135.4 / 303.8 / 227.4 / 30.0` | 199.6 | `66.7 / 40.4 / 105.6` | — |
| EchoIsles (70s) | `gl` `hd_on` `stock` (r2) | `0 / F / 1985` | 1295.0 | 1286.8 | 735.2 | 0.0 | 164.3 | `154.6 / 301.1 / 229.1 / 30.1` | **608.8** | `79.5 / 45.7 / 112.5` | — |
| 生与死 (170s) | `proxy` `hd_off` `stock` | `0 / F / 19286` | 1019.3 | 1006.8 | 614.5 | 0.0 | 572.4 | `569.8 / 264.3 / 130.1 / 32.5` | 218.6 | `254.4 / 285.5 / 435.6` | `1564.4 / 635.8` |
| 生与死 (170s) | **`proxy` `hd_on` `stock`** | **`0 / F / 19286`** | **706.9** | **701.8** | **316.0** | 0.0 | **748.7** | **`741.6 / 403.2 / 131.0 / 32.5`** | **363.8** | `398.2 / 445.6 / 727.7` | **`2094.7 / 918.7`** |
| 生与死 (170s) | `proxy` `hd_on` `laa` | `0 / F / 19286` | 714.8 | 704.6 | 310.5 | 2040.4 | 749.1 | `742.0 / 401.9 / 131.0 / 26.9` | 363.8 | `398.2 / 446.1 / 729.8` | `2034.3 / 849.8` |
| 生与死 (170s) | **`proxy` `hd_on` 实装目录 (`laa`)** | **`0 / F / 19286`** | **651.6** | **651.4** | **254.2** | **2040.4** | **768.7** | `744.8 / 416.9 / 172.2 / 27.1` | **363.2** | `398.2 / 445.8 / 728.2` | **`2034.9 / 849.6`** |
| 生与死 (170s) | `inproc` `hd_off` `stock` | `0 / F / 19286` | 442.5 | 429.6 | 114.5 | 0.0 | 842.1 | `833.0 / 369.2 / 261.0 / 108.3` | 218.6 | `254.4 / 285.7 / 436.1` | — |
| 生与死 (170s) | **`inproc` `hd_on` `stock`** | **`2 / T / 7774` (崩溃!)** | **461.5** | **459.0** | **113.1** | 0.0 | **752.2** | `739.1 / 390.8 / 264.1 / 153.3` | 260.9 | `294.2 / 321.3 / 455.8` | — |
| 生与死 (170s) | `gl` `hd_off` `stock` | `0 / F / 19286` | 782.9 | 772.5 | 305.0 | 0.0 | 621.3 | `610.6 / 350.1 / 228.7 / 30.0` | 1390.2 | `427.6 / 291.7 / 443.1` | — |
| 生与死 (170s) | **`gl` `hd_on` `stock`** | `0 / F / 19286` | **448.4** | **439.5** | **114.0** | 0.0 | **822.0** | `810.8 / 480.2 / 229.7 / 30.1` | **2230.0** | `590.6 / 454.1 / 738.8` | — |

> 注：`proxy` 实装目录组（`source=w3`）同时加载了用户目录中原本存在的 `War3Tools1.11.mix`（+33 MiB 镜像及 UI 占用）与 `StormBreaker.asi`，45s LostTemple 冒烟（`_smokeB`）及 170s 生与死（`st-proxy-hd_on-w3-sy-laa-r1`）均零错误、零回退完成（`fallback=null`, `errorFiles=[]`）。

---

## 3. 核心结论：什么真的移去了 64 位，什么还留在 32 位？

### 3.1 开启高清前后的精确增量对比（`hd_off -> hd_on`，均 `stock` 2 GiB）

| 地图 | 指标 | `WarVK x64 proxy` | `WarVK inproc` (32位) | `-opengl` |
| --- | --- | ---: | ---: | ---: |
| **Lost Temple** | `lowUsable` 变化 | **`-20.5 MiB`** (`1527.7 -> 1507.2`) | `-59.2 MiB` (`1010.6 -> 951.4`) | `-46.4 MiB` (`1300.8 -> 1254.4`) |
| Lost Temple | `lowLargest` 变化 | **`0.0 MiB`** (`906.6 -> 906.6`) | `-72.6 MiB` (`564.4 -> 491.8`) | `0.0 MiB` (`735.2 -> 735.2`) |
| Lost Temple | `low.mapped` 变化 | **`0.0 MiB`** (`32.5 -> 32.5`) | `+21.6 MiB` (`94.6 -> 116.2`) | `+0.1 MiB` |
| Lost Temple | `Storm+0x5738C` 变化 | `+16.5 MiB` (`-> 54.0`) | `+16.6 MiB` (`-> 54.1`) | **`+430.6 MiB`** (`-> 648.1`) |
| Lost Temple | 64位 `hostPriv / WS` 增量 | **`+321.4 / +208.0 MiB`** | — | — |
| **EchoIsles** | `lowUsable` 变化 | **`-11.3 MiB`** (`1537.7 -> 1526.4`) | `-57.1 MiB` (`1018.4 -> 961.3`) | `-20.1 MiB` (`1315.1 -> 1295.0`) |
| EchoIsles | `low.mapped` 变化 | **`0.0 MiB`** (`32.5 -> 32.5`) | `+31.1 MiB` (`94.7 -> 125.8`) | `+0.1 MiB` |
| EchoIsles | `Storm+0x5738C` 变化 | `+5.5 MiB` (`-> 38.7`) | `+5.5 MiB` (`-> 38.7`) | **`+409.2 MiB`** (`-> 608.8`) |
| EchoIsles | 64位 `hostPriv / WS` 增量 | **`+311.5 / +192.3 MiB`** | — | — |
| **生与死 (170s)** | 结果 | **全程稳过 (`exit=0`)** | **第 78.8s 崩溃 (`Error.dmp`)!** | 压至 `448.4 MiB / 114.0 MiB` 险过 |
| 生与死 | `lowUsable` 变化 | **`-312.4 MiB`** (`-> 706.9`) | `461.5` 时即崩溃（`lrg=113.1`） | **`-334.5 MiB`** (`-> 448.4`) |
| 生与死 | `low.mapped` 变化 | **`0.0 MiB`** (`32.5 -> 32.5`) | **`+45.0 MiB`** (`108.3 -> 153.3`) | `+0.1 MiB` |
| 生与死 | 32位 `private` 增量 | `+176.3 MiB` (`572.4 -> 748.7`) | （第 78.8s 崩前已达 752.3） | `+200.7 MiB` (`621.3 -> 822.0`) |
| 生与死 | `Storm+0x5738C` 变化 | `+145.2 MiB` (`-> 363.8`) | — | **`+839.8 MiB`** (`1390.2 -> 2230.0`, **超 2 GiB!**) |
| 生与死 | 64位 `hostPriv / WS` 增量 | **`+530.3 / +282.9 MiB`** (`-> 2094.7`) | — | — |

### 3.2 对假设的诚实回答：为什么《生与死》下 32 位内存还是涨了 `+176 MiB`？

1. **渲染侧与显卡驱动侧确实几乎 100% 移出了 32 位**：
   - 在全部三张地图、无论是否开启高清资源包，`WarVK x64 proxy` 的 32 位 `low.mapped.committed` 始终死死钉在 **`32.5 MiB`**（增量为 **`+0.0 MiB`**），`low.image.committed` 仅微增 `0.9~1.7 MiB`（`StormTide.asi` 与 9 个 `.mix` PE 头部）。
   - 所有 D3D9/DXVK/Vulkan 贴图、顶点/索引硬件缓冲、管线状态、渲染器 arena 及 NVIDIA 驱动提交全部落入 64 位 `warvk-d3d9-host.exe`——在《生与死 + StormTide》下，单是 64 位渲染宿主一侧的提交内存就已经达到 **`2094.7 MiB`（2.05 GiB，超过一整个标准 32 位进程的全部可寻址上限！）**，其中高清包本身贡献了 **`+530.3 MiB`**。
   - 正是靠这份剥离：原本仅带原版贴图就只剩 `442.5 / 114.5 MiB` 的 **32 位 `inproc` 在装上 StormTide 后第 78.8 秒刷怪切波时瞬间崩毁**；而 **`WarVK x64 proxy` 连不开 LAA 都能稳稳打完 170 秒并剩余 `706.9 MiB` 可用 / `316.0 MiB` 最大连续空洞**。
2. **但游戏引擎自身的 CPU 模型缓存仍然留在了 32 位 `Game.dll`（Storm 堆）**：
   - 对《生与死》`proxy hd_off -> hd_on` 的 700+ 个 Storm 命名堆逐项做差，发现 **32 位侧 `-312.4 MiB` 的可用空间下降中，`Storm` 命名堆本身就解释了 `+292.1 MiB` 保留区（含 `+160.1 MiB` 实提交，占 `private` `+176.3 MiB` 增量的 90.8%）**，核心集中在三项引擎 CPU 几何表：
     - **`.?AVC3Vector@NTempest@@`**：committed `43.54 -> 143.94 MiB`（**`+100.39 MiB`**，requested `+89.64 MiB`）—— `Game.dll` 解析高清 `.mdx` 后在 CPU 上保留的 3D 顶点与法线坐标数组；
     - **`.?AVC2Vector@NTempest@@`**：committed `13.75 -> 35.94 MiB`（**`+22.19 MiB`**）—— UV 纹理坐标数组；
     - **`.G`**：committed `7.94 -> 27.94 MiB`（**`+20.00 MiB`**）—— `uint16` 网格三角面索引数组；
     - 其余为 `cmemblock.cpp`（`+5.56`）、`CAnimEmitter2ObjStatus`（`+3.50`）、`SFile.cpp` MPQ 哈希/块表（`+2.66`）、`Interp.cpp`（`+2.11`）等。
   - **架构含义**：魔兽争霸 3 1.27a 是 CPU 蒙皮（CPU Skinning）+ CPU 动画插值 + CPU 碰撞拾取引擎。虽然渲染后端已经将 GPU 资源、驱动、指令缓冲与着色器彻底搬迁到 64 位进程，**但只要模型多边形顶点数/模型种类大幅增加，`Game.dll` 仍需要在自己的 32 位 Storm 堆里存一份 CPU 几何源数据**。因此“塞大模型后 32 位游戏宿主内存并非严格固定不变”，而是会剔除掉图形/显存倍增压力，只随 `.mdx` 几何源数据量呈较温和的线性上升（对战地图仅涨 `+9~19 MiB`，重资产自制大图《生与死》约涨 `+176 MiB` 私有 / `+292 MiB` Storm 保留）。
3. **对比 `-opengl + StormTide`**：
   - 面对同样的高清模型，《生与死》上 `-opengl` 低 2 GiB 可用空间跌至 **`448.4 MiB`**、最大连续空洞 **`114.0 MiB`**（和 `inproc` 崩溃线 `461.5 / 113.1 MiB` 几乎重合），私有提交反而高达 **`822.0 MiB`**（比 `proxy` 多吃 `+73.3 MiB`）；
   - 同时 `Storm.dll+0x5738C` 累积分配计数器在 170 秒内飙升至 **`2230.0 MiB`**（已跨过 `2048 MiB / 0x7FFF_FFFF` 边界），而 **`WarVK x64 proxy` 仅为 `363.8 MiB`（不到 OpenGL 的六分之一）**。若开启 `LAA`（用户实装目录当前配置），`proxy` 在《生与死 + StormTide》下仍同时享有约 `652~715 MiB` 低 2 GiB 空间及额外 `2040.4 MiB` 高 2 GiB 空间，余量远非 32 位 `-opengl` 可比。

---

## 4. 覆盖范围与边界声明

1. 以上沙盒基准为各配置 `r1`（或 `EchoIsles` 避让并发冲突后的 `r4` / `r2`）单轮 70s/170s 实机采样式收口结果；实机测试硬件为单张 RTX 4060 Ti，处于非输入隔离桌面。
2. `EchoIsles` 首次测试期间外部并行任务 `va-auto-au-laa-r1d1`（未申请协同 `game` 锁）短暂占用了 Windows 全局事件 `Warcraft III Game Application`，导致若干启动期碰撞并返回 `"Warcraft III was unable to initialize."`；经 `WOW64` 调用栈采样证实为 `Game.dll` 重定位后 `0x6f027fca`（单实例事件冲突），加入启动前单实例事件释放等待后 `r4`/`r2` 均一次通过。
3. 本报告激活的是 StormTide 9 个核心高清 `.mix` 包（754.22 MiB），未混合第三方阴影钩子 `StormTideShadow.asi` 与多选皮肤；测试前后端及临时实例已全部清理并释放锁，[E:\Work\Warcraft III](file:///E:/Work/Warcraft%20III) 保持 `WarVK-x64-Beta-20261118b` 与 `StormTide` 同时生效可玩状态。
