# 合规审计**更正**：上一轮漏列了 3 个 AutoTest 文件（本轮闭合）— 2026-09-18（round 72）

> 上一轮（round 71）我在 `2026-09-18-hard-constraint-compliance-audit.md` §5 诚实标注：
> 「**若存在我未列出的 AutoTest 改动，本审计不会发现它**」（因为 `git diff --no-index` 在 AutoTest 上
> 因 Windows 超长路径失败，只能逐文件核对）。
>
> **本轮用 Node 自写遍历（无路径长度限制、只读）做完整比对，那个缺口被抓住了。**

## 1. 完整比对结果（基线 AutoTest vs 当前树）

```
基线 AutoTest 文件数 = 566 ; 树 = 10948
  仅基线有（删除）      = 0        ← 没有任何文件被删除
  仅树有                = 10382    ← 几乎全是 artifacts/ 运行产物（非源码改动）
  两边都有但内容不同    = 8
```

**内容不同的 8 个文件**：

| # | 文件 | 上一轮是否列出 |
| --- | --- | --- |
| 1 | analyze_frame_evidence.py | ✅ 已列（K1 解析器排除终态） |
| 2 | analyze_palette_object_evidence.py | ✅ 已列 |
| 3 | test_palette_object_evidence_analysis_static.py | ✅ 已列 |
| 4 | test_palette_object_wire_roundtrip.py | ✅ 已列 |
| 5 | test_semantic_build_thread_gate_static.py | ✅ 已列 |
| 6 | **test_analyze_frame_evidence.py** | ❌ **上一轮漏列** |
| 7 | **test_first_sight_observation_chain_static.py** | ❌ **上一轮漏列** |
| 8 | **test_independent_review_sep18_fixes_static.py** | ❌ **上一轮漏列** |

## 2. 三个漏列文件的改动内容（**逐个查明**，无一放宽判据）

### (6) `test_analyze_frame_evidence.py`  **+5 / −1**

```python
-        for version in (0,3,7,99,-1,"1",2.0,True,None):
+        # 2026-09-18 阶段 C 更正：这里原先把 **3** 列为"未知版本"，但 3 早已登记
+        # （v3 = 分段 + 正常观察链）⇒ 本用例一直在**假失败**（它不在 259 个
+        # `test_*_static.py` 门禁内，所以没被发现）。现在 3 与 4 都是已登记版本，
+        # 本用例改用**真正未登记**的 5。**要求未变**：未登记版本必须整份拒绝。
+        for version in (0,5,7,99,-1,"1",2.0,True,None):
```

⇒ **更正一个假失败**（3 早已登记）；**要求未变**（未登记版本仍必须整份拒绝）。
⇒ 该文件**不在** `test_*_static.py` 通配内 —— 与本项目既有记录一致（它是手工补跑项）。

### (7) `test_first_sight_observation_chain_static.py`  **+12 / −5**

```python
-# ---- 3. 写方：只有真的发过首见才抬版本 ----
-assert 'result["version"]=PaletteObjectRecorder().firstSightUsed() ? 3 : 2;' in FRAME_EVIDENCE, (
-    "块版本必须由 firstSightUsed() 决定（未用首见链的导出仍是 2）")
+# ---- 3. 写方：**恒定 v4**（2026-09-18 阶段 C）----
+# 原断言要求「版本由 firstSightUsed() 决定」，那是**动态版本**：同一个二进制产生两种块形状，
+# 而版本本应是**契约**而不是内容摘要。现改为断言恒定 v4，并**显式禁止**动态选择回归。
+assert 'result["version"]=4;' in FRAME_EVIDENCE, (
+    "阶段 C：块版本必须**恒定 v4**（含未使用观察链的导出）")
+assert 'firstSightUsed() ? 3 : 2' not in FRAME_EVIDENCE, (
+    "阶段 C：按 firstSightUsed() 动态选择 2/3 必须已删除（版本是契约，不是内容摘要）")
```

⇒ 这正是裁定「**新写方一律输出 paletteObject version=4（删除 firstSightUsed() 动态选版本）**」的落地，
  且**是加强**：新增了一条**负向断言**（禁止动态选择回归）。

### (8) `test_independent_review_sep18_fixes_static.py`  **+5 / −2**

```python
-assert "0u, false);" in _tail, "生产采集点必须传 nativeKnown=false"
-assert "0u, true);" not in _tail, "生产采集点不得再传 nativeKnown=true"
+assert "0u, false," in _tail, "生产采集点必须传 nativeKnown=false"
+# 2026-09-18 阶段 C（K3）：该站点还必须**按值携带 attemptSerial**（否则实机判序仍是终身单调）。
+assert "uint64_t(manifestFrame));" in _tail, \
+    "首见采集点必须按值携带 attemptSerial（K3）：用当地 manifest 记录帧作尝试号"
+assert "0u, true" not in _tail, "生产采集点不得再传 nativeKnown=true（无论其后还跟几个实参）"
```

⇒ round 57 已记录的**加强**（旧字面量绑定了旧实参数；改用更宽的锚 + 新增 attemptSerial 断言）。
  **没有删除任何断言、没有放宽任何期望**。

## 3. 更正后的合规判定（与上一轮结论一致，但依据完整）

```
✅ 需要归因的 AutoTest 改动 = 8 个（上一轮只列了 5 个，本轮补全）
✅ 三个补列项全部是「加强 / 更正假失败」，无一处放宽判据
✅ 无删除文件；10382 个"仅树有"几乎全是 artifacts/ 运行产物
✅ 产品侧结论不变（9 个文件、两个渲染路径 hunk 已逐个核对）
```

## 4. 本轮的方法论收获

上一轮我把 `git diff --no-index` 的**工具失败**当成"只是没做全"，并写进了"不声称"里。
这一轮用**自写遍历**证明：**那个缺口里确实藏着 3 个我没想到的文件**。

⇒ **教训**：当审计工具在某个范围内失败时，"诚实标注缺口"是必要的，但**不等于**已尽到审计责任；
  应当**换一种工具把它做完** —— 本轮就是这样才发现漏列的。

## 5. 不声称

- **不**声称"仅树有 10382"里没有我该负责的文件：它们是 `artifacts/` 下的运行产物（日志、截图、exe、json），
  我**没有**逐个查看 —— 只能说它们**不在源码路径**上；
- **不**声称基线 AutoTest 566 文件是"全部历史文件"（基线包是 A 阶段的交付包，其 AutoTest 可能本就只含一部分）；
  因此"仅树有 10382"**不能**被读成"我新增了 10382 个文件"；
- **不**声称本审计证明了语义等价（`diff` 只证明改了哪些行）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。