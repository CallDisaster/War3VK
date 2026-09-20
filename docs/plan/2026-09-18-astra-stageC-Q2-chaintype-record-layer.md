# 阶段 C · Q2 第一步：链型进入**记录层**（事件随链型导出）— 2026-09-18

> Goal `goal-d50cfe33-846f-4837-a630-aad33a0143fd`。本轮**改产品源码并重建 DLL**。

## 1. 落地内容

| 层 | 改动 |
| --- | --- |
| `PaletteObjectEventRecord` | 新增 `chainType`（默认 `RejectionRecovery`） |
| `MakeRecord` | 盖 `record.chainType = e.chainType`（链型**随事件**导出，记录层） |
| `PaletteObjectChainType` | 完整定义**前移到记录结构之前**（记录结构携带它且带默认值） |
| `Case 26` | 新增 2 条断言：观察链的**每条**事件必须带 `Observation`；拒绝恢复链的每条必须带 `RejectionRecovery` |

**⇒ `Case 26` 由 6 checks 增至 8 checks，0 failures。**

**⚠️ 明确边界**：本轮**只到记录层**。`chainType` **未上 wire**（`data[4]` 未写）、
`Find`/`Insert` **仍未按链型分区**。这两件事必须**同时**落地：在链型尚未上 wire 之前
让同一对象出现两条条目，读方会按**对象键**把它们并成一条链（比现状更坏的假链）。

## 2. 本轮现场抓出的**三个我自己的错误**（全部是"没验证就以为成立"）

### 2.1 构建失败被我自己的过滤器藏住了

`ninja ... | Select-String 'error C' | Select-Object -First 4` **什么都没打印**，而实际是
`ninja: build stopped: subcommand failed.` —— MSVC 的报错格式不匹配我的模式。
**后果**：我一度以为改动"已应用且通过"，而产物其实是**陈旧的**（src mtime 22:20 > exe 22:15，
且 exe 不含新符号）。

**教训**：**过滤输出前必须先确认构建成败**；"没看到错误"不等于"没有错误"。

### 2.2 前向声明不够 —— 默认成员初始化器需要**完整**枚举

`chainType = PaletteObjectChainType::RejectionRecovery` 出现在**只有前向声明**的位置
⇒ `'RejectionRecovery' is not a member of …`。scoped enum 可前向声明，但**枚举子不可见**。
**修正**：把完整定义前移，而不是前向声明。

### 2.3 枚举的 scope 我猜错了

我先试 `PaletteObjectChainType`（非限定）、再试 `PaletteObjectEvidence::PaletteObjectChainType`，
**两次都错**（它既不在类内，也不在测试的可见 scope）。
**处置**：测试改为**比较底层值**（`static_cast<uint32_t>(…) != 1u` / `!= 0u`），
并注明"不点名枚举类型（其 scope 不宜在测试里硬编码）"。
⇒ 这是**回避**而非解决：枚举的实际 scope 应被查明并统一（见 §4）。

## 3. 状态

```
AutoTest 全量静态    : 259 scripts, 0 failed
meson               : Ok: 85  Fail: 0
ninja -C build32 -n : no work to do
evidence test       : SUMMARY: 26 passed, 0 failed   （Case 26 = 8 checks）
wire roundtrip      : CHECKS=1160 FAILURES=0
候选 DLL            : 36,297,544 B
                      SHA-256 79B3C229AE7715252BB1BE171BB38F0AED3FD080346233C7FC513CE642EC2C83
站点                : E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
git                 : 无写操作
```

## 4. 下一步（Q2 的实质部分，必须**一次做齐**）

```
④ 查明并统一 PaletteObjectChainType 的 scope（消除测试里的底层值比较）
⑤ Find/Insert 按 (对象键 × 链型 × 窗口) 匹配 ⇒ 两类链各自独立、同一对象可同时持有两条
⑥ 定义归属规则：S/E/D/ObjectGone/CloseWindow 在两类链并存时挂到哪条（必须显式，不得默认）
⑦ 链型上 wire：记录结构已有字段 ⇒ sink 写 data[4]；读方 v4 的 RESERVED_DATA 必须
   **按版本**排除 4（现存旧毛病：版本无关的保留槽校验），并新增 data[4] 的链型解码
⑧ 读方链分组键由"仅对象键"改为「对象键 × 链型」
⑨ 负向探针：同对象两类链各自结算；链型与事件形状不符必须拒绝；v1/v2/v3 的 data[4] 必须为零
```

## 5. 不声称

- **不**声称链型已上 wire（**未上**）；**不**声称 `Find`/`Insert` 已按链型分区（**未做**）；
- **不**声称同一对象可同时持有两条链（**目前仍不能** —— `NoteFirstSight` 仍会复用拒绝条目，
  即复核的 `R→FirstSight` 改类缺陷**仍未修**）；
- **不**声称阶段 C 主体完成；K3（跨帧判序）未做；
- 全局边界依旧：一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。
