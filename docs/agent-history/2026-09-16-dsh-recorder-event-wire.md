# 2026-09-16 CPU 取证历史外置 E1：WVE1 协议编解码与 Golden 验证（返修二）

状态：按主线程二次审核意见完成返修。全面审查 C++ 负例夹具，确保每个用例仅包含单一目标缺陷；
保留主线程在 line 340 的独立 array 修改；消除未分配缓冲范围；记录 codec 大 count 边界；
完善 Python 测试可选 `--output` 门与无参纯内存运行。待主线程统一编译复核。
旧 dxvk 工作区只读；唯一写入树为 `dxvk-v1.22-integration-20260914`。
基线基于 `codex/backup-before-recorder-offload-20260916`（原 284 处 dirty 原样保留）。

## 1. 交付物清单与身份

| 文件 | 字节大小 | SHA-256 |
|---|---|---|
| `tools/render_host/recorder_event_wire.cpp` | 12406 | `9c45dfeab04662add6c6621fd13b9217eca7d827533bea90be9e713f21375cab` |
| `AutoTest/test_recorder_event_wire.cpp` | 27959 | `8b2eb63f786d603ce6fb48674241f1238c3e397a004018763d37fc7b07ed3f08` |
| `AutoTest/test_recorder_event_wire_golden.py` | 24693 | `0404c7226d3667efbe9b3f5538eb3c6d00d075f7af48d22853ba7f7e2e5dd19d` |
| `docs/agent-history/2026-09-16-dsh-recorder-event-wire.md`（本报告） | 见文件系统 | 按约定不自哈希 |

## 2. 负例测试夹具审查与修正

对照主线程 `wire-r1`、`wire-r2` 回执，逐项排查相互干扰与未分配范围：
1. **保留主线程 Null 修复**：`TestNullBufferRejection` 中保留主线程修改，使用独立 `independentBuffer`，消除与 `&written` 的别名误报。
2. **Seal Shape 单一缺陷修正（r2 line 560）**：`badH.op = Op::Seal; badH.count = 1` 时，原用例继承了 Golden 的 `trigger = 100` 与 `accepted = 0`，导致先触发 `Trigger` 错误。现修正显式重置 `trigger = NoTrigger`、`accepted = 50`、`attempted = 50`、`lost = 0`、`reason = 1`，确保仅 `count = 1` 为唯一错误。`reason = 0` 与 `reason = 5` 用例同理修正。
3. **消除未分配事件缓冲范围**：`badH.count = MaxEvents + 1` (161) 时，原用例传递单元素 `&ev` 导致 overlap 检查探测未分配栈空间。现分配真实的 `std::vector<Event> events161(MaxEvents + 1, ev)` 堆内存，彻底消除未分配缓冲。
4. **Totals 与 Trigger 负例解耦**：`TestTotalsRejection` 中 Seal 负例显式设 `trigger = NoTrigger`，杜绝 `trigger > accepted` 掩盖；`TestTriggerRejection` 中 Seal 负例显式设 `count = 0`，杜绝 Shape 掩盖。
5. **范围溢出测试**：`TestRangeOverflowAsOverlap` 传入 `SIZE_MAX` 验证范围计算溢出安全归类为 `Overlap`，不发生内存越界访问。

## 3. Codec 边界说明

当前 `tools/render_host/recorder_event_wire.cpp` 中 `Encode` 的实现边界：
- 当 `events && header.count > 0` 时，先计算 `eventBytes`。若 `header.count > SIZE_MAX / sizeof(Event)`，则视为范围溢出立即返回 `WireError::Overlap` 且不修改输出引用。
- 在此判定之后方执行常规字段校验与输出清空。按主线程要求，本次保持既有严格拒绝实现，不自行变更协议，如需收紧由主线程后续统一裁定。

## 4. Python 测试与 Golden 规范

1. **零 I/O 导入与可选输出**：
   - 导入期无任何磁盘写入。
   - 默认无参数执行：纯内存运行，控制台输出 `GOLDEN=<hex>` 与 `checks=24 PASS`，不写文件。
   - 可选 `--output <DIR>`：仅在目录不存在时新建并写入 `golden.hex`, `golden.bin`, `summary.json`；若目录已存在立即拒绝（exit 1）。
2. **Golden 数据包保持未改**：
   - 大小：472 字节（80 + 392）。
   - SHA-256：`2401882cff8cb5e50d23911ce278168375f5e33cccfe76f36d9a808c2ffa11ee`。
   - 浮点位模式核实：Golden 包包含 `+0.0f`, `-0.0f`, `1.0f`, `-2.0f`, `0.25f`, `-1.0f`, `0.5f`, `100.0f`, `-100.0f`, 次正规数, 最小/最大正规数, 以及特征 NaN (`0x7FFFFFFF`, `0xFFFFFFFF`)。正负无穷 `+Inf` (`0x7F800000`), `-Inf` (`0xFF800000`) 在 C++ 独立测试 `TestFloatBitsPreservation()` 中验证，未编入当前 Golden 包。

## 5. 验证执行与未执行项

- **真正执行**：
  1. `python -B AutoTest/test_recorder_event_wire_golden.py`：24/24 项全通过，纯内存无写入。
  2. 验证 `--output` 门：已有目录拒绝、全新目录写入全通过。
  3. C++ 静态符号与括号配对检查通过（Brace balance: 0, Paren balance: 0）。
- **未执行项**：未运行 C++ 编译器（等待主线程串行 BelowNormal 统一编译复核）；无 GPU、无游戏、无 Native Record、无进程创建。