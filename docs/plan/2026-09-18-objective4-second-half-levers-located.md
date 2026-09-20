# 目标④ 第二半：操纵杆与对照入口已定位（无需重新构建）— 2026-09-18

## 1. 严格契约的开关（`src/d3d9/war3/render/war3_skin_palette_selection.h:7-33`）

```cpp
#ifndef WARVK_SKIN_PALETTE_CONTRACT_DEFAULT
#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0
#endif

inline bool ContractEnabled() noexcept {
  static const bool enabled=[] {const char* v=std::getenv("DXVK_WAR3_SKIN_PALETTE_CONTRACT");
    return v?std::strcmp(v,"1")==0:WARVK_SKIN_PALETTE_CONTRACT_DEFAULT!=0;}();
  return enabled;
}
```

| 层次 | 值 |
| --- | --- |
| 编译期默认 | `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` ← `meson.build:399-400` 在 `warvk_skin_palette_contract_candidate` 时定义为 1 |
| 本构建实测（round 227） | `warvk_skin_palette_contract_candidate = True` ⇒ 默认 **1（开）** |
| 运行时覆盖 | 环境变量 **`DXVK_WAR3_SKIN_PALETTE_CONTRACT`**（`"1"` 开；其它值关） |

### ⚠️ 一个必须记住的约束

`static const bool enabled = [...]()` ⇒ **只在首次调用时读一次并锁存**。
⇒ 环境变量必须在**进程首次调用 `ContractEnabled()` 之前**设置；**运行中途改它无效**。
⇒ 做正常/异常对照时，只能通过**两次独立进程启动**（不同 env）来对比，不能在单次运行内切换。

## 2. 契约确认是活代码（不是本构建里的死分支）

```
ContractEnabled         : 19 处
  d3d9_device.cpp(6)  war3_model_hook.cpp(7)  war3_live_palette_selection.cpp(1)
  war3_live_palette_selection_chain_legacy_reference.inc(1)
  war3_frame_evidence.cpp(1)  war3_internal_test_config.h(1)
  war3_skin_palette_admission_test.cpp(1)  war3_skin_palette_selection.h(1)

GroupRange(             : 11 处
  war3_skin_palette_selection.h(4)  war3_skin_palette_admission_test.cpp(6)  war3_model_hook.cpp(1)

skin::Source::Legacy*/CapturedWriter : 6 处
  d3d9_device.cpp(3)  war3_current_draw_contract.cpp(1)  war3_model_hook.cpp(1)
  war3_skin_palette_admission_test.cpp(1)
```

⇒ 契约在**真实选择入口**（`war3_live_palette_selection.cpp`）与**原生模型路径**（`war3_model_hook.cpp`）上都被查询。

## 3. 目标④ 四个子问题各自对应到什么

| 子问题 | 对应代码 | 已有工具 |
| --- | --- | --- |
| **选中什么数据** | `struct Selection`（`h:20-28`）：`source`/`space`/`domain`/`slot`/`actualGroupCount`/`ownerEpoch`/`publicationTicket`/`captureSerial`/`hash` 等；`enum class Source { Unknown, CapturedWriter, CapturedRawArena, OwnedPartSnapshot, LegacyGlobalSlot, LegacySlotCache, PoseGroups, CModelGroups }` | `skin-selection/v1` wire 标签（`d3d9_device.cpp:20977`，受 `InputsEnabled()`） |
| **为何允许** | `ContractEnabled()`（开/关）+ `GroupRange(required, actual)`（`h:34-36`：`required && actual && actual<=256 && required<=actual`） | `war3_skin_palette_admission_test.cpp`（6 处 `GroupRange`） |
| **提交时是否被换掉** | `Selection` 被消费处：`d3d9_device.cpp`(6 处契约检查) 与 `war3_current_draw_contract.cpp` | 同上 |
| **正常对象是否被误伤** | 拒绝路径的判定与计数 | 同上 + `ContractEnabled` 关闭时的对照 |

## 4. 下一步（具体、可执行，下一轮）

```
① 读 war3_skin_palette_admission_test.cpp —— 它已有 6 处 GroupRange + ContractEnabled，
   很可能就是现成的"准入正常/异常对照"harness；先搞清它覆盖了什么、缺什么。
② 用两次进程启动做对照（因 ContractEnabled 是锁存的）：
   正常 : DXVK_WAR3_SKIN_PALETTE_CONTRACT=1（= 本构建默认）
   异常 : DXVK_WAR3_SKIN_PALETTE_CONTRACT=0
   观察 : skin-selection/v1 事件中 source/slot/actualGroupCount 的分布变化，
          以及正常对象是否因关闭契约而被误伤（或反之：开启契约时是否拒绝了本应通过的对象）。
③ 明确记录：这是**隔离桌面**的功能对照，**不是前台性能数据**（硬约束）。
```

## 5. 现场

```
站点 E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
本轮未改任何文件（只读代码）。
未提交、未部署、未晋升稳定；候选 ≠ 发布。
```