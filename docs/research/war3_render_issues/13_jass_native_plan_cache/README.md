# Task-4：JASS Native 调用计划缓存（ASM 驱动）

## 目标
1. 只优化 `JassInterpreter_MainLoop(case21) -> ExecuteNativeFunction` 热路径。
2. 不改变 JASS 语义：返回值、参数弹栈顺序、native 调用顺序保持一致。
3. 默认关闭快路径，保留一键回退到 trampoline。

## ASM 依据（仅 ASM，不使用反编译 C）
1. `JassInterpreter_MainLoop(0x6F7F1A20)` 在 `0x6F7F2069` 调用 `ExecuteNativeFunction(0x6F7EF590)`。
2. `ExecuteNativeFunction` 关键流：
   - `GetTLSJassData(0x6F7E2FE0)` 获取 native entry；
   - 从 `entry+0x20/+0x24` 读取参数个数与签名；
   - 参数循环读取 `stack(+0x8C)` 与 `stack[index]+0x18/+0x20`；
   - 特殊分支调用 `RegFuncAddr2Handle(0x6F7ECE50)`、`ComputeHandleMemoryAddr(0x6F7EDA90)`；
   - `alloca + MemCopyWrapper(0x6F7EF330) + call [entry+0x1C]`；
   - 调用后执行 `stackTop -= entry->paramCount`。
3. 注册链 `sub_6F7E3710 -> sub_6F7DEBE0` 写入 native 元信息：
   - `entry+0x1C`: native function ptr
   - `entry+0x20`: paramCount
   - `entry+0x24`: signature
   - `entry+0x38`: return type

## 本轮实现
1. AddressBook 增加 Task-4 地址字段：
   - `executeNativeFunction=0x7EF590`
   - `jassFuncPauseAndCreateFrame=0x7F1810`
   - `getTlsJassData=0x7E2FE0`
   - `regFuncAddr2Handle=0x7ECE50`
   - `computeHandleMemoryAddr=0x7EDA90`
2. 新增编译期开关（默认安全档）：
   - `kNativeJassNativeCallHookEnabled=false`
   - `kNativeJassNativeCallPlanCacheEnabled=true`
   - `kNativeJassNativeCallFastInvokeEnabled=false`
3. 新增模块：
   - `war3_jass_native_plan_cache.*`
   - `war3_jass_native_invoke_x86.*`
4. `war3_hook_jass.cpp` 新增 `ExecuteNativeFunction` hook 接口与回退路径。

## 缓存与执行策略
1. 缓存键：`nativeEntryPtr`（`GetTLSJassData(arg0)` 返回值）。
2. 缓存层：TLS L1（8 槽）+ 全局 L2（`unordered_map`）。
3. 计划命中前校验：`funcPtr/sigPtr/paramCount` 任一变化即重建。
4. 失败条件统一回退：签名非法、参数越界、helper 不可用、调用器不可用。

## 风险控制
1. 发布默认不启用 Task-4 hook 与 fast invoke。
2. 非 x86 或不支持调用器时，自动回退原始路径。
3. 统计日志默认关闭，避免热路径额外噪声。
