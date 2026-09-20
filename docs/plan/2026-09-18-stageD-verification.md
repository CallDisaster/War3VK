# 阶段 D · 单独验证记录（权威单跑）— 2026-09-18（round 50）

> Goal goal-d50cfe33-846f-4837-a630-aad33a0143fd。
> 裁定要求「每阶段单独出包、单独验证」。本文是 **D 阶段**的那次单独验证。

## 1. 门禁结果（同一轮内一次跑完）

| 门禁 | 结果 |
| --- | --- |
| AutoTest 静态全量 | **261 脚本 / 0 失败** |
| meson test | **Ok: 85 / Fail: 0** |
| `ninja -C build32 -n` | **no work to do** |
| palette 证据宿主机测试 | **31 passed, 0 failed** |
| 生产编码往返（wire roundtrip） | **CHECKS=1160 FAILURES=0** |
| 读方 analysis static | **OK** |
| 读方 analyze_frame_evidence | **OK**（注：**不在** 261 静态门禁通配内，见 §4） |
| D2 顺序锁（单跑） | palette object **arm-order static checks passed** |
| D3 快照锁（单跑） | palette object **header single-snapshot static checks passed** |

## 2. 本阶段改动的哈希（可核对）

```
d3d9.dll (build32)         = 41A70AA50AEABE4245F3D9F640ECC79501D403720AF7F1CE4DACE06A7E9BF951
                            36,326,521 B
war3_palette_object_evidence_sink.cpp  = B4733CD31AC9D0CCB2388C0F048A6724FDF2D4C4013F9B3E32A3DB8AE72D7CC7
war3_frame_evidence.cpp                = 860AEBA581B366BDC10E9C59587918C29D55341C16BFC612ED328240BCD9FC1C
war3_palette_object_evidence.h         = 5F3758DD7335C99A86D06775F264452D6A96851D9DFD5B10DD9194348B414318
```

## 3. ★ 站点核对：**未部署**（这是硬约束的机械核对，不是声明）

```
E:\Work\Warcraft III\d3d9.dll = F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
```

⇒ 与**基线**哈希**逐字相同** ⇒ 本阶段所有改动**都只在开发树里**，
**没有**进入玩家站点目录。

## 4. ⚠️ 本记录**不能**证明的事

1. **不能**证明 D 阶段完成：**B1 的运行期半边未做**；B3 尚未裁定是否必要；
2. **不能**证明实机行为有任何改变：D1/D2 是**结构性/同步域**修复，
   D3 是"已满足 + 加锁"；**没有**任何实机观测支持"症状被修好了"；
3. `test_analyze_frame_evidence.py` **不在** 261 静态门禁的通配内（`test_*_static.py` 不匹配该名）
   —— 本记录里的 OK 是**我手工补跑**，**不是门禁的一部分**；
4. 两个 P0 **都未完成** ⇒ 依照裁定，**不新增实机因果结论、不晋升稳定候选**；
5. 本轮**没有**"出包"（未生成交付 zip）：裁定要求每阶段单独出包，
   而 D 阶段尚有 B1 运行期半边未做 ⇒ **现在出包会把未完成阶段伪装成完成**，
   故**暂不出包**，留待 B1 完成后。

## 5. 不声称全门禁通过

第 1 节列的是**我跑到的**门禁。裁定明确禁止"声称全门禁通过" ⇒
本记录只陈述**上述各条各自的结果**，不把它合并成一句"全门禁通过"。

## 6. 全局边界（每轮重申）

一条链结算不代表观察完整，观察完整不代表对象已证明，更不代表阴影已恢复。