# 截图同步 I/O 取消与退出合同

状态：2026-09-22 实施前冻结的维护候选合同。不是构建、慢磁盘或游戏退出验收。

## 一手依据

- Microsoft `CancelSynchronousIo`：调用需要线程句柄的 `THREAD_TERMINATE` 权限；成功只代表请求了取消，
  不等待完成；最终操作可能正常完成、以 `ERROR_OPERATION_ABORTED` 完成，或其他失败。
  `ERROR_NOT_FOUND` 只表示本次没有找到待取消请求。
  <https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio>
- Microsoft `WriteFile`：同步调用返回前缓冲区仍由 I/O 使用；异步文件句柄也可能同步阻塞。
  <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile>
- Microsoft 取消说明：底层驱动不保证支持取消；跨一系列同步调用取消时必须处理调用间竞态。
  `MoveFileEx` 不支持这些取消函数。
  <https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations>
- Microsoft `DuplicateHandle`：可以把当前线程 pseudo handle 复制为真实句柄，并显式指定访问权。
  本次只申请 `SYNCHRONIZE | THREAD_TERMINATE`，不把 `std::thread::native_handle()` 当 HANDLE。
  <https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle>

## 本次责任边界

截图保留同步逐行 TGA 写入、已有 GPU 完成证明及原有 `std::thread` 创建方式。工作线程入口
以 DuplicateHandle 复制当前线程真实句柄，原子发布句柄/失败状态；作用域结束发布退出标志。
退出设置 stop，唤醒 worker，等待句柄发布或退出，循环尝试取消其文件阶段内的同步 I/O，
最后 join 确认实际线程结束，再关闭复制句柄；取消请求绝不是释放许可。
句柄尚未发布时使用 25 ms 条件等待；复制失败可见并退回原有 join，不提前释放。

文件阶段的开始、结束和取消调用由同一小型 gate 同步。工作线程不持 gate 做文件 I/O；
Logger 与证据通知在阶段之外，避免取消误伤公共日志。停止检查与进入系统调用之间仍可能有竞态，
所以 `ERROR_NOT_FOUND` 后继续等待并重试，而不提前释放或假报完成。

Impl、slot、设备/缓冲引用、TGA 行缓冲和文件句柄在同步操作完成并 join 前保活。
保留 CREATE_NEW、.part 和无覆盖 rename；只有真实发布成功才报告 saved。
如果 stop 与已经开始的成功 rename 竞争，正常发布可以赢得该竞态。
join 后尚未终结的 HistoryReadback 明确失败完成；不把 GPU 完成未知的 Submitted 槽变为 Retired。
进程终止时现有 loader-lock 特例保持不 join。

## 未解决边界与验收分层

- 不承诺硬退出时限。驱动拒绝取消、不可取消的 rename、文件系统元数据操作或其他线程停滞仍可能拖住退出。
- 不使用 detach、TerminateThread 或期限到达后销毁在用对象。
- 共用生产控制代码的替身测试覆盖取消未命中、延迟完成、取消/正常完成竞态和资源保活。
- 真实 Win32 CPU 测试以命名管道制造同步 WriteFile 阻塞并取消，并校验完整 TGA 文件；
  它不是实际慢磁盘、GPU 或游戏退出证明。Meson timeout 只保护独立测试进程。
- 正常盘/慢盘、首次/连续截图、录制开关、尺寸变化、真实游戏退出仍是独立后续验收。

截至本文初次写入尚未构建、运行新测试、部署或启动游戏。

## 源码候选与统一验证入口

`war3_async_screenshot_io.h` 是实际生产和 CPU 测试共用的文件阶段 gate、取消/完成/join
控制、句柄发布与 join 后历史任务失败发布入口。`war3_async_screenshot.cpp` 保留原来的文件写入和 GPU
准入，使用原有 `std::thread`，仅复制其当前线程 HANDLE；每 25 ms 观察完成，仅在文件阶段请求取消。
句柄复制失败或轮询失败会回到原有 join，不把观察失败当作完成。

Meson `screenshot_io_production` 包含：取消前未进入系统调用的 ERROR_NOT_FOUND、取消接受后
延迟完成、取消拒绝后延迟完成、正常完成赢得取消竞态、观察失败仍 join、Logger 阶段互斥，
三轮真实同步阻塞管道取消、实际文件 TGA 内容及 join 后历史状态发布。另保留
`native_async_screenshot_core`、`screenshot_cold_production` 和
`AutoTest/test_native_async_screenshot.py` 回归入口。
句柄控件的共用生产测试另覆盖停止早于启动发布、未发布时有限间隔等待、复制失败退回 join、
join 抛出不关闭句柄，以及正常 join 之后才关闭句柄。

这些 CPU 测试运行共用控制头和 Win32 API，未实例化真实 `AsyncScreenshot::Impl`：
CreateNew/.part/rename、真实 Logger、Vulkan 映射资源与 FrameHistory 并发结算仍只有源码边界审查，
不能把正向普通文件 TGA 测试写成完整生产截图链或真实慢磁盘退出验收。

实现与测试源文件已加入工作树，C++ 构建/运行由主线程统一进行，尚未登记 C++ 通过结论。
本分支只执行了 `py AutoTest/test_native_async_screenshot.py`：18/18 静态/源码策略检查通过，
以及所改已跟踪文件的 `git diff --check` 通过；不能据此宣称管道取消或真实退出已验证。
未给 Present 增加 I/O、等待或新的每帧分配；文件阶段门只在实际保存/退出路径使用。
没有新增截图大小或队列额度，没有改变 GPU fence、Retired 释放与 loader-lock 特例。

## 2026-09-22 主线程统一离线收口

以上“尚未运行”是本文件初次写入时的历史状态。本轮固定依赖干净镜像完成
`screenshot_io_production` **122 检查/0 失败**，含三轮实际阻塞同步管道的取消；
原截图 core/cold 回归和统一 Meson **101/101**、静态脚本 **275/275** 通过。
独立只读复核确认主树/冻结镜像五个相关产品文件一致，未发现本次新增发布竞争或取消即早释放。
两个历史 TEMP 快照的独立重抽子门仍缺，不隐藏在静态通过数内。

BelowNormal/-j2 exact DLL 构建 **466/466**、no-work、PE32/i386 通过；未剥离候选
36,156,663 B / `8238712B18619647801223F7EEFA639972CC640081B34EDC6138A9522727988D`。
回执位置与输入身份见开发日志。本轮没有部署/启动游戏，CPU 管道测试仍不构成真实慢盘、
完整 `Impl::save` 或 GPU/history 组合退出验证；不承诺硬超时，不声称原玩家崩溃已因此解决。
