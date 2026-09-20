# v1.22 JASS / 局部体积雾 / Froxel 首批功能 checkpoint

结论：**限定功能通过，未发布**。所有编辑位于独立 v1.21.00 基础集成树；原性能树与玩家 DLL
不覆盖，未完成 Water 分支不合入。此处没有宣称全量 API、前台 FPS、长期稳定或 Guide 最终画质通过。

## 实际改动

1. 修正 `sun.enabled=false`：清主方向光 diffuse/specular RGB，不清 ambient/其他 light 字段；
   自定义材质直射项和正常 CSM 强度保持同一开关。JASS wire、Shader API ABI 不变。
2. 修正 1.27a `LoadInteger`/`LoadReal` 的精确原生签名末尾分号。此前 typed 安装整体失败，
   尽管字符串回退使不少 API 看起来仍可用。保留原子安装与严格匹配，不接受任意签名。
3. 加入显式开启的实际体积执行见证，分离 requested/effective backend、拒绝阶段、帧/epoch、
   fog population 与 composite。准入前的旧 active 文案改为 requested，不伪装 GPU 完成。
4. 新建隔离测试 runner、真实 JASS timer fixture、只改副本的 MPQ 脚本装配与取证汇总。
   原生 bridge 测试和真实 JASS 字节码证据明确区分。shader/预算/积分算法不修改。

## 构建与离线验证

- Fresh build32 release / Win32，以 pinned 子模块构建，父进程 BelowNormal、总并行 -j2。
  首次 456-edge 计划在 DLL link 暴露缺失 MinHook archive；产品 C++ 编译通过。
  从 pinned MinHook 四个 x86 C 源文件串行构建静态库后，4 个剩余 link 完成。
  不复制旧库、不忽略链接失败。后续 typed 修复为 exact 2 edge（bridge object + DLL link）。
- 最终 exact DLL no-work。PE32/i386，35,258,436 bytes：
  `D888587E899F9B7EC63C8C1C23F7DB2C5EDDDA4AE8BF9CEA7EAA41CCD5EDE2D4`。
  DLL 路径 `build32/src/d3d9/d3d9.dll`，同字节冻结在最终 runtime 目录的 candidate.dll。
- 10 份定向 Python/static **102/102**，4 个 Win32 runnable exit 0：sun-light-policy、
  fog-volume-manager、japi-v1-protocol、volumetric-shader-work-admission。
  相关 py_compile、git diff --check 通过。最后汇总命令曾误写一个测试模块名，保留失败日志；
  正确模块名重跑 102/102，不能把 ImportError 算通过。
- `warvk_api.j` 916 行及合成 map 脚本均通过 pjass；MPQ 内 war3map.j 逐字节回读相同。
  此为直接 stock JASS carrier 路线，不声称 YDWE GUI 保存、catalog 回读或 Icefall 发布验收。

## 四次事务（全部 fresh process、隔离、2560×1440、零 global input）

根目录 `AutoTest/artifacts/v122_visual_api_20260914/`，每次 JSON/BMP/DLL/debug-events 独立保存。

| 目录 | DLL | 判定 |
| --- | --- | --- |
| v122_visual_api_first_20260914 | AF6D…F2D1 | FAILED：High/Medium已有证据，Legacy 1440p 16步触发现有350 Mi工作预算 |
| v122_visual_api_legacy8_20260914 | AF6D…F2D1 | PASS（native-carrier）：三形、High/Medium及8步Legacy、错误/销毁用例 |
| v122_jass_vm_first_20260914 | AF6D…F2D1 | FAILED：VM stage0发现 typed-handshake=false；不追认为typed通过 |
| v122_jass_vm_typed_fixed_20260914 | D888…E2D4 | PASS：10阶段、83 VM断言、49公开函数被fixture直接调用 |

后两份 fixture 地图均是原测试地图的独立副本，包含精确当前 API 包装。最终副本：
5,882,765 bytes / `5691174EB7A667D9C8553ADD7612C1209626EFF30BA33B8D4B94C4433C511989`。
fixture 使用地图自己的 timer 和 stock PreloadGen 写出逐阶段结果；宿主不伪造结果文件。
49 个公开函数包括 1 个 expected UnsupportedFeature（Bloom=18），不是49个效果全产品化；
其余60个公开函数未被该fixture直接调用，完整名单在 validation-summary.json。

最终10阶段：太阳开→关→Sphere High→雾关闭→Box High→Cylinder High→Cylinder Medium→
低俯角High→雾内High→销毁/容量/错误清场。具体包括：

- typed handshake、point位置/颜色/半径写入、real/int精确标量结果、同结果字符串回退。
- Sphere/Box/Cylinder创建、位置/旋转/尺寸/密度/边缘、禁用、销毁与stale handle拒绝。
- 8槽成功、第9次创建失败且error19，逐项销毁；backend3拒绝，最终fogCount0。
- 帧递增、非零map/device epoch、requested/effective backend2或1、compositeSubmitted=true、
  实际backbuffer2560×1440。只证明对应样本的命令/像素链，不保证所有帧无异常或High从不回退。

## 看过的画面与结论

按需查看了首轮AF6D的sun-on/off、sphere-high/fog-disabled，以及最终D888的
vm-sun-on/off、vm-box-high、vm-cylinder-high、vm-low-angle-high、vm-inside-high。
太阳关闭后的地面/建筑/单位直射亮度及方向阴影明显变化，火焰/传送门/环境贡献仍可见；
局部雾在对应位置出现，关闭后移除。Box/Cylinder与进入雾内有可见介质；未见整帧损坏。
低俯角图被前景大树遮挡较多，只作该机位功能证据，不能据此宣称所有阴影柱/Guide边缘问题解决。
未采连续视频/动态遮挡序列，不下随机闪烁、时域稳定或动态Guide最终画质结论。
截图上的FPS不作性能基准；窗口隔离与开启诊断、不同取景都不符合发布前台比较合同。

## 恢复与证据

- 四次均 exact native process HANDLE 结算、隔离桌面关闭、video registry复原、零相关进程。
  System GPU事件和新增dump均0；收尾只读检查最近gpu_incident仍为8月11日旧文件。
- 测试安装 `E:/Work/War3/d3d9.dll` 恢复 A570：35,946,664 /
  `A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A`。
- 玩家 `E:/Work/Warcraft III/d3d9.dll` 始终 B1FCC：34,577,677 /
  `B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993`。
- 作者原map始终11376…E7CF、5,857,721 bytes。宿主实际启动复制到test install的短路径；
  最终核其内容为本轮fixture5691后保留副本，再恢复为前两次native测试的11376内容。
  不宣称这是未知更早时间的test目录历史，也不触碰作者源map。
- 最终runtime receipt：5,110 bytes /
  `2C975E2F0E9DD0E73886BE3103B50A6869E03AC21A526C23D8EA31B6BCECCAB1`。
- validation-summary.json：10,177 bytes /
  `E252F952C1DEACFF6D8DBF112CA9F78C7B5FA4379CA1F933EEBD3C33A420FA28`。
  汇总冻结时记录32条dirty，新增本checkpoint文档后最终33条（17 tracked +16 untracked）；
  不重写该证据。git diff --check exit0，pinned子模块无commit漂移，water/shader/dxvk目录差异为空。

## 下一步，不冒充本轮已完成

剩余API逐项状态与真实结果、11条unsupported的GUI可见性/产品范围、作者包catalog/编辑器保存链；
Guide动态边缘、复杂重叠/局部介质边界、太阳与点光组合、长期/重启生命周期及前台性能护栏。
typed拦截从未安装变为实际安装，还需单独量化原生hashtable旁路开销，不能假定无成本。
原版模型点光接入、异步截图/同步优化的release-base移植及最终组合仍是独立发布阻断项。
本DLL不是整晚性能树的完整替代品，不自动部署给玩家，不改版本号、提交或发布。
