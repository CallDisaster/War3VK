# Storm.dll 内存钩子 ASM 逆向记录

## 目标
- 固化 `Storm.dll` 的 `SMemAlloc/Free/GetSize/ReAlloc` 关键 ASM 结论。
- 明确原生头布局读取偏移，避免后续误用“伪 Storm 头”导致崩溃。
- 为 `src/d3d9/war3/memory/war3_storm_hook.*` 与 `war3_tlsf_pool.*` 提供约束依据。

## 当前环境
- 模块：`Storm.dll`
- 逆向方式：仅使用 IDA ASM 反汇编，不采用伪代码
- 结论日期：2026-03-13

## 导出入口
- `Ordinal 401 @ 0x1502B830`：`Storm_MemAlloc`
- `Ordinal 403 @ 0x1502BE40`：`Storm_MemFree`
- `Ordinal 404 @ 0x1502C000`：`SMemGetSize`
- `Ordinal 405 @ 0x1502C8B0`：`Storm_MemReAlloc`

## 原生指针校验
### `Storm_CheckMemPointer @ 0x1502A830`
- 固定检查 `word ptr [user-2] == 0x6F6D`
- 固定读取 `byte ptr [user-5]` 作为 flags
- 当 flags 某位开启时，会继续读取：
  - `byte ptr [user-6]`
  - `word ptr [user-8]`
  - 基于偏移计算出的更早前导位置
- 该路径还会校验 `0x12B1` 哨兵

工程结论：
- 只要我们返回给游戏的块没有真实满足这些前导字段，原生 `Storm_CheckMemPointer` 就不会把它当成合法 Storm 块。
- 这比“伪造一个近似 Storm 头然后赌后续字段不被访问”安全得多。

## `SMemFree` 关键读取偏移
### `Storm_MemFree @ 0x1502BE40`
关键访存顺序：
- 先走 `Storm_CheckMemPointer`
- 之后直接读 `byte ptr [ebx-5]`
- 默认按 `lea edi, [ebx-8]` 取头
- 若 `test byte ptr [ebx-5], 8` 为真：
  - 走 `mov edi, [edi-4]`
  - 即读取 `dword ptr [user-12]` 作为回退头指针
- 然后读取 `word ptr [edi+4]`
- 再把该值左移 16 位，用来拼所属 heap/critical-section 相关地址

工程结论：
- 原生 `SMemFree` 不只是看 `[user-2]` 魔数。
- 它会沿着 `user-8`、`user-12` 继续解引用并参与堆锁定位。
- 任何不精确的假头，一旦漏进原生 `SMemFree`，都可能在启动期直接写坏链表或 `EnterCriticalSection` 崩溃。

## `SMemGetSize` 关键读取偏移
### `SMemGetSize @ 0x1502C000`
- 同样先依赖 `Storm_CheckMemPointer`
- 也会读取 `user-5`、`user-8`、`user-12`
- 某个大块路径上会直接读取 `dword ptr [user-16]` 作为 size

工程结论：
- `GetSize` 同样区分小块/大块前导布局。
- “只伪装 Free 能过”也不够，`GetSize` 会再暴露头布局错误。

## `StormHeap_AllocPage` 头布局结论
### `StormHeap_AllocPage @ 0x1502A510`
已确认原生存在至少两类头：
- 小块路径：用户指针前是 8 字节风格的压缩头
- 大块路径：用户指针前存在 12/16 字节扩展头

已确认写入行为：
- 小块路径会写：
  - `[hdr+0] = blockSize`
  - `[hdr+2] = prefixAdjust`
  - `[hdr+3] = flags`
  - `[hdr+4] = packed heapRef`
- 其中 `packed heapRef` 的高 16 位包含 `0x6F6D`
- 大块路径额外写：
  - `dword ptr [user-16] = requestedSize`
  - `dword ptr [user-12] = baseHeaderPtr`
  - `user-8..-1` 仍有一组带 flags 的压缩头

工程结论：
- 原生头不是“固定 10 字节”。
- 旧版 `StormCompatHeader` 这种简化伪头是不精确的，不能继续作为生产假设。

## 对 StormBreaker 接管策略的约束
### 精准性
- “只接管大块，放行小块”方向是对的。
- 原因不是抽象上的“感觉更稳”，而是 Storm 本身就是多 heap/多锁分配体系，小块放给原生更符合其缓存和碎片模型。

### 安全性
- 接管块不能伪装成原生 Storm 块。
- 更安全的做法是：
  - 用 TLSF 私有头记录 `magic/requestedSize/cookie`
  - 让 `[user-2]` 故意不是 `0x6F6D`
  - 如果有漏钩路径把我们的块喂进原生 `SMemFree/GetSize/ReAlloc`，原生会在 `Storm_CheckMemPointer` 处直接拒绝，而不是继续误解引用

### 并发性
- 指针识别不应依赖全局 `unordered_map + mutex`
- 推荐路径：
  - `TlsfPool_IsFromPool(raw)` 做地址范围快判
  - 然后直接校验私有头
- 这样可以避免多线程资源加载时的全局锁竞争

## 当前代码约束
- `src/d3d9/war3/memory/war3_storm_hook.h`
  - `StormTlsfHeader` 固定 16 字节
  - `rejectTag` 明确不写 `0x6F6D`
- `src/d3d9/war3/memory/war3_storm_hook.cpp`
  - 零锁识别：`TlsfPool_IsFromPool + StormTlsfHeader`
  - 不再维护 `unordered_map<void*, size_t>`
- `src/d3d9/war3/memory/war3_tlsf_pool.cpp`
  - 使用原子范围快照，避免扩展池遍历数据竞争

## 当前遗留问题
- 2026-03-13 当前版本在游戏启动早期即崩。
- 崩溃地址不在 `Storm.dll`，而是在 `Game.dll`：
  - 运行时 `Game.dll base = 0x68C90000`
  - 崩溃 `EIP = 0x68FBC2CB`
  - 对应 `RVA = 0x0032C2CB`
- 下一步要继续从 `Game.dll` 的启动期调用链确认：
  - 哪个大块 `SMemAlloc` 被接管了
  - 该分配是否要求更严格的 Storm 原生语义

## 结论
- 当前最重要的不是“把假头继续补细”，而是坚持“不要伪装成 Storm 原生块”这条底线。
- 只接管大块依旧是正确方向，但必须继续确认启动期是否存在一类大块虽然来自 `SMemAlloc`，却仍然依赖原生 Storm 的额外语义或生命周期配对。
