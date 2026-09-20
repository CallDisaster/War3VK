# 构建归零；**c2 检查点已相对当前工作树过期** — 2026-09-18

## 1. 发现并修复：构建曾处于**脏状态**

round 236 我改了一处头文件注释。`ninja -C build32 -n` 随即显示 **28 个待做目标** ——
头文件改动触发依赖重编译，而**我上一轮没有重建**。

这违反我自己的规约（`AGENTS.md` 的"确认增量构建没有遗留工作"）。已修复：

```
第一次构建 : [6/6] Linking target src/d3d9/d3d9.dll   （仍余 22 个测试目标）
第二次构建 : [22/22] Linking ...                      （全部完成）
ninja -C build32 -n  =>  ninja: no work to do.        ✅ 归零
```

## 2. 一个反直觉但可解释的现象：**只改注释也会改变二进制**

```
改注释前候选 DLL : CFE40FE5F929F40F94FB81F00B7AF118E2B24D93033EBC5FD0C7EDEEA5D0F31D
改注释后候选 DLL : 77CAEED95684BB596B0E8AACBAEB1B14DC083CD603B09185CC1749546987394D
字节数           : 36,289,280（两次相同）
```

原因：注释行改变了**其下方代码的行号** ⇒ 调试信息/`__LINE__` 相关数据随之变化；
本构建 `b_ndebug=false`（断言开启）也保留更多此类信息 ⇒ 二进制哈希改变。

**推论（对检查点很重要）**：DLL 哈希**不能**用来判定"源码语义是否变了"。
哈希相同 ⇒ 语义必然相同；但**哈希不同 ⇏ 语义变了**。

## 3. ⚠️ c2 检查点已相对当前工作树**过期**

| 量 | c2（round 229 创建） | 当前工作树 |
| --- | --- | --- |
| src 文件数 | 964 | 964 |
| src 指纹 | `D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5` | `C2675AFB5FDA39BD7FE6A2D60EEE10383B72CF996F125B38830C65C7F993F4D9` |
| 候选 DLL | —（c2 不含 DLL） | `77CAEED95684BB596B0E8AACBAEB1B14DC083CD603B09185CC1749546987394D` |

round 229 之后我改过：

```
round 232  AutoTest/test_palette_object_wire_roundtrip.py  （check_header_block 参数化）
round 233  同上（G 段注释）
round 235  src/.../war3_palette_object_wire_roundtrip_test.cpp （夹具声明更正）
round 236  src/.../war3_palette_object_evidence.h            （fail-visible 声明补机制）
```

⇒ **c2 描述的是"round 229 那一刻的源码"，不是当前工作树。**

**这是快照的正常性质，但必须明确说出来** —— 否则有人会拿 c2 的指纹去比对当前树、
发现不一致，然后误以为"检查点坏了"或"树被改坏了"。

**c2 的用途**：把工作树**回退到 round 229 那一刻**（那是"P0 版本门控 + 行为测试 + 检查点①闭环"的状态）。
它**不是**"当前候选"的检查点。

## 4. 当前状态核实

```
ninja -C build32 -n            : no work to do          ✅
src 文件数 / 指纹               : 964 / C2675AFB…         
候选 DLL                        : 36,289,280 B  77CAEED9…
现场站点 E:\Work\Warcraft III   : F275545B…（**仍为基线，未部署**）  ✅
War3 进程                       : none                  ✅
最近改动的测试                   : palette analysis static  OK（97 tests）
                                 wire roundtrip  CHECKS=1137  FAILURES=0
                                 ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED
```

## 5. 为什么本轮**没有**做实机重测

实机重测（验证写方修正① 是否把保留的 FirstSight 从 32 提升向 107）是剩余**最有价值**的证据，
但它需要：部署候选到站点 → 隔离桌面运行 120 s → 取数 → **恢复站点**。

**风险**：若我的上下文在中途耗尽而站点留在**已部署**状态，后果不可接受
（玩家安装目录会被一个未验证的候选覆盖）。

⇒ 我选择**不在上下文紧张时开始**这个序列。下一轮应以**充足上下文**专门做它，
并预留恢复步骤的余量。

## 6. 现场

```
本轮未改任何源码（只做了构建与核实）。
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 多帧回归锁。
未提交、未部署、未晋升稳定。
```