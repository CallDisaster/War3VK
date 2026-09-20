# P0-4 观察链结算 = ObservationClosed — 2026-09-18（未提交 / 未部署 / 未晋升稳定）

## 1. 缺陷（Astra 实测 + 裁定）

原结算顺序：
```
if (closedChain)                              terminal = Recovered;
else if (hitCount == 0 && !sawServed)         terminal = WindowExpired;   ← 先命中
else if (chainType == Observation)            terminal = ObservationClosed;
```
⇒ 只走到链首 / 只到入队的**观察链**被判成「窗口过期」—— 那是在**借用拒绝链的含义**（该对象从未被拒绝）。
而生产 `d3d9_device.cpp:23539-23548` **故意不发 Served**（无选中的 palette 不得凭空造事实），
因此真实序列 `FirstSight → Enqueued → Close` 此前会被误判。

我当初把 `WindowExpired` 排在前面，是为了保住旧 **Case 24** 的断言 —— 这等于**让旧错误决定新语义**
（Astra 明确指出这是错的）。

## 2. 裁定与修复

裁定：「仅有链首后关闭，也应使用 ObservationClosed，同时诚实报告没有完整绘制证据。」

```
if (closedChain)                              terminal = Recovered;
else if (chainType == Observation)            terminal = ObservationClosed;   ← 观察链优先
else if (hitCount == 0 && !sawServed)         terminal = WindowExpired;      ← 收窄为只对拒绝恢复链
```

`WindowExpired` 现在的含义是明确的：**真的发生过拒绝、窗口内从未被接住、也没有服务事实**。

## 3. 有意更新的旧期望（**修契约，不是放宽判据**）

`Case 24` 的断言 `a never-rejected object closes as WindowExpired, not Recovered` **已按裁定更新**为
要求 `ObservationClosed`。新期望**更强**：它要求终态只表达「观察结算」，
既不得冒充 `Recovered`，也不得冒充拒绝链的 `WindowExpired`。断言消息里写明了更新理由与日期。

## 4. 新用例与探针

新增 **Case 40**（三条，含对照）：
```
[P0-40] headOnly=7 enqueuedOnly=7 rejectNoCatch=2   (ObservationClosed=7 WindowExpired=2)
① 仅链首的观察链 ⇒ ObservationClosed
② 生产真实序列 FirstSight→Enqueued→Close（该路径故意不发 Served）⇒ ObservationClosed
③ 对照：**拒绝链**从未被接住 ⇒ 仍必须是 WindowExpired（防止重排把两种含义混起来）
```
**探针**（退回旧顺序：给观察链分支加 `&& e.sawServed`）⇒
`headOnly=2 enqueuedOnly=2`（= WindowExpired）且 Case 40 的 ①② **具名失败**，③ 仍通过。已还原（逐字节相等）。

## 5. 验证（强制重编 + 检查退出码）
```
ninja -C build32 -j4    exit 0        ninja -C build32 -n   no work to do
AutoTest 静态全量         263 / 0       ninja -C build32 test  Ok 85 / Fail 0
宿主机测试               39 passed, 0 failed（Case 1..40）
wire 原生                ROUNDTRIP 126/0 PASS
evidence.h md5          C7707C95C3B9441BB74703723FA552F9
```

## 6. 不声称
```
· 不声称实机已验证（本轮零实机采集）
· Case 35 的双链输出**未变**（拒绝链仍 WindowExpired）—— 那是 P0-6 的路由问题，本修复不涉及
· 不声称读方已针对新语义重新核对（已列入 P0-4 的独立验证子线程任务）
· 不声称重入护栏（P0-3 的 R4 修正）已有行为用例 —— Case 40 是 P0-4 的用例，二者不同
```
