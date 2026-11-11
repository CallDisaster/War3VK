# X1a：受限D3D9开发代理的有界异步Clear

状态：2026-10-23，本文保留 X1a Sleep 版本的 CPU/IPC 及 GPU 四例通过、96轮COM超时历史。
后续 X1b 事件唤醒候选已完成新 CPU/IPC 和两轮六例 GPU 子集，见
[事件唤醒实测与新回执](2026-10-23-x64-notification-waits.md)。旧失败不追认，本文旧源码身份不代表当前源。
不是游戏接入、产品异步owner完成或内存收益证明。开发代理继续拒绝`Game.dll`。

## 本次边界

沿用原`Connection → 四槽SPSC → 单worker → authenticated pipe → x64 DXVK owner`，
只有合法Clear从同步回包改为本地有界入队。Draw、资源操作、Readback、Query、Present、Finish
仍为保序同步屏障。没有新wire、溢出队列、GPU退休规则或第二套渲染器。

- InitReady仅由实际Initialize精确D3D_OK/空载荷回包建立，不由线程/管道存在或入队推断。
- Full反复重试同一不可变packet；只有Published提交序号。四槽外还存在worker的一份私有Batch，
  所以最多五项未结算工作，不把它错报为四项，也不靠加槽绕开背压。
- Clear本地成功只表示已入队，不等于远端接受、GPU完成或可显示。异常Clear回包在ACK/done前
  锁存故障；worker异常在日志和secondary drain前停止新准入。
- 单producer不能在同步屏障返回前发布下一项；读取共享reply前取得该屏障精确done，
  不能把此前Clear的done误当作Readback/Draw完成。冷计数仅在worker join后读取。
- 本地admission上限5秒；同步call只生成一次start+30秒预算，admission不重开reply计时。
  保留基线done-first语义：恢复调度时已完成的精确reply仍可先于时间检查被接受，
  这不是所有迟到reply均被拒绝的硬实时保证。transport和primary/secondary exit仍各5秒。
- 析构join和取消drain仍可能阻塞，必须由外层owned-process watchdog约束；尚不能链进游戏热路径。

## 源码与历史保全

本 X1a 历史 checkpoint 的开发文件：`tools/render_host/d3d9_connection.h` SHA
`50EF52006D619AF6B28323F3B3B7003484B39CB95AC3C7C192CCF669C8A94994`；
`d3d9_proxy.cpp` SHA `1B7F31740D02DCF92B7DFD2C00B7447759D9D791628DB449F4AA8ADEF4A1C157`。
旧两文件逐字保存在`AutoTest/artifacts/x1a-formal-preimage-20261023/`，不改旧VVL/build回执。
当前源变化后，旧完整build源码身份不再代表当前proxy，后续测试必须新编译该proxy。

## 实际CPU验证

独立审查后，由主线程串行编译x86/x64，`-O2 -Wall -Wextra -Werror`，
restricted-Medium非输入Win32 Desktop实际执行。不是只运行静态字符串测试。

首轮`x1a-connection-cpu-20261023-r1`在链接期失败：测试runner遗漏现有`protocol.cpp`的
ReadLe/WriteLe实现。未运行native；失败raw与旧runner源保留。只补真实编译/冻结输入，不做替身协议。

修订轮`AutoTest/artifacts/x1a-connection-cpu-20261023-r2/receipt.json`：
SHA `B68BB675B03276EBF4C11A6DE60C5B04E09B4320234CC10FB650B92349A66905`。

| 实际执行 | 断言 | Desktop见证 | 结果 |
|---|---:|---|---|
| x86 Connection/queue | 263 | owner + 三个真实worker | PASS/exit0 |
| x64 Connection/queue | 263 | owner + 三个真实worker | PASS/exit0 |

这两行是同一263项覆盖的双架构执行，不宣称526种不同情形。fixture实际调用`call(Readback)`/
`call(Draw)`并握手证明等待；回复为合成CPU数据，`ipcTested=false/gpuTested=false`。
包含Init拒绝与尚未回包、Full序号和私有副本、错误Clear状态/载荷、DEVICELOST、等待中故障不拷贝、
Draw INVALIDCALL后恢复以及deadline算术。deadline算术不是30秒实际等待测量。
原始stdout/stderr、构建日志和输入身份均保全；没有外层强杀/Job残留/输入桌面变化。
19项读方纯测与5项deadline源码检查分别通过，不能算作另外的GPU覆盖。

## 实际IPC故障门（CPU对端，不是GPU故障注入）

主线程独立执行`AutoTest/run_d3d9_connection_fault_ipc.py`，真实x86 Connection/worker/
OwnedChild/认证管道连接x64协议对端。对端在Initialize精确成功、Clear序号2已发送、
Readback序号3已实际进入等待后才制造故障，不直接调用客户端acceptReply替代传输。
32字节活栈缓冲（8字节前保护+16字节结果+8字节后保护）贯穿调用，不借用失效映射。

固定五例都在restricted-Medium非输入Desktop执行，均exit0、无强杀、Job与Desktop正常收口：

| 对端行为 | 实际拒绝原因 | Readback耗时 | 缓冲/后续准入 |
|---|---|---:|---|
| Clear返回S_FALSE | asynchronous/init reply contract | 31 ms | 32字节未变，后续Clear拒绝 |
| Clear成功但带4字节载荷 | asynchronous/init reply contract | 31 ms | 同上 |
| 错nonce | reply identity | 31 ms | 同上 |
| 对端正常退出，不回Clear | header/System/ERROR_BROKEN_PIPE=109 | 31 ms | 同上 |
| 只发1字节reply header | header/timeout/WAIT_TIMEOUT=258 | 5047 ms | 同上 |

最后一项是实际原5秒transport超时加调度开销，不是延长截止。五例都保留published=3、
CPU acknowledged/done=1、queued=1及pending CPU ACK，不伪造GPU退休或远端DEVICELOST。
异常收口的secondary exact-child drain为15–16 ms；它只证明故障后清场，
不追认未执行的primary正常退出（childExit仍STILL_ACTIVE，primary settled=false）。

严格读方30/30纯测包含错误分类反例、launcher创建时间绑定、精确字段/类型/顺序、
primary/worker同因与冷计数对拍；子门允许nonce一半为0，不允许整个nonce为空。

回执：`AutoTest/artifacts/x1a-fault-ipc-20261023-r1/receipt.json`，
SHA `BE70F2DD89782702C8EFA7E6A0D98BEC4716C1BB7A5E7098A129E4FCC56C39FD`。
x86 fixture：851456 B / `D9507D00D3E4A655D07B97ADB2AA281E1237476132F5AE11682D5B0B2A543560`；
x64 CPU对端：790016 B / `A023DE36E7411BF16D7DBA50F6D7EC209865D6623F6345AB45ACBA7925BB14A1`。
49项源码、53项最终输入/二进制身份和初始raw均保全。对端无Vulkan，
`gpuTested=false/publicComTested=false/productOwnerComplete=false`，不能据此关闭GPU丢失或游戏退出门。

## GPU组合r1/r2：四项真实通过，不覆盖整组超时

`x1a-ipc-gpu-20261023-r1`完成三个x86构建和双架构Desktop探针，但runner把数值schema=1
的探针送入字符串schema读方，尚未执行GPU case就失败。修复仅增加独立严格探针读方；
旧脚本及失败回执保留，不回填通过。

`x1a-ipc-gpu-20261023-r2/receipt.json` SHA
`659B9DA435A1ACB7B4F79C476559E9995E0BDD941F4BE806B6411E1EA62806BB`：
同一VVL365已验host、实际新编译proxy，restricted-Medium非输入Desktop串行执行。

| case | 结果 | exchanges | GPU submitted/completed/retired |
|---|---|---:|---|
| cap5 | PASS，4槽+1私有批、2次Full同序号、恢复后像素正确 | 38 | 21/21/21 |
| draw | PASS，真实Draw完整回复暂停期间同步屏障不返回 | 31 | 14/14/14 |
| normal | PASS，原像素与正常退出 | 31 | 14/14/14 |
| invalid | PASS，原非法输入拒绝后恢复 | 37 | 15/15/15 |
| com | FAIL，原45秒外层watchdog终止exact-owned Job | 未完成 | 无完成摘要 |
| raw | 未执行 | — | — |

前四例VVL warning/error均0；183项冻结输入和9份raw清单独立复验一致。
COM在7906/15891/24016/32063/39531ms完成16/32/48/64/80轮，表现为持续推进但未在期限内完成，
不称死锁，也未证明由Sleep、VVL、GPU或IPC单独造成。cap5的31ms是整段背压，不是单次Sleep测量。
COM清理后Job空、Desktop关闭、input仍Default；清场成功不追认case通过。
没有延长45秒/5秒/30秒、减少96轮、改变预算/准入或删除draw。

## 待验与下一步

1. 上述五项在途容量及Draw屏障已在r2通过；新增窄等待成本测量，定位COM超时，不用CPU测量代替GPU门。
2. 同身份重跑完整六例，保留原96轮COM与raw反例、VVL和全部期限；旧r2失败不追认。
3. 上述IPC故障子门已通过；GPU DEVICELOST、生产异步owner/非阻塞取消和销毁仍独立待验。
4. 无仪表的调用延迟、分配/拷贝、私有内存与尾延迟另测；当前没有节省MiB/FPS结论。
5. host-owned child固定尺寸/DPI/父先退出是下一独立窗口边界；完整D3D9、WarVK pass、
   真实游戏画面与x86 VA/commit A/B仍未完成。

前置VVL两例证据见[初始化与surface所有权](2026-10-22-x64-initialization-and-surface-owner.md)。
玩家DLL、系统、游戏与发布均未动，旧失败不追认。
