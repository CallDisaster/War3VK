# 2026-10-10 LLVM clean build 三项旧回归失败的只读审查

本 lane 仅只读审查源码、现有 LLVM 对象文件和主线程保存的测试产物；
不修改生产/测试、不编译、不重跑测例。唯一写入是本报告。修复与运行由主线程完成。

## 结论

这三项红灯不能合并称作“LLVM 暴露生产故障”：

| 旧失败项 | 真实机制 | 主线程保存的复验 |
| --- | --- | --- |
| war3_live_palette_selection | M2-5B 测试拿没有统一初始值的结构 padding 做全结构 memcmp；七命名输出字段没有差分。 | 共用同一默认结构的representation初始化后，保留完整memcmp和7字段对拍，40211653/40211653。 |
| war3_frame_evidence_runtime | 继承旧 OUTPUT 指向已不存在目录；生产导出按原契约拒绝该路径。 | 仅清本测试进程的OUTPUT后通过。 |
| war3_palette_object_wire_roundtrip | 与上项相同的无效OUTPUT；15场景均无法导出，Python缺EXPORT是次生失败。 | 同样清测试进程OUTPUT后通过，二项合计2/2。 |

这证明本组三项的初始化/环境兼容问题闭合，不等于GPU、游戏阴影、全部工具链或天气视觉验收。
本报告未修生产算法，也未放宽解析、完整性、导出和逐字段判据。

## 初始日志证据

最初读取 `build32_weather_20261010/meson-logs/testlog.json`（10237510 B），
只提取三项元数据及各stdout/stderr首尾10行，没有输出8MiB错误全文。
Meson重跑会替换该文件，因此旧运行以主线程保存的artifact为准：

- `AutoTest/artifacts/weather-night-20261010/meson-01.log`。
- `AutoTest/artifacts/weather-night-20261010/live-palette-before-padding-fix.txt`。
- `AutoTest/artifacts/weather-night-20261010/live-palette-after-padding-fix.txt`。
- `AutoTest/artifacts/weather-night-20261010/meson-export-clean-env.log`。

初始三项：

- live palette：exit1，2.5216s；stdout199字符；stderr8929184字符/168477行。
  M2-4 7477257/0，A9 947458/0；M2-5B 1347808 checks/168476 failures；总40211653 checks。
  首尾错误均是 `M2-5B stats memcmp mismatch`。
- frame evidence runtime：exit1，0.0315s；stderr27字符，只报 `exported["ok"].get<bool>()`。
- wire roundtrip：exit1，0.2467s；C++ ROUNDTRIP174 checks/15 failures，Python74 checks/4 failures。
  所有场景缺导出；前面的编码器73/0、记录器冻结终态已经成立，不能把失败解释为编码全坏。

## M2-5B：逐字段反例和完整比较边界

真实测试位于 `src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp` 的M2-5B块。
每用例为1次全结构memcmp，加7次字段整数精确相等；初始化原为：

```cpp
st = dxvk::War3ShadowCaptureStats{};
```

只读逐行统计保存的旧失败文本，错误类别精确为：

- Grid整结构memcmp18432条。
- Seq整结构memcmp44条。
- Rand整结构memcmp150000条。
- 合计168476条，没有一条7命名字段差分。

这正好是1347808/8个用例，即每个用例只有整体representation比较失败。
网格还覆盖skinned=false，此时生产/旧参考聚合函数均不写任何字段。
这比“看起来像padding”强：7个指定输出对拍无差异，而全字节比较在无写路径仍有差异。

生产 `war3_palette_submitted_aggregation.cpp` 与
`tests/war3_palette_submitted_aggregation_legacy_reference.inc` 的BEGIN/END正文
经只读字符串比较完全相同（各2495字符）。两侧只写那7个统计字段；源码没有生产端附加写入。
这不替代实际逐字段测试，只作为失败定位的交叉证据。

### 布局与旧对象文件证据

此clean配置为clang22.1.8，i686-w64-windows-gnu，C++17/-O3/-mstack-alignment=4。
只读审查原对象中 `DiffM25bSystematic` 的反汇编，没有调用编译器生成新文件：

- 全结构memcmp立即数长度0x1008，即4104B。
- 当前Stats全部是uint32/uint64和4个uint32数组；按本目标uint64的8B成员对齐，
  826项声明布局大小同为4104B，存在18处4B内部padding，合计72B。
- 最早padding为offset660..663，在
  `semanticSceneLivePaletteRefreshLastMatrixCount` 与
  `semanticSceneLivePaletteRefreshLastMatrixHash` 之间；之后包括940..943、1204..1207等。
- 旧LLVM生成的初始化先memset首段0x294（660）B，下一段从offset664开始。
  它将成员初始化为0，却没有给660..663建立统一representation。

因此原测试把语言层面的成员等价与不受约束的padding字节等价混为一谈；
“trivially copyable”只保证能复制representation，不保证两个独立value-initialized对象的padding相同。
对象文件会随主线程重建被替换；以上是本次修复前只读审查记录，不应当作当前对象仍有相同初始化。

### 最小修正（主线程已执行）

```cpp
static const dxvk::War3ShadowCaptureStats empty{};
std::memcpy(&st, &empty, sizeof(st));
```

两侧从同一个representation起点出发，同时保留默认成员值、原先7字段精确对拍和全结构memcmp。
不能直接删memcmp，不能只比7字段，否则会丢掉“写到了其它统计成员/字节”的原验收意图。
保存的after运行已经达到M2-5B1347808/0，总40211653/40211653；修法不是把红灯改成宽松绿灯。

## 两项export：无效环境而非工具链IO差异

独立提取两项最初日志env，均实际继承：

```text
DXVK_WAR3_FRAME_EVIDENCE_OUTPUT=E:\Work\warvk-live-E\evidence3
```

只读 `Test-Path` 结果为False。`war3_frame_evidence.cpp::OutputDirectory()` 的显式env分支
只校验绝对路径/禁止`..`，不创建该目录；之后GetFileAttributesW要求已存在普通目录，拒绝reparse。
不存在时返回空，Control(export)返回具名错误，冻结证据保留。这是原设计fail-closed，不是LLVM路径编码回归。

主线程只从相关测试进程环境移除OUTPUT，让其走程序目录的默认输出路径；没有修改机器全局环境、
玩家目录或生产路径准入。保存的 `meson-export-clean-env.log` 显示runtime0.05s、roundtrip0.67s，2/2通过。

以后该组测试应明确控制OUTPUT：或者从测试进程环境排除，或者给独立已创建目录；
不应隐式继承玩家旧录制目的地。无效输出目录的反例可以独立保留，继续断言导出失败且证据保留。

## 保留的边界

本次只确认三项具体旧失败的原因及主线程的最小修复复验；
没有给其它memcmp测试作全库替换，没有修改production统计或放宽缺链认证，
没有声称真实GPU/游戏负载下的内存、阴影或天气效果由此已通过。
