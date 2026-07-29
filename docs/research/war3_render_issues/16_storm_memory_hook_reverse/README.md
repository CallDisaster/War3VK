# Storm.dll 内存钩子 ASM 逆向记录

## 目标
- 固化 `Storm.dll` 的 `SMemAlloc/Free/GetSize/ReAlloc` 关键 ASM 结论。
- 明确原生头布局读取偏移，避免后续误用“伪 Storm 头”导致崩溃。
- 为 `src/d3d9/war3/memory/war3_storm_hook.*` 与 `war3_tlsf_pool.*` 提供约束依据。

## 2026-07-25：managed ReAlloc 的 16 字节 ABI 修复

高压图 `(4)生与死v1.28读档bug修复.w3x` 在旧候选上稳定于约 37 秒加载阶段
崩溃。minidump 显示 `Game.dll+0xFB135` 向 null-derived `0x00200000` 写入；
GDB 在 Storm 拒绝点捕获到一个仍属于 TLSF、头与 cookie 均有效、近期释放表槽为
0 的 live managed block，但用户地址以 `...A08` 结尾，只满足 8 字节对齐。

根因是初始 managed block 通过 `TlsfPool_AllocAligned(..., 16)` 创建，而普通
`TlsfPool_Realloc` 的 moving path 只保证 TLSF 基础对齐。旧实现把移动后的 8-byte
aligned block 重新发布；下一次 `QueryManagedBlock` 又按公开 16-byte ABI 拒绝
它，最终把 null 交给会无条件保存返回值的 Game 容器扩容代码。

当前产品合同：

- `AllocManagedBlock` 在发布前验证 raw pointer 的 16-byte alignment，失败即释放并
  fail-closed；
- managed TLSF→TLSF ReAlloc 只允许 `TlsfPool_ReallocInPlace` 原地调整；
- 原地失败时由 `AllocManagedBlock(16-byte aligned) + copy + release old` 完成
  moving realloc；
- managed 路径禁止重新引入普通 `TlsfPool_Realloc`。

离线合同现为 18/18 PASS。最终 exact DLL
`F4020142D8632D160747EB515E3CC5B97A901C667195C1755E71B8D56C252582`
在同一高压图连续通过 109.751 秒与 115.662 秒完整隔离运行，两轮均
`framesIncomplete=0` 且无新 crash dump。

## 2026-07-18：稳定产品策略已整合（build-only）

当前 DXVK 内置版本固定采用 StormBreaker v1.3.0 的正收益边界：

- `size >= 0xFE7C` 才进入 TLSF；小块继续使用 Storm 原生 allocator；
- `SMemAlloc/Free/GetSize/ReAlloc` 四个导出与原生小块 `search` 修复作为同一批次安装；
- 未设置 `DXVK_WAR3_STORM_TAKEOVER_MODE` 时即为稳定策略，显式值只接受
  `stable/large`；`full/hybrid` 在产品 DLL 中直接拒绝，仍只允许独立内存诊断使用；
- Storm 身份必须同时闭合文件 SHA-256、PE 字段、导出 RVA 与前导字节。当前唯一接受的
  `Storm.dll` 为 334,312 字节，SHA-256
  `F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB`；
- 原生大块释放或确实搬迁的 ReAlloc 会按 `[user-16]` 实际大小与 Storm 压缩头记账大小的
  差额，原子修正 `g_TotalAllocatedMemory @ +0x5738C`。`size==0` 依照 ordinal 405 的释放语义
  处理，`0x10` no-move 不做释放补账；
- TLSF 的 64 KiB 页目录只负责 O(1) 负过滤。精确块起点校验、私有头读取与
  `LIVE -> BUSY` claim 必须在 allocator 生命周期锁内完成；Free 后端若拒绝精确块，旧头和
  tombstone 会恢复，绝不能把候选指针落回 Storm；
- pool 固定在低 2 GiB，关闭 vendored 进程级小块 cache，按真实 size class 扩池，并在扩展池
  完全空闲时即时退役。所有 Alloc/ReAlloc 在首次进入 TLSF 前均做 size/alignment 可表示性检查；
- Hook 回滚若无法证明已移除，会永久保留 trampoline 并进入原生透传保护态，禁止再次安装。

standalone 兼容事件的旧协议只有固定名称，因此本质上是 Windows 会话级广播而不是进程互斥。
当前实现会先拒绝本进程已加载的已知 `StormBreaker*.asi`，发布事件后再次校验全部 Hook 目标；
另一个 DXVK 进程已有事件时不会误阻断本进程。重命名的旧 ASI 若恰好与 DXVK 同时越过各自检查，
仍没有跨实现的原子握手协议，不能宣称该异常并发场景已被数学闭合。

近期释放表与 v1.3.0 一样是 4096 槽有界诊断防线。正常成对生命周期由 exact block + claim
闭合；哈希碰撞后的非法 double-free/UAF 不属于永久拒绝证明，文档和日志不得把它表述成证明。

离线合同 `AutoTest/test_stormbreaker_stable_policy_offline.py` 当前 17/17 通过。唯一主线程已完成
`ninja -C build32 src/d3d9/d3d9.dll -j2`，随后 no-work；DLL 为 29,250,964 字节，SHA-256
`45E26440CBF33BFBC125CA4128693343C6879E954D1082817BC5C666F9363CF6`。本轮尚未部署、未启动
Warcraft III，不能据此声明运行正确或内存收益。下一运行门先做隔离 crash gate；资源普查必须使用
`(4)生与死v1.28读档bug修复.w3x`，光影测试图或 SunkenCity 的结果不能替代重图驻留证据。

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
  - 页目录负过滤后，在 allocator 锁内完成 exact block、`StormTlsfHeader` 与原子 claim
  - 不再维护 `unordered_map<void*, size_t>`
- `src/d3d9/war3/memory/war3_tlsf_pool.cpp`
  - 使用原子页目录做负过滤，扩展池遍历与退役由同一 allocator 锁保护

## 2026-03-13 历史遗留问题（已被上方稳定路线取代）
- 当时版本在游戏启动早期即崩。
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
