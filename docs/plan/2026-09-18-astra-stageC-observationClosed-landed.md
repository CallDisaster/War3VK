# 阶段 C：`ObservationClosed=7` 端到端落地 — 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码 + 读方并重建 DLL**。
> 按 round 8 的清单一次做齐 ①②；③（负向探针）**本轮未完成**，如实记档。

## 1. ① 终态分支顺序（Case 24 的约束已被吸收）

`CloseWindow` 的判定链现为：

```cpp
hasRejectFact && closedChain          -> Recovered        // K2：必须真实拒绝事实
hitCount == 0u && !sawServed          -> WindowExpired    // **先于** Observation 分支
chainType == Observation              -> ObservationClosed
否则                                   -> Unclosed
```

**顺序不可交换**：round 8 曾把 `ObservationClosed` 排在 `WindowExpired` 之前，
被 `Case 24` 以 `a never-rejected object closes as WindowExpired, not Recovered` 拦下。
`ObservationClosed` 只接管**过去会落到 `Unclosed`** 的那一档。

## 2. ② 计数器贯通（四级）

| 级 | 改动 |
| --- | --- |
| C++ 记录器结构 | `uint64_t closedObservationClosed = 0u;`（独立桶，**不并入** `closedUnclosed`） |
| C++ 结算桶 | `else if (terminal == ObservationClosed) m_counters.closedObservationClosed++;` |
| 头块 JSON | `counters["closedObservationClosed"]=…` |
| 读方 v4 counters 集 | `OBSERVATION_CLOSED_FIELDS`，**只在 `version>=PALETTE_OBJECT_CHAIN_TYPED_VERSION`** 并入 `expected_counters` |

**记录器枚举**：`PaletteObjectTerminal` 新增 `ObservationClosed = 7u`（0..6 一位不动）。
v1/v2/v3 读方的终态表**不含 7**（`TERMINALS_BY_VERSION`，round 6 已建）⇒ 遇到即整份拒绝。

## 3. 我主动回退的一处**过度扩展**（重要）

我起初还把 `closedObservationClosed` 加进读方的 `CLOSED_FIELDS` 与
`TERMINAL_CLOSED_FIELD`（想顺带做桶一致性检查）。结果 **84 errors**：

```
ERROR: test_cli_refuses_the_new_counterexamples … KeyError: 'closedObservationClosed'
```

**根因**：`CLOSED_FIELDS` 被**无版本区分地**用于一致性取值，而 **v1/v2/v3 的 counters
合法地不含该键** ⇒ 旧版本产物在那里取键即失败。

**处置**：**回退**这两处，并在源码里写明为什么不并入（`closedObservationClosed` 的登记
由 `OBSERVATION_CLOSED_FIELDS` 在**版本维**上完成）。⇒ 读方套件恢复 `100 OK / 55 OK`。

**教训**：一个"顺带增强"如果把**版本无关**的集合当成版本相关来用，就会把旧版本全部打挂。
版本相关的登记必须**只走版本维**。

## 4. ③ 负向探针：**本轮未完成**（明确列出，不计为已完成）

尚未加入测试的三条：

1. **v1/v2/v3 遇终态 7 必须整份拒绝** —— `TERMINALS_BY_VERSION` 已具备该能力，
   但**没有用例**证明它是活的（不能被"读到 7 就一律拒绝"或"一律接受"的实现冒充）；
2. **桶标签与终态条数一致** —— `closedObservationClosed` 必须等于导出中
   `terminal==ObservationClosed` 的条数（现在没有任何断言把它与事件对上）；
3. **Observation 链永不 `Recovered`** —— `Case 26` 覆盖的是"无拒绝事实"这一**必要**条件；
   还应有一条"观察链走完 S/E/D 也不 `Recovered`"的**直接**见证（现在它落 `Unclosed`）。

**⇒ 本轮不得声称 `ObservationClosed` 的契约已被门禁锁住。**

## 5. 状态

```
ninja -C build32 -n : no work to do
AutoTest 全量静态    : 259 scripts, 0 failed
meson               : Ok: 85  Fail: 0
evidence test       : SUMMARY: 26 passed, 0 failed
wire roundtrip      : CHECKS=1160 FAILURES=0   （由 1153 增加）
读方套件            : analysis static 100 OK ; analyze_frame_evidence 55 OK
候选 DLL            : 36,297,544 B
                      SHA-256 5931974B2C95E70FC320F627061EE72AE8F6A3001805D4E45D9DC9B27FE11A67
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 6. 不声称

- **不**声称 `ObservationClosed` 的契约已被门禁锁住（见 §4 的三条探针**未做**）；
- **不**声称终态 7 已在**实机**出现 —— 生产入口是否会产生观察链并走到该分支，**尚无实机证据**，
  且按裁定在 D 批次完成前**不新增实机因果结论**；
- **不**声称链型已实现：`chainType` 仍**未参与查找**、**未上 wire**（v4 目前 = v3 字段集
  + 恒定版本号 + 1 个计数器）；
- **不**声称 K3（混合链 `R→FirstSight`、跨帧判序）已修；
- **不**声称阶段 C 主体完成。全局边界依旧：「一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复」—— `ObservationClosed` 同样**只**表示
  「观察已结算」。
