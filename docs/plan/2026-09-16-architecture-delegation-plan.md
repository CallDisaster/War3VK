# 主线程设计 / DSH机械实施：第一批分工

用户授权：将可机械执行的工作交给dxvk工作区的新DSH对话，主线程保留核心设计和验收。
最初供应商/model为`scnet / GLM-5.3`；用户随后明确允许较慢时改用`GLM-5.3-Flash`，
当前已核实并显式选择`scnet / GLM-5.3-Flash`。工具返回无可选reasoningEffort，不自行替换其它供应商或模型。
会话：`session-a9b8cb49-7270-4234-898a-93ff8849114b`。

## 工作树与角色

- DSH会话归属：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`，该旧树只读。
- 唯一可写树：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk-v1.22-integration-20260914`。
- 基线：branch `codex/v1.22-release-integration-20260914`，HEAD `ae890542d766470d1703f5bea7f5b73636039733`，
  269 dirty；身份参照`AutoTest/artifacts/overnight_architecture_20260916/checkpoint-final.json`。
- 主线程负责：设计取舍、协议/状态机、生产接线、GPU寿命、最终差异审核、验证结论和开发日志。
- DSH负责：按下述冻结规格实现测试入口与入口自身测试、列举现有测试范围。不得自行扩大为生产重构。
- 本批不涉及C++/shader/ABI/Meson修改，不构建DLL或native程序，不运行游戏/GPU，不部署，不提交/推送。
  不动旧证据和调试器，不恢复夜间自动化，不创建更多子会话。只使用apply_patch编辑授权文本。

## 第一批可执行机械任务

### M1：显式白名单的纯Python定向测试入口

新增`AutoTest/run_architecture_python_checks.py`，只使用标准库。

- `--group recorder|render-host|all`，默认all；`--list`只列出选择，不能导入或执行测试。
- `--output <new.json>`可选；存在即在执行测试之前拒绝，写入使用CreateNew/x，不覆盖或删除。
  默认只向stdout输出汇总JSON。未知组由CLI拒绝；禁止接受任意脚本名、glob或自动discover。
- 根目录从脚本位置推导，不依赖启动cwd；逐个用`sys.executable -B <绝对测试路径>`执行，cwd固定为本集成树。
  不用shell，不并行。Windows子进程BelowNormal/CREATE_NO_WINDOW；每个脚本固定120秒超时。
- 在任何测试启动前确认整个选中清单无重复且文件存在。每个脚本记录前后size/SHA、returncode、超时/启动错误、
  stdout/stderr；后两者单项最多64KiB并标明截断。测试数量指脚本数，不猜测为断言数量或全量回归。
- 单脚本失败仍收集后续白名单脚本结果；任一失败、超时、源脚本身份变化或零执行均使ok=false/exit非0。
  `--list`是列举模式，不称验证通过。现有测试失败必须原样报告，不能改断言/skip/阈值来消除失败。
- 汇总固定schema1、scope=`TARGETED_PYTHON_STATIC_AND_SYNTHETIC_ONLY`，明确没有运行native/GPU/game/DLL构建。
  SHA只证明选择的脚本与入口身份，不声称覆盖所有间接依赖；已有native gate保持独立，不调用它们的main。

recorder精确顺序（11份）：

1. `test_analyze_frame_evidence.py`
2. `test_analyze_frame_history.py`
3. `test_frame_inputs.py`
4. `test_frame_evidence_control_static.py`
5. `test_frame_history_static.py`
6. `test_frame_history_shortcut_static.py`
7. `test_frame_recorder_self_contained_static.py`
8. `test_frame_recorder_memory_static.py`
9. `test_frame_recorder_default_policy_static.py`
10. `test_self_contained_recorder_gate.py`
11. `test_recorder_control_lease_gate.py`

render-host精确顺序（4份）：

1. `test_render_host_transport_gate.py`
2. `test_render_host_shared_slots_gate.py`
3. `test_render_host_producer_inbox_gate.py`
4. `test_render_host_sample_worker_gate.py`

all为两个清单按上述顺序连接，共15份，不自动加入本次入口测试造成递归。

### M2：入口自身的正反例与测试清单

新增`AutoTest/test_architecture_python_checks_runner.py`，使用标准unittest和mock；验证真实入口函数而非重写模型。
至少覆盖：组与确定顺序；无重复；list不启动；未知组；任一文件缺失不启动；已有output不启动且不改字节；
正确python/绝对路径/cwd/shell=false/串行参数；成功；失败继续但汇总非0；timeout；启动异常；源文件变化；
日志截断标识；输出CreateNew失败不谎报成功。测试用临时目录/mock，不运行native或游戏。

新增`docs/agent-history/2026-09-16-dsh-mechanical-batch-01.md`：

- 按15份脚本逐项列出它实际验证的对象、静态还是synthetic/实际Windows解析、不能证明什么。
- 记录自己的命令、结果、3个授权文件size/SHA（报告本身不自哈希）、失败与限制。
- 若现有脚本失败或规格存在歧义，给出原始失败，不擅改既有文件或生产合同；主线程裁定。

## 精确写入分工

DSH仅可新增上述3个文件；测试输出仅可放自己的全新ignored目录
`AutoTest/artifacts/dsh_mechanical_20260916_batch01/`（若已存在停下，不覆盖/清理）。
可运行这15份Python测试、自己的入口测试以及新Python的语法检查。不得运行任何run_* native/实机gate。
主线程此时只写本计划、独立终态设计文档和DEVELOPMENT_CHANGELOG；不写DSH的3个文件。
共享已有源码/测试均只读。若开工发现其他人修改授权文件或新文件已存在，停下回报。

## 验收及后续队列

主线程收到结果后审核白名单、是否存在隐式执行/跳过、全部差异与旧路径SHA，再独立重跑定向入口和自身测试。
只有验收后才将M1/M2标记完成。创建会话或任务accepted不是完成。

下一批候选（本次未授权DSH实施）：按主线程冻结的终态接口补测试矩阵/接线静态断言；
再往后才考虑已证明等价的重复工具函数提取。数据选择、回退政策、缓存key、GPU释放与IPC协议不得当作机械任务外包。

## 长期任务池：先满足条件，再派发

| 可拆出的工作 | 子线程可做的机械部分 | 必须先由主线程给出的设计/验收 |
| --- | --- | --- |
| 终态收敛 | 按固定输入/输出表补CPU测试、状态枚举接线断言 | 完成/失败/包完成的区别、唯一提交接口、锁序及合法恢复 |
| 旧旁路清点 | 指定模块内列调用点/开关默认值/读写字段/测试覆盖，不改来源策略 | 哪条路径是权威入口、何时能够退役；不把rg无命中当语义证明 |
| 已证明等价的helper提取 | 按主线程签发的符号和调用映射搬代码、改include、登记既有测试 | 行为等价、依赖方向、布局/ABI和构建影响；禁止自行合并看似相同的图形算法 |
| 字段/接口迁移 | 按精确字段表更新序列化、reader、文档及错误输入测试 | schema版本、兼容与拒绝策略；禁止last-wins、隐式补缺值或沿用旧通过证据 |
| 验证维护 | 显式测试清单、运行回执、正反例、命令可复现性 | 通过阈值、证明范围与缺项处理；不能把skip/全拒绝记为修复 |

每批只给一组可独立审核的写路径；前一批验收完成才下发下一批。需要独立执行的第二子会话时再明确其文件范围，
不让多个代理同时改同一生产文件/总日志。不按“一个模型负责所有测试、另一个负责所有源码”无限授权。
关键渲染链的VB/IB/palette/material选择、缓存相等性、GPU退役、32/64通信及安全预算始终由主线程裁定。
