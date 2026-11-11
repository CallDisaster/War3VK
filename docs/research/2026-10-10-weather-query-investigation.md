# 2026-10-10 即时天气时间查询异常：独立只读定位

最新进展（主线程补记）：同一生成脚本的真实游戏 A/B 已闭合。下文原审查的“待实机”
保留为当时结论，不代表当前仍未验证。`canary-query-before` 的旧 DLL 返回
`9.72999987e-05`，微小暂停时间被 stock S2R 读大；同 fixture 的
`canary-query-after` 返回 `0.0000973`，35 项 JASS 检查 / 0 错误。
两份合并 JASS SHA 同为 `DC4CD44CAF4CBA576B84018A693D2CE1AA90A97E01F1C03AC038E03CBCF5510C`。
旧失败 3 项与新通过分别冻结，未追认旧运行；最终候选又独立通过同一35项门。
完整交付与未覆盖范围见 [晨间报告](2026-10-10-weather-morning-report.md)。

范围：canary-b/c stage 10/11 原始回执、JASS wrapper、生产 carrier/dispatcher、
同件 Game.dll 的 S2R/R2S/string-manager。只读审查；本 lane 未编译、开游戏或改源码/阈值。
主线程同期正在实施修正，本报告保留失败候选事实，不将新源码追认为旧 DLL 行为。

## 结论

已定位一个确定存在的协议兼容缺陷：失败候选用 `std::to_chars(general,max_digits10)`
把 float 格式成字符串，原版 JASS `S2R` 却不支持科学计数法。
例如 `9.73e-05` 会被同件 Game.dll 读成 **9.73**，而不是 **0.0000973**。

该机制高度吻合 canary-c 的“立即 Play(0) 后回执 9.730，但 owner/render 从零推进”。
它不是渲染异步、旧帧缓存或 GPU 未完成能解释的差异，而是 float→text→JASS real 的接口转换问题。
旧回执未保存 S2R 前的 raw query string，故不能逐字证明那一次实际返回串就是 `9.73e-05`；
准确的当次微秒值仍待最小实机实验闭合。确定缺陷和当次精确值的证据等级分开。

## 1. 原始事实与身份

两轮使用同一 DLL：34,435,072 B，SHA256
`B001361CC839C26021DE251981FE271E3951DE59700BEF3845FEEF33EDCD400C`。
两份 MPQ 回读 `readback-war3map.j` SHA256 均为
`C3076B9FD49F6AF09B73F9199FB6AC249AC0327105D7134509A38ADF7646A2B1`。
因此不能先用 wrapper 版本不同解释 b/c 差异。

| 原始阶段 | canary-b JASS 回执 | canary-c JASS 回执 | 当前代码含义 |
| --- | ---: | ---: | --- |
| 10 | 18.001 | 17.999 | 关闭天气，清旧 keyframe，配置新时间线，尚未 Play |
| 11 | 0.000 | **9.730** | Play(0)，错误检查，随后同一 Tick 内 Receipt→GetWeatherTime |
| 12 | 8.014 | 8.015 | 一个 8 秒 timer 间隔后查询，无额外 Seek |

来源是两份 `run.json` 的 `stageReceipts[].raw`，不是截图 OCR 或分析器猜测。
c stage11 原文本明确写 `checks=26;errors=0;id=1;time=9.730`，raw SHA256 为
`C5687D1232E7A8069FB9EA6D7D129B27845488672BD6F57E178B1BAC0CB1DFF5`。

同轮 c 的 owner/render 取样：stage10 最后一次 elapsed=112.594，owner=17.9990216、enabled=false、
revision=19/history=4；stage11 首次 elapsed=113.373，owner=0.7172398、render=0.7160122、
revision=20/history=5。随后 owner/render 连续推进到 7.7178683/7.7167051。
b 的 stage11 首次 owner/render 是 0.4403833/0.4390971，行为一致。

这些外部取样不是即时 JASS 调用同一时刻的观测，但足以说明 CPU/渲染状态并未在 stage11 保持 9.73 秒。
从即时 9.73 变到下一阶段 8.015，也不能在没有额外重定位证据时解释为正常演出时钟递增。

## 2. 同步调用链

1. `WarVKPlayWeatherTimeline`（`warvk_api.j:453`）调用 Preloader，生产 `DispatchWeather`
   直接调用 `runtime.playTimeline`；不经过 settings mailbox 或 GPU queue。
2. `playTimeline`（`war3_weather_runtime.cpp:348`）在锁内开启、取消 pause、Seek 到传入 0。
3. `WarVKGetWeatherTime`（`warvk_api.j:470`）是 `S2R(GetLocalizedString(payload))`。
4. `Bridge_GetLocalizedString`（`war3_jass_command_bridge.cpp:1396`）同步进入 TryHandleCarrier。
5. carrier 调用生产 Dispatch；天气 GetTime 在 CPU 状态锁内 advance/getTime，返回 ResultKind::Real。
6. Dispatch 的 LocalizedString 分支调用 FormatReal，返回拥有其 `std::string` 的 Reply。
7. carrier 在 Reply 仍存活时调用 `to_jString(reply.text)`，返回 JASS string handle。
8. 原版 S2R 将返回字符串转换为 real；Receipt 的 R2S 再把该 real 显示成三位小数。

这条链没有异步 weather query、GPU readback、render diagnostics 回读或“最近一次 Served”替代值。
`WeatherCanaryError` 的 error query/clear 操作也不会修改天气时间。
在检查过的源码链中没有发现会让这次数值错误必须依赖 reentry 的路径。

## 3. 原版二进制验证，不依赖网络传闻

IDA 当前输入 `E:/Work/War3/Game.dll` 的 MD5 为 `267861A0DFD416DBAD13E7EE3EC7794A`。
本 lane 同时核对该文件与 `E:/Work/Warcraft III/Game.dll`，二者 SHA256 都是
`E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`，
与 b/c `protectedInputs` 的 Game.dll 身份相符。以下地址采用 IDA base=0x6F000000；
RVA 适用于这份文件，不要求实机 ASLR 地址相同。

| 路径 | 实际二进制锚点 | 直接观察 |
| --- | --- | --- |
| S2R native 注册 | Game+0x1E9BA9..0x1E9BB8 | `(S)R` 与 `S2R` 绑定实现 Game+0x1F33F0 |
| S2R 实现 | Game+0x1F33F0 | 原 native string→UTF8，然后调用 parser Game+0x53BF0 |
| parser 数字路径 | Game+0x53C5A..0x53CA8 | isdigit、累积十进制数字，最多九个有效数字参与 mantissa |
| parser 非数字路径 | Game+0x53CAD..0x53CB6 | 只有第一个 `.` 可继续；其它字符直接结束。e/E 不特殊处理 |
| R2S native | Game+0x1F2720 | 调 formatter Game+0x52FE0，固定 precision=3；这是输出显示，不是该错误的制造端 |

所以即使 C++ 生产解析器支持指数，原版 S2R 仍只得到 `9.73` 的 mantissa。
旧 general formatter 对小于 1e-4 的常见数值进入科学计数法，这正是立即 Play 后微秒级查询的范围。
普通 8/18/32 秒回执正常，不能证明刚启动时的字符串数值协议正确。

### 返回字符串寿命的反证审查

`to_jString` 对 1.27a 实际调用 Game+0x1DA520，ECX 传入 C string，随后进入 VM string manager。
Game+0x7F1540 对相同内容查找已存在 string handle；新内容路径经 Game+0x7DEAF0
保存复制字符串，再用 RCString AssignOrLink（Game+0x51310）持有 VM 的 string representation。
该路径不是把 Reply 的临时 `c_str()` 裸指针直接当作永久 JASS string 返回。
原版 R2S 自己也把局部栈 `char[256]` 经同一个 Game+0x1DA520 转成 handle。

因此，在本次检查范围内，Reply 离开作用域不能被直接认定为悬空返回字符串根因。
不把这一结论外推为整个 JASS VM/任意 reentry 的全局并发安全证明。

## 4. 最小修正及兼容边界

主线程拟将共同 FormatReal 改为 `to_chars(fixed)` 无显式 precision，以保持最短可 round-trip
普通十进制，避免用 fixed 六位强行把微小非零数截成零。当前 64B buffer 足以装有限 binary32
的 fixed shortest 表示；输入解析器仍可接受既有指数格式，不需要改请求 wire grammar。

共同 formatter 还用于 math/curve、visual time 和 stats 的 legacy real string 返回，
故这是共享读方协议修正，不只是 weather 一个函数。typed 实数通路不调用该 formatter。
错误/版本等非 real 文本也不受这项变化影响。

必须区分两项保证：

- fixed shortest 能确定消除 e/E 被原版 S2R 截断的缺陷。
- C++ from_chars 能逐 bit 复原，不等于 stock S2R 对全部 float 也逐 bit 复原。
  同件 stock parser 只保留九个有效位，并用 float 的 10^n 恢复位数。
  极小 subnormal 可能因中间 10^45 溢出而回零，极大值可能存在额外舍入。
  天气正常时间及本次约 1e-4 秒问题不需要借机扩大成全 VM float 算法重写；
  极值须另做真实 S2R 数值容差或采用 typed 通路，不能把 host round-trip 测例写成全域 JASS 认证。

## 5. 最小因果验证实验

由主线程独占实机 lease 执行，本 lane 不开游戏：

1. 不依赖 weather，原版 JASS 同时记录 `S2R("9.73e-05")` 与 `S2R("0.0000973")`，
   另带正指数/负值对照。预期前者约 9.73，后者约 0.0000973；用数值断言，不只用 R2S 三位显示。
2. Play(0) 后保存 **同一个** GetLocalizedString 原始返回到局部 string，再对该 string S2R。
   将 raw text 与 real*1000000 的显示共同落盘，避免丢掉微秒值。
3. 同 Tick 内暂停，再重复取 raw query/real，冻结时间防止实验自身多次推进造成混淆。
   保留原“Play 后接近零”“下一 timer 周期正常递增”门，不能删除或放宽。
4. 在修正前/后 DLL 分别跑同一夹具并固定身份；修正后要求 raw reply 不含 e/E，
   wrapper/直接 carrier 解码值均接近冻结的 CPU 时间，随后 render 从同一个起点推进。

现阶段可信结论上限：**同件原版解析器已证明不支持指数，失败候选确实允许输出指数；
这是一条可复现且高度吻合的接口缺陷。旧 c 当次 raw reply 未保存，精确载荷和修复后实机门待主线程闭合。**
