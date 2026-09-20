# 2026-09-16 DSH 机械批次 01：定向 Python 白名单入口与自测

状态：M1/M2 已实现并自测，待主线程最终边界验收。仅 Python/静态/synthetic：无 Ninja、无
native/DLL 构建、未运行任何 run_* native gate、无游戏/GPU/部署、未提交推送、无子会话；
旧 dxvk 树只读；未 reset/clean/checkout。基线核对：HEAD `ae890542d766470d1703f5bea7f5b73636039733`、
branch `codex/v1.22-release-integration-20260914`，与 checkpoint-final（269 dirty）一致；
开工前 3 个授权文件与 artifacts 目录均不存在。

## 交付物身份

| 文件 | size (bytes) | SHA-256 |
| --- | --- | --- |
| `AutoTest/run_architecture_python_checks.py` | 17650 | `f1026513a267ab7ff18fb4dd73463505ce153c2271a5fffef58e2b9b9e70034d` |
| `AutoTest/test_architecture_python_checks_runner.py` | 30836 | `602c986d3caa0fef19cf1ce07783b012ad2a207153c7083bcd1ebf81c09d1a0c` |
| 本报告 | 按合同不自哈希（size 见最终回传） | — |

## 15 份脚本覆盖表

| # | 脚本 | 实际验证 | 性质 | 不能证明 |
| --- | --- | --- | --- | --- |
| 1 | `test_analyze_frame_evidence.py` | `analyze_frame_evidence`：schema 1/5/7 严格字段/布尔校验、u64、JSON 重复键/NaN 拒收、伪验收/伪造像素能力/序列断档拒收、截图金丝雀精确关联（PIL 临时真实 TGA） | synthetic+真实图像，纯 CPU | DLL 运行时、GPU 截图管道、游戏内捕获 |
| 2 | `test_analyze_frame_history.py` | `analyze_history`：6 帧关联、时间预算 300ms 精确边界、伪造 `preSpanTicks`/断档/缺 copy/错 epoch 拒收 | synthetic+真实图像 | GPU 回读时序、实机行为 |
| 3 | `test_frame_inputs.py` | struct 构造二进制的重建/gather 回环/调色板等价/NaN 不修复等 + `war3_frame_inputs.cpp`、gather shader、scene 头静态 pin | synthetic 解析(numpy)+静态 | GPU gather 实际执行、游戏内重建 |
| 4 | `test_frame_evidence_control_static.py` | 控制动作、默认关+CREATE_NEW、热路径无文件/JSON、身份与环槽分离、caster 不混哈希、流式冻结导出、热生产者无共享互斥 | 静态源码 pin | 运行时/链接行为 |
| 5 | `test_frame_history_static.py` | 捕获期无 IO、触发后才回读、完成先于读取、无 GPU 等待、热键仅游戏窗口、CREATE_NEW 保留玩家文件 | 静态源码 pin | 运行时 |
| 6 | `test_frame_history_shortcut_static.py` | 回调先于 IME、旧回调非权威、Ctrl+Shift+C、HUD 边界、1 秒按时间非固定帧、watcher pid 复用 | 静态源码 pin | 玩家机器实际按键投递 |
| 7 | `test_frame_recorder_self_contained_static.py` | worker 解析 profile/锁外 join、租约签发-先 join 后释放、launcher 无必需 Python、图像状态单一权威 | 静态源码 pin | 实际 join/租约运行时序 |
| 8 | `test_frame_recorder_memory_static.py` | 先准入后分配/代际、清理先于提交、采样器只读限本进程（无 VirtualAlloc/OpenProcess/写内存）、测试分离链接 | 静态源码 pin | 高压下真实分配行为 |
| 9 | `test_frame_recorder_default_policy_static.py` | release 选项 false、默认单一权威、profile 测生产控制、资源守卫先于 arm、不启用渲染候选 | 静态源码 pin | 构建出的 DLL 默认值 |
| 10 | `test_self_contained_recorder_gate.py` | `use_build_defaults` 剥离激活/输出变量、`incident_paths` owner/session/nonce/缺失/越界/伪验收全拒收、禁 watcher/子进程的源码+AST pin | synthetic 纯函数+静态 | gate 本体运行（main 未执行） |
| 11 | `test_recorder_control_lease_gate.py` | `syntax_arguments` 剥离输出/依赖开关补 `-fsyntax-only`、未知形式/响应文件拒收；`split_windows` 用真实 `CommandLineToArgvW` | synthetic+实际 Win32 解析 | 实际编译器调用（main 未运行） |
| 12 | `test_render_host_transport_gate.py` | 传输回执接受/变异：位数/对端验证/句柄/写入数/取消请求≠完成/截止不可重置/超限不读体/错对端先于协议 | synthetic（自述非 IPC 替代） | 真实管道 IO |
| 13 | `test_render_host_shared_slots_gate.py` | RH1 回执正负例、独立摘要、CPU 拷贝与 ACK 匹配、soak 48 轮逐轮/无复用/无换标、WVS1 golden 72 字节 | synthetic+golden | 真实共享内存映射 |
| 14 | `test_render_host_producer_inbox_gate.py` | 队列回执两位宽正例、计数器类型/范围、种群不可掩盖丢样、gpuTested/ipcTested 必须为 false、缺失字段不默认零 | synthetic JSON 合同 | 真实线程时序 |
| 15 | `test_render_host_sample_worker_gate.py` | normal/disconnect/slow 三形态、独立字节非仅自带摘要、scope/源/租约配对、种群拷贝代数、真实 QPC 窗口、严格解析器 | synthetic（自述非 worker IPC 替代） | 真实 worker IPC |

共同限制：全为 CPU/离线证据；不证明产品 DLL、GPU 生命周期、玩家前台、崩溃修复或发布接受；
对应 native gate 独立存在且本批未调用其 main。

## 实际命令与结果（cwd=集成树根；`python`=3.13.11，均 `-B`）

- 精确区分：两文件 `py_compile` 由主线程复验执行；我方自身语法检查仅为 `ast.parse`（不写 pyc）；我方唯一一次 `py_compile` 尝试因参数引号错误未成功执行（详见第 3 节）。
- `python -B AutoTest/test_architecture_python_checks_runner.py` → **35/35 OK，exit 0**。
- `python -B AutoTest/run_architecture_python_checks.py --list --group recorder|render-host|all`
  → 3×exit 0（11/4/15 份；`verified:false`，不导入不执行）。
- `--group recorder --output …final_run_recorder.json` → exit 0，11/11 rc=0；
  `--group render-host --output …` → exit 0，4/4；`--group all --output …final_run_all.json`
  → exit 0，15/15。三组 `schema=1`、`scope=TARGETED_PYTHON_STATIC_AND_SYNTHETIC_ONLY`、
  `not_run=[native,gpu,game,dll_build,deployment]`、`identity_changes=0`（逐脚本前后 size/SHA 一致）、
  `output.error=null`、入口 SHA `f1026513…0034d`；最慢脚本 10.76s < 120s。
- 负例探针：`--group everything` → exit 2；`--output` 指向已存在文件 → exit 1、目标字节不变、
  stdout 无 JSON、零启动；`--list --output` → exit 2 且未建文件。
- 证据目录 `AutoTest/artifacts/dsh_mechanical_20260916_batch01/`：修正前中间 `run_recorder.json`
  与 `final_*` 全套（json/stdout/stderr/list/probe/selftest 转录），未覆盖未删除。

## 限制与既定取舍

- SHA 只证明所选脚本与入口在捕获时刻的身份，不覆盖间接依赖（`analyze_*`/`run_*_gate` 模块、
  旧树只读导入链、引用树 vendored mcp 1.30——导入无副作用，服务由 `__main__` 守卫）。
- 更正（2026-09-16 第二批审查指出，此前陈述对 stderr 有误）：15/15 脚本记录的 stderr 全部
  非空——unittest 的进度点与 `Ran N tests`/`OK` 摘要输出到 stderr（实测 108–151 bytes，
  与父线程 dsh_mechanical_20260916_parent_review/run-all-r2.json 一致）；stdout 全部为 0；
  均未截断。64KiB 截断/膨胀语义由 35 项自测直接覆盖。**64KiB 只是记录摘要的截断上限，
  不是子进程内存限额，也不是进程级内存保护。**
- `truncated`：任何内容被丢弃（原始超限或替换解码膨胀收缩）即 true；原始字节计数始终保留。
- `--output` 已存在的拒绝路径：stderr+exit 1，无 stdout JSON（拒绝≠一次运行）；
  `--output` 相对路径按启动 cwd 解析，树根由脚本位置推导，二者独立。
- 依赖现状：PIL 12.1.0、numpy 2.4.0 可用；若缺失将以导入失败原样报告，不由入口掩盖。

## 中途修正（主线程审查驱动，均已复验）

1. 输出发布失败并入最终机器可读结果：发布尝试先于 stdout，失败写 `output.error` 并强制
   `ok=false`、exit 非 0；新增真实 main 反例（mock open/write 抛 OSError → exit 非 0 且
   stdout JSON `ok=false`）与"父目录缺失"自然 CreateNew 失败反例。
2. `clip_stream` 替换膨胀修复（`b'\xff'*70000` 旧结果 196608 bytes → 现 65535 ≤ 65536）：
   UTF-8 编码超限按字符前缀单调二分收缩，丢弃即 `truncated=true`，raw 计数保留；
   新增 4 个真实函数反例（invalid UTF-8 / 边界多字节字符丢弃与恰好填满 / 低于上限的膨胀）。
3. 我方错误（均测试侧修正，入口行为正确）：误用 `py_compile` 自建 1 个 `.pyc` 已删
   （目录内其余 `.pyc` 为 05:46 夜间既有，未动）；旧用例误用已存在目录（实为拒绝分支）；
   mock 路径未 resolve 被 8.3 短名（`ADMINI~1`→`Administrator`）击败。