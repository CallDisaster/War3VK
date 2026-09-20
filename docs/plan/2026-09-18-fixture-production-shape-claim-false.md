# 🔴 夹具的"生产形状"声明是**假的**，已更正；并暴露一个**覆盖缺口** — 2026-09-18

## 1. 假声明

`war3_palette_object_wire_roundtrip_test.cpp:221`（原文）：

```cpp
// 键取**生产形状**（identityWeak=true、lifecycleIdentity=0），与生产采集点的真实取值一致。
```

**这句话是假的。** 该场景用 `MakeKey(io.session, true, 0u)`，而 `MakeKey`（`:85-87`）会写：

```cpp
key.epochUnknown = false;              // 生产恒为 true
key.deviceEpoch  = kDeviceEpoch;       // 非零；生产恒为 0
```

## 2. 逐字段对照（已写进源码）

| 字段 | G 夹具 | 生产采集点（`war3_palette_object_capture.h:151-166`） | 一致? |
| --- | --- | --- | --- |
| `identityWeak` | `true` | `true` | ✅ |
| `lifecycleIdentity` | `0` | `0` | ✅ |
| `epochUnknown` | `false` | **`true`** | ❌ |
| `deviceEpoch` | `kDeviceEpoch`（非零） | **`0`** | ❌ |

⇒ **G 与生产只在两个字段上一致。**

## 3. 为什么夹具要这样（不是笔误）

`MakeKey:86` 自带说明：

```cpp
// epochUnknown=false 时必须给出**真实**设备代际，不得用 0 冒充"已知且为零"。
```

⇒ 夹具刻意走"**设备代际已知且非零**"的形状，那是**版本 2 合同**下的形状；
而生产恒为 `epochUnknown=true` + `deviceEpoch=0`（弱身份 + 未知代际）。

## 4. 🔴 由这条假声明掩盖的真实问题：**生产身份形状零覆盖**

```
本驱动（A–G 七个场景）没有任何一个场景使用生产的身份形状
  (identityWeak=true, epochUnknown=true, deviceEpoch=0, lifecycleIdentity=0)
```

**实机导出已自证这一差异**：

```
生产导出：epochUnknownRecords = 0 而 weakIdentityRecords = 1
但生产**必然**是两者同时非零 —— 因为生产键恒 epochUnknown=true
⇒ 说明该导出的键形状与"真实生产键"并不同源（也与本夹具的假设并不同源）
```

⇒ **这是一个已知的覆盖缺口。** 此前它被那句假声明"与生产一致"掩盖着 ——
读到那句话的人会以为生产形状已被覆盖。

## 5. 我的处置：改注释，把真相与缺口都写下来

已把第 221 行改为逐字段对照表 + 明确声明"**生产身份形状在本驱动中没有任何场景覆盖**"。

**没有改夹具取值**：那会牵动 A–G 大量断言（`identity_proof` 档位、版本 2 的证明种类判定等），
需要一次成规模的改造与重新基准化。**本轮只消除假声明，不伪装已覆盖。**

## 6. 验证

```
重建 : war3_palette_object_wire_roundtrip_test.exe 成功
驱动 : exit=0  CHECKS=1137  FAILURES=0
       ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
```

（仅注释改动，行为不变 —— 计数与上一轮一致，这同时也再次印证了驱动确定性。）

## 7. 现场

```
本轮只改了测试文件的一处注释，未改产品源码、未改夹具取值。
现场站点 = 基线 F275545B…（未部署）；未提交、未部署、未晋升稳定。
```