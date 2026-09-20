# 检查点 c2：已生成，但**还原性尚未验证**（我的校验脚本有嫌疑缺陷）— 2026-09-18

## 1. 结论先行

| 项 | 状态 |
| --- | --- |
| c2 归档已生成 | ✅ 26.62 MB |
| 归档内含源码/根配置/AutoTest/Shader/构建选项 | ✅ |
| **还原性验证** | ❌ **未通过（且未排除是校验脚本自身的错）** |

## 2. 生成过程与两次失败

### 失败 1：选择范围纳入了 32 GB 运行产物

第一版直接把 `src/shaders/include/smaa/AutoTest/tools/WarVK` 全量复制，结果：

```
staged size: 32,095 MB      files: 12,469
```

根因（已定位）：**`AutoTest\artifacts` = 31,974 MB**

```
AutoTest\artifacts                             31,974 MB
  \frame_evidence_runs                         20,241 MB
  \self_contained_recorder_runs                 6,605 MB
  \v122_native_lights_20260914                  2,651 MB
```

⇒ **检查点必须排除 `AutoTest/artifacts`** —— 这是检查点①的一条实质约束（c1 只有 20.8 MB，应当也是这么做的，但 c1 没写下选择规则）。

已清理该 32 GB 临时目录（E: 剩余 82.3 GB）。

### 失败 2：还原性校验 `MATCH: False`，但文件数一致

排除 artifacts 后重建：

```
staged  : 121.5 MB
zip     : 26.62 MB
zip sha : 0DDE7D4E229B0298663CA5DAF2BFF33EC966A03958ED6A61FEBE8C26856B7992
repo fp : 964|D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5
zip  fp : 964|D6882B1B836C6FA63F27DDA6D6411EDC7DE8756121F08DD411663D0E23C19FAC
MATCH   : False
```

**关键观察：文件数两边都是 964，只有哈希不同。**

## 3. 我的首要嫌疑：**校验脚本自身**

指纹 recipe 用的是相对路径 `$_.FullName.Substring($base.Length+1)`。而 `$env:TEMP` 的实际路径是

```
D:\tmp\AppData\ADMINI~1\Local\Temp\wvkc2b-ex
                    ^^^^^^^^ 8.3 短名
```

`Get-ChildItem` 返回的 `FullName` 很可能是**长名**形式（`Administrator`），而我传入的 `$base` 是**短名**形式
⇒ `Substring($base.Length+1)` 切掉的字符数与实际不符 ⇒ **相对路径整体错位** ⇒ 哈希必然不同，
**而文件数不受影响** —— 与观测完全吻合。

⇒ **`MATCH: False` 很可能是我的测量误差，不是内容不一致。**

这与 round 226 的教训同源：**比较失败时，先怀疑比较本身。**

## 4. 我还**没有**排除的确切风险

另一可能：`Compress-Archive`/`Expand-Archive` 对某些路径做了改动（长路径、特殊字符、空目录），
或复制时有文件被跳过而恰好由别处补上。**我尚未排除这一可能。**

⇒ 因此**不能**声称 c2"可还原"。**目标①仍未闭环。**

## 5. 下一步（明确、可执行）

1. 把暂存/解压目录放到**不含 8.3 短名**的路径（如 `E:\Work\_ckpt\c2`），重跑校验；
2. 比较时改用**相对路径由 `Resolve-Path -Relative` 或手工以分隔符切分**得出，不依赖 `Length` 偏移；
3. 若仍不一致，逐文件比对（先比长度集合、再比哈希集合）以区分"复制丢失"与"路径错位"；
4. 校验通过后，**把选择规则与排除规则一并写进检查点文档**（c1 的教训：规则没写下 ⇒ 不可复核）。

## 6. 当前候选与现场

```
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 多帧回归锁
验证：war3_palette_object_evidence_test 24 passed 0 failed；meson 85 Ok / 0 Fail；静态 259/0
现场站点 E:\Work\Warcraft III\d3d9.dll = 基线 F275545B…（未部署）
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```