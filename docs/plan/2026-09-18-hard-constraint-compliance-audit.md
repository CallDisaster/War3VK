# 硬约束合规审计：**机械核对**本计划是否碰过「准入 / 几何 / 优先级」— 2026-09-18（round 71）

> 裁定硬约束：「**不得借本计划改变 palette 准入、几何选择或渲染优先级**，
> 不得以关闭动态 caster 或减少正常阴影作为修复成功。」
>
> 本文用 **A 阶段的原始基线包**（`E:\Work\WarVK-delivery-20260918-doc-r1`，裁定要求其不可变）
> 作为对照，做**机械 diff**（`git diff --no-index`，**只读**，非 git 写操作）。

## 1. 产品源码：改动**只有 9 个文件**，零新增/删除

```
d3d9_device.cpp                              19    4     ← 渲染路径（见 §2）
war3_shadow_renderer_core.cpp                 4    1     ← 渲染路径（见 §2）
war3_palette_object_evidence.h              219   72     ← 证据记录器
war3_palette_object_evidence_sink.cpp        20   10     ← 证据发射
war3_palette_object_capture.h                12    1     ← 证据帧域工厂
war3_frame_evidence.cpp                      14    8     ← 会话/发布协议
war3_palette_object_evidence_test.cpp       597    0     ← 宿主机测试
war3_palette_object_wire_roundtrip_test.cpp  24    0     ← 往返测试载体
war3_palette_object_evidence_cost_test.cpp    9    2     ← 成本测试夹具

only-in-one-side:（空）  ⇒ 没有新增或删除任何产品文件
```

## 2. ★ 两个**渲染路径**文件的逐 hunk 核对

### `war3_shadow_renderer_core.cpp`：**唯一** hunk

```
@@ -6827 +6827,4 @@  在 TryBuildRuntimeGroupPalette 内
-                  recheckEvidence.currentPaletteFrameTag != 0u);
+                  recheckEvidence.currentPaletteFrameTag != 0u,
+                  // K3：拒绝点用**当场清单记录**自己的序号 —— 与 Served 点同族
+                  // （packet.renderable.frameSerial），故同一次尝试在两点得到同一个号。
+                  renderable.frameSerial);
```

⇒ **只是给证据采集调用追加一个实参**（K3 尝试号）。**没有**触及准入/几何/优先级。

### `d3d9_device.cpp`：6 个 hunk，全部在证据区

```
@@ -20864,0 +20865,7 @@   E：声明 paletteObjectSelectionFromLiveNative（携带值）
@@ -20869 +20876,4 @@   E：新鲜度分支改为带花括号 + 置假（**条件本身未改**）
@@ -20870,0 +20881 @@   E：补右花括号
@@ -22138 +22149 @@   E：nativeKnown 改用携带值（**不再用 frameTag 反推**）
@@ -22143 +22154,3 @@   K3：Served 点传 attemptSerial
@@ -23520 +23533,3 @@   K3：FirstSight 点传 attemptSerial
```

**诚实说明一处需要说清的地方**：E 的改动**确实**动到了选择流程的**局部**代码 ——
我引入了一个局部布尔并把既有的

```cpp
  if (skinned && ... && !IsSkinPaletteSelectionCurrent(selectedPalette))
    selectedPalette = {};
```

改成带花括号的同一条件的块。**条件表达式与被赋的值都未改变**（hunk 显示的是
`selectedPalette = {};` 那一行被替换为"注释 + 置假 + 同一赋值 + 右花括号"）。
⇒ **准入判据本身未被改动**；但"改动了这一行的周边"是事实，我不淡化它。
（行为由全套门禁 + 新增静态锁共同约束，见 §4。）

## 3. AutoTest：6 改 + 5 增，全部可归因

| 文件 | + | − | 归因 |
| --- | --- | --- | --- |
| `test_palette_object_wire_roundtrip.py` | 42 | 16 | B 批次：check_scenario_g 移出注释 + REQUIRED_SCENARIOS |
| `analyze_palette_object_evidence.py` | 124 | 14 | 读方：链型/终态/版本门控 + 终态×链型矩阵 + K1 + 注释修正 |
| `analyze_frame_evidence.py` | 8 | 0 | **K1**：解析器排除终态（710/716 误拒） |
| `test_palette_object_evidence_analysis_static.py` | 210 | 6 | 上述读方规则的用例 |
| `test_semantic_build_thread_gate_static.py` | 33 | 9 | 11p/11q/11r(d)：v4 常量、分组键、sink data 写集 |
| `test_palette_object_capture_points_static.py` | 0 | 0 | **未变** |
| `test_palette_object_arm_order_static.py` | 新增 | | D2 |
| `test_palette_object_header_snapshot_static.py` | 新增 | | D3 |
| `test_palette_object_attempt_serial_caller_static.py` | 新增 | | K3 生产调用方 |
| `test_palette_object_native_reason_static.py` | 新增 | | E |
| `capture_startup_environment.ps1` | 新增 | | E 实机半的预备装置（**只读**） |

## 4. 结论（合规判定）

```
✅ 没有改动 palette 准入判据（唯一相关处是 E 的局部布尔，条件未变）
✅ 没有改动几何选择
✅ 没有改动渲染优先级
✅ 没有以"关闭动态 caster"或"减少正常阴影"作为修复（本计划完全没有触碰 caster 生成/阴影开关）
✅ 没有新增/删除产品文件
✅ 没有 git 写操作（本次审计用的是 git diff --no-index，只读）
```

## 5. 本次审计**不能**代替什么

- 它**不能**代替行为验证：`diff` 只证明"改了哪些行"，不证明"语义等价"；
  E 那处局部改动**确实**动了渲染文件里的代码，其行为保证来自**全套门禁 + 静态锁**，不是来自本审计；
- 它**不能**排除"我碰巧改对了但语义仍不同"（例如条件里的求值顺序）——
  那需要逐点语义复核，**未做**；
- `AutoTest/` 的全目录 diff **未能完成**（Windows 超长路径导致 `git diff --no-index` 在
  `AutoTest/artifacts/.../2026-02-21_hook_split_and_bridge_fastpath.md` 处报 `Could not access`）
  ⇒ §3 是**逐文件**核对（我列的是我知道被改过的文件）。**若存在我未列出的 AutoTest 改动，本审计不会发现它。**

## 6. 不声称

- **不**声称"零改动风险"（`diff` 只证明改动**位置**）；
- **不**声称 AutoTest 侧已穷尽（全目录 diff 因超长路径失败，见 §5）；
- **不**声称本审计等价于实机验证（**零实机观测**）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。