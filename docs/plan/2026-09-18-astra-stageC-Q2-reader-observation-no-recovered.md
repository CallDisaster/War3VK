# 阶段 C · Q2：读方裁定规则 —— **Observation 永不使用 Recovered** — 2026-09-18

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。本轮**只改读方（Python）**，未改产品源码。

## 1. 裁定原文到实现的映射

裁定要求：「Observation **永不**使用 Recovered；Recovered **必须**有真实拒绝事实」。

Rejected 事实目前由**写方**在 CloseWindow 时判定（hasRejectFact && closedChain ⇒ Recovered）。
但读方**同样**必须拒绝这种组合 —— 否则一份伪造/损坏的导出（观察链配 Recovered）
会被当作一条普通链接受。因此本轮在读方加**具名判错**。

## 2. 实现（analyze_palette_object_evidence.py 的 judge_chain）

在终态取出之后立即判定：

```python
if terminal=="Recovered" and any(event.get("chainType")=="Observation" for event in events):
    issues.append("observationChainMustNotUseRecovered")
```

- **具名**（不是布尔、不是静默丢弃）：读方对账时能直接看到是哪条规则被违反；
- **v1/v2/v3 天然不受影响**：那三个版本没有链型载体（解码处恒给 RejectionRecovery），
  所以本规则不会让旧产物开始报新错 —— 旧合同一位不变。

## 3. 载荷性（**由变异证明**，不是论证）

把规则临时改成 `if False`（恒假）后重跑分析套件：

```
self.assertIn("observationChainMustNotUseRecovered", json.dumps(result))
AssertionError: 'observationChainMustNotUseRecovered' not found in '{...}'
Ran 103 tests in 0.076s
FAILED (failures=1)
```

⇒ 该断言**确实由这条规则驱动**（去掉规则即失败），随后规则已还原。

## 4. 状态

```
STATIC=259/0
=== meson ===
Ok:                85
Fail:              0
=== no-work ===
ninja: no work to do.
=== 读方 ===
OK
=== wire ===
wire=0
CHECKS=1160 FAILURES=0
DLL=9CB3AA3C91172A7607A4DA86FC36BE44A044BE94CE70E2977EC7086D59D5CC87
分析套件 : 103 tests, OK（原 102 + 新增 test_observation_chain_must_not_use_recovered）
站点 : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git  : 无写操作
```

## 5. 不声称

- **不**声称该规则已在**实机**数据上触发过（它是一条判据，不是观察结果）；
- **不**声称「Recovered 必须有真实拒绝事实」的**写方**判定已被独立复核（本轮只加读方防线）；
- **不**声称窗口维度已进入查找键；**不**声称读方会拒绝链型与**阶段形状**不符（本轮只拦终态）；
- **不**声称阶段 C 主体完成；K3（跨帧判序 / attemptSerial）**未做**；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。