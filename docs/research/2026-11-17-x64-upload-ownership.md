# 代理模式纹理更新的所有权链（2026-11-17，只读分析）

范围：`IDirect3DTexture9::LockRect/UnlockRect` 一次纹理层更新，从 x86 游戏写字节到 x64 宿主 GPU 最后一次使用。
只读代码与 P0 计数，不改设计；任何“换暂存方案”的结论都留给 Fable（P1）。
标注：**已核对**＝读过代码或计数恒等式成立；**推断**＝由代码推理，未经实测或未读到 DXVK 内部实现。

> **更正（2026-11-17，据只读复核 `2026-11-17-x64-texture-upload-and-exit-repair-design.md` §3）**：
> (1) 环记录的复用门是 `head`，不是 `applied`：`Consumer::tryRead` 先把整条记录复制进宿主 `Packet` 并 `head.store`，然后才执行；`applied` 是执行后另行发布的进度。原第 3 行与 §4“环记录｜宿主 applied”写错，已改。
> (2) CPU 负载复制原先漏了“环 → 宿主 Packet”一次、又把游戏写像素算进去；按游戏写完 Lock 数据为起点，正确计数是 **6 次 CPU 负载复制 + 1 次 GPU 复制**，见新的 §2 编号 ①–⑥。

## 1. 结论先说

- 一次更新经过 **6 次 CPU 负载复制（backing→打包、打包→环、环→Packet、Packet→事务、事务→staging、事务→managed 副本）+ 1 次 GPU 复制**、**3 次以上同步往返**、宿主 **2 次等 GPU 空闲**（已核对代码）。
  P0 实测（Lost Temple、Echo Isles、生与死进图窗口）：每世界帧 23–33 次更新，几乎全是 16×16 A8R8G8B8 单层、每张每帧一次、整层（1,024 B，1 个块）。
- 每段缓冲的“可复用时刻”大多早于现在实际等待的时刻：x86 侧的块缓冲在环发布时就可复用；宿主侧的单个 `owner.staging` 只需等到**本次拷贝那一个提交**完成即可复用，
  而现在每次更新前先 `settle()`（冲掉并等完整条世界流列表），后 `submit(true)`（再等一次）。（代码已核对；“只需等本次提交”是推断）
- 因此“只确认 Commit、块异步”这一类改动的上限不在 x86 侧，而在宿主侧：只要 `uploadTextureRegion` 仍是 settle+submit(true)，每次更新仍把世界流切成一段并等 GPU 空闲。

## 2. 所有权链（按时间顺序）

| # | 持有者 | 缓冲 | 谁写 / 谁读 | 何时可复用或释放 | 位置 |
|---|---|---|---|---|---|
| 1 | x86 游戏 | 纹理层常驻副本 `levelBytes[level]`（或首次上传时的上传窗口 `levelWindow[level]`） | 游戏在 `LockRect` 返回的指针里写 | `UnlockRect` 成功后锁释放；常驻副本保留，作为以后局部锁的权威 CPU 副本，受残留预算 LRU 驱逐（驱逐后再锁时用 `ReadContent` 从宿主取回） | `d3d9_proxy.cpp` `Texture::LockRect/UnlockRect/releaseLevel/restore`（已核对） |
| 2 | x86 前端 | 栈上 `std::array<uint8_t,MaxPayload>`（约 64 KiB）块缓冲 | `UnlockRect` 按行 memcpy（CPU 复制 ①） | 本块 `checkedCall` 发布进环之后即可复用（环已有副本） | 同上（已核对） |
| 3 | 共享内存命令环 | 环记录（头 + 负载） | `ringPublish` memcpy（CPU 复制 ②）；宿主 `tryRead` 读出 | 宿主把记录复制进自己的 `Packet` 并发布 `head` 越过它之后，生产者即可覆盖（在执行之前；**不是** `applied`）——更正 | `d3d9_command_ring.h` `Producer::tryWrite` / `Consumer::tryRead`（已核对） |
| 3b | x64 宿主 | `d9::Packet request`（主循环的一条记录私有副本） | `Consumer::tryRead` 的 `get(...)`（CPU 复制 ③）——原文漏计，更正 | 本条记录 apply 结束；下一条记录覆盖 | `d3d9_command_ring.h` `tryRead`，`d3d9_host_main.cpp` 主循环（已核对） |
| 4 | x64 宿主 | `TextureUpload::m_bytes`（`std::vector`，**每次 Begin 新分配**，大小=区域字节） | Begin 分配；每个 Chunk 从 Packet memcpy（CPU 复制 ④） | Commit 末尾 `reset()` 释放（Abort 同） | `d3d9_texture_wire.h` `TextureUpload`，`d3d9_host_main.cpp` Begin/Chunk/Commit（已核对） |
| 5 | x64 宿主 | 单个 `owner.staging`（主机可见、coherent，初始化时按 `MaxBufferBytes` 建，不够才换大的） | `uploadTextureRegion`：先 `settle()`，再从事务 memcpy（CPU 复制 ⑤） | `submit(true)` 返回时 GPU 已完成拷贝，下一次上传可覆盖 | `d3d9_backend.cpp` `uploadTextureRegion`（已核对） |
| 6 | GPU | 纹理图像 | `copyBufferToImage`（GPU 复制），单独一条命令列表，`submit(true)` 等完成 | 之后录制的绘制看到新内容；之前录制的绘制已在 settle 中提交并完成 | 同上（已核对） |
| 7 | x64 宿主 | 保留内容 `e->content`（给 `ReadContent` 回填 x86 副本用） | Commit 后从事务 `PatchTextureLayout`（CPU 复制 ⑥，retainContent 开启时） | 资源销毁时释放 | `d3d9_host_main.cpp` CommitTextureUpload（已核对） |

另外：游戏在 backing 里生成像素不计入上述 6 次（更正：原文把它记为 ⓪）；`UploadBuffer` 走另一条路径（动态缓冲直接写主机可见内存），不在本文范围。

## 3. 每次更新的同步与等待（P0 计数，已核对）

- x86：`BeginTextureUpload`、每 64 KiB 一个 `TextureUploadChunk`、`CommitTextureUpload` 都是同步调用；16×16 更新 = 3 次往返。
- 宿主 Commit：`settle()`（`flushRecordedGroup` + `flushStream` 提交当前世界流列表 → `checkFlightSubmissions` → `vkWaitSemaphores` 到最后序号 → `waitRetired`），
  然后 `begin()`（又一次 settle，此时空转），录拷贝，`submit(true)`（等提交线程 → 再 settle 等 GPU）。
- P0 计数：宿主提交按调用点，`TextureUploadBefore`（被上传切断的流列表提交）与 `TextureUploadAfter`（拷贝本身）各约每次更新 1 次；
  Lost Temple 每世界帧约 67 次宿主提交，其中约 64 次来自这两处。

## 4. 每段真正需要等什么（推断，供 P1 设计参考，不是方案）

| 段 | 现在等到 | 最少需要 | 依据 |
|---|---|---|---|
| x86 块缓冲 | 宿主执行完 Commit（同步调用） | 环发布完成 | 环里已有副本（已核对） |
| 环记录 | 宿主复制进 Packet 并发布 `head`（执行前） | 同左 | 现状已是最少；若让 GPU 直接读环里的负载，`head` 会允许生产者过早覆盖，需另设 GPU 租约（更正：原写“宿主 applied”） |
| 宿主 `m_bytes` | Commit 结束 | 拷进 staging 之后 | 之后没人读它（已核对），只是多一次分配 + 复制 |
| `owner.staging` | 本次拷贝提交完成（submit(true)） | 同左；若改成分段/环形暂存，则只需等“使用该段的那次提交”的时间线值 | 推断 |
| 上传前的 `settle()` | 当前世界流列表提交**并在 GPU 上完成** | 本次拷贝排在先前录制的采样之后（同一队列的提交顺序 + 屏障） | 推断：是否必须等 GPU 完成，取决于 DXVK 跨命令列表的图像布局/屏障跟踪，未核对 |
| 读回 / `ReadContent` | 主机保留副本（不碰 GPU） | 同左 | 已核对 |

## 5. 不在本文回答的

- 这批 16×16 纹理是游戏的哪个功能：P0 第二批二进制记录了创建调用点（`CreateTexture` 的返回地址，模块+RVA），见 P0 回执；本文不命名。
- 改成异步 Commit 时的失败语义（fail-closed）、句柄与 revision 规则、暂存分段的内存上限：归 Fable 设计。
