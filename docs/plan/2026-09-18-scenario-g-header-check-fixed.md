# ✅ 场景 G 的 header 检查失败**已查明并修复**：是测试未同步，不是产品缺陷 — 2026-09-18

## 1. 定案：不是产品缺陷

把完整输出落盘后（`py ... $exe *> E:\Work\g_fail.txt`），失败原因只有一行：

```
FAIL: G: the production export must declare extension version 2 (got 3)
```

⇒ **`check_header_block` 硬编码"生产导出必须是版本 2"**，而 G **正确地**声明了版本 3。

该函数写于 **2026-09-17 —— v3 出现之前**，其 docstring 至今仍写着：

> "生产写入端现在**确实**写分段与身份证明种类，因此真实导出的扩展版本必须是登记在案的**版本 2**（= 现行生产形状）"

批次 3 引入正常观察链（`FirstSight = 5`）后，生产导出**在发出首见时必然是版本 3**。
⇒ **这属于审计第 7 项"测试同步"的具体一例：测试没跟上版本，不是产品缺陷。**

（这也是上一轮"两种可能"里的 **(b)**：`check_header_block` 未对 v3 参数化。）

## 2. 修复

```python
def check_header_block(name, data, expected_version=2):   # 新增参数，默认 2
    ...
    check(set(block) == set(frame_evidence.PALETTE_OBJECT_BLOCK_FIELDS[expected_version]), ...)
    check(block.get("version") == expected_version, ...)
```

| 调用点 | 参数 | 理由 |
| --- | --- | --- |
| A–F（`check_header_block(name, data)`） | 默认 **2** | 它们确实是版本 2 的形状，**行为一字未变** |
| G（`check_header_block("G", data, expected_version=3)`） | **3** | G 是首见链 ⇒ 必然版本 3 |

⇒ 参数化**没有放宽**任何判据：A–F 仍必须正好是版本 2、字段集精确相等；G 必须正好是版本 3。

## 3. 结果：G 的 header 块现在**真的被断言**了

```
改前：CHECKS=1117  FAILURES=0    (G 只跑 check_root_envelope + check_scenario_g)
改后：CHECKS=1141  FAILURES=0    (G 多跑 check_header_block)
              ^^^^ +24 项
ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
  (A=Recovered+certified, B=StageCompleteUncertified, C=Uncovered/objectLevelEvidenceDropped,
   D=uncovered/noObjectEvidenceExported, E=concurrent-load/counts-identical,
   F=auto-post-window-freeze+Recovered-terminal-in-export)
```

这**部分闭合**了审计"v3 的门禁是空的"：G 的字段集、版本号、`watchCount` 现在都有断言。

## 4. 过程中我自己犯的两个错（都记下）

| 错 | 现象 |
| --- | --- |
| 改了格式串占位符却漏传参数 | `TypeError: %d format: a real number is required, not list` |
| 把 `%s`/`%r` 误转义成 `%%s`/`%%r` | `TypeError: not all arguments converted during string formatting` |

⇒ 两次都是**为了让消息带上版本号**而改动格式串时引入的。教训：**改 `%` 格式串时，占位符与参数元组要同步改**，
且不要为了"转义"而多写一个 `%` —— Python 的 `%` 格式化里 `%%` 只在需要**字面**百分号时使用。

另外，上一轮我因为**过滤流式输出**而抓不到失败行；本轮改成 `*> 文件` 落盘后**一次就看到了**。
⇒ **先把完整输出落盘，再离线检索。**

## 5. 全量验证

```
AutoTest 全量静态                     : 259 scripts, 0 failed
meson test                           : 85 Ok / 0 Fail
wire roundtrip 驱动（含 G 的 header）  : CHECKS=1141, FAILURES=0
```

## 6. 仍未做（G 门禁的剩余部分）

G 仍**跳过**三项 A–F 跑的检查，需要 G 夹具的身份参数才能正确参数化：

| 检查 | 缺什么 |
| --- | --- |
| `check_decoded_events(name, data, identity_weak, lifecycle_identity, identity_proof)` | G 夹具的这三个值 |
| `check_gaps` / `check_chain`（依赖上一项） | 同上 |
| `report_spec` | 同上 |

**不能猜这三个值**（猜错会让判据失去意义）。须先读 G 夹具的 `MakeKey` 再补。

## 7. 现场

```
本轮只改了 AutoTest 驱动（测试代码），未改任何产品源码。
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 多帧回归锁。
现场站点 E:\Work\Warcraft III\d3d9.dll = 基线 F275545B…（未部署）；未提交、未部署、未晋升稳定。
```