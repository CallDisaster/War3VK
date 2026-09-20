# 阶段 D · B1 运行期半边：**决断为方向 A**，并给出一次性执行方案 — 2026-09-18（round 55）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。**本轮无代码改动**（决策 + 定位）。

## 1. 为什么**不需要**裁定（纠正 round 52 的结论）

round 52 我写「方向 A 会让两类测试职责不清 ⇒ **需裁定**」。重新评估后**这是过度上报**：

- 裁定要的是「**三条屏障测试**」这个**结果**，没有规定它必须住在哪个二进制里；
- **方向 A 满足该结果**，且**不扩大构建面**（方向 B 要动 meson，把宿主测试从"纯 CPU 记录器测试"
  变成"链接生产 sink 的测试"—— 那是**更强的副作用**）；
- 我担心的"职责混淆"**可以消解**：只要该用例**显式命名**为会话生命周期屏障，
  并在文件里注明「它落在此二进制**仅为链接原因**」；
- ⇒ 在**同样满足裁定**的两个方向里选**副作用更小**的那个，是工程判断，不是裁定事项。

**（这本身是一次自我纠正：round 52 我把"我个人的洁癖"包装成了"需裁定"。）**

## 2. 执行方案（下一轮一次完成，不再需要额外侦察）

**文件**：`src/d3d9/war3/render/tests/war3_palette_object_wire_roundtrip_test.cpp`

**位置**：`main()` 内、ROUNDTRIP 汇总打印**之前**（`int main()` 在 `:629`；
汇总打印在 `:652-654`；失败计数 `g_failures` 在 `:42`，**同 TU 已有 sink 调用**）。

**插入块**（自成一个作用域，自带计数与打印，不干扰既有 checks）：

```cpp
// 2026-09-18 阶段 D（批次 2，B1 运行期半边）：armed 的发布是单一同步域。
// 注意：本用例属于**会话生命周期屏障**，不是编码往返组用例；
// 它落在这个二进制里**仅为链接原因**（宿主测试目标不链接 sink TU）。
{
  unsigned barrierFailures = 0u;
  DisarmPaletteObjectEvidence();
  if (PaletteObjectEvidenceArmed()) { ++barrierFailures; std::cout << "FAILURE: B1 disarmed must report armed=false\n"; }
  ArmPaletteObjectEvidence(kRecordFrameSerialBase, 0u);
  const bool armed = PaletteObjectEvidenceArmed();
  PaletteObjectEvidenceHeader header{};
  QueryPaletteObjectEvidenceHeader(header);
  std::cout << "BARRIER armed=" << (armed ? 1 : 0)
            << " watchCount=" << header.watchCount << "\n";
  if (armed && header.watchCount > 1024u) { ++barrierFailures; std::cout << "FAILURE: B1 armed header must come from an initialised recorder\n"; }
  if (armed && header.counters.terminalEmitted > header.counters.emitted) { ++barrierFailures; std::cout << "FAILURE: B1 header snapshot must be self-consistent\n"; }
  DisarmPaletteObjectEvidence();
  if (PaletteObjectEvidenceArmed()) { ++barrierFailures; std::cout << "FAILURE: B1 disarm must publish armed=false\n"; }
  std::cout << "BARRIER checks=5 failures=" << barrierFailures
            << (barrierFailures == 0u ? " PASS\n" : " FAIL\n");
  g_failures += barrierFailures;
}
```

**关键**：`armed` 的真假**必须打印** ⇒ 子门关闭导致"真空通过"是**可见的**，不是静默通过。

**执行顺序**：①用 `read` 确认 `main()` 内汇总打印的确切文本与 sink 调用的限定形式；
②插入上述块；③`ninja`；④运行取 `BARRIER` 行；⑤运行整套门禁（261 静态 / 85 meson / 1160 wire /
31 evidence）；⑥反向变异：把 `armed` 读写退回裸 `bool` ⇒ **静态锁**必须失败（round 47 已证），
本用例的载荷性则来自「disarm 后 armed 必须为假」这条**无条件**断言。

## 3. 状态（本轮无代码改动）

```
STATIC=261/0 ; meson 85/0 ; no-work ; evidence 31/0 ; wire CHECKS=1160 FAILURES=0
DLL : 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951（未变）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
B 阶段包 : E:\Work\WarVK-delivery-20260918-stageB-gate.zip  EB0B8059…（round 54）
git  : 无写操作
```

## 4. 更新的待裁定清单（少了一条）

```
① K3「一次尝试」由谁递增 —— 决定 C 能否闭合
② （**已消解**：B1 落点决断为方向 A，见 §1）
③ 窗口维度是否需要「保留条目地重新 arm」的新能力
④ 归属规则在其余阶段形状上是否也要显式定义
```

## 5. 不声称

- **不**声称 B1 运行期半边已完成（**本轮只是决策与定位**，代码未动）；
- **不**声称方向 A 已在运行中验证过；
- **不**声称 C 或 D 阶段完成；**不**声称两个 P0 完成 ⇒
  **不新增实机因果结论、不晋升稳定候选**；
- 全局边界依旧：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。