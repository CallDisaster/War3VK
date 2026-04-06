# DXVK-War3 结构整改报告（2026-02-14）

## 1. 本轮目标
- 在不破坏现有功能的前提下，先做低风险结构治理与可交接化。
- 与“渲染链优化”并行，确保代码与文档状态一致。

## 2. 当前结构快照（实测）
- `src/d3d9` 根目录文件数：`118`
- `src/d3d9` 全树文件数：`241`
- `src/d3d9/war3` 文件数：`92`，子目录数：`13`
- 根目录 `d3d9_war3_*.(cpp|h)`：`24`
- `src/d3d9` 根目录 Markdown：`0`

## 3. 本轮已完成整改

### 3.1 文档从源码根目录剥离
- 结果：`src/d3d9` 根目录已无 Markdown 文件，减少“源码/文档混放”。
- 新文档承载目录：`docs/war3/analysis/legacy_d3d9/`

### 3.2 路径引用修复
- 修复文件：`src/d3d9/war3/native/README.md`
- 修复内容：逆向报告路径改为  
  - `docs/war3/analysis/legacy_d3d9/war3_render_reverse_report.md`

### 3.3 新增交接文档
- `docs/war3/analysis/2026-02-14_conservative_merge_and_dispatch_bridge_status.md`
- `docs/war3/analysis/2026-02-14_world_stage_reverse_inference.md`
- 本文件：`docs/war3/architecture/2026-02-14_structure_rectification_report.md`

## 4. 仍存在的结构风险

### 4.1 根目录仍偏大
- `src/d3d9` 根目录 `118` 文件，仍高于“核心目录精简”的目标。
- 巨型文件仍集中在根层：
  - `src/d3d9/d3d9_device.cpp`
  - `src/d3d9/d3d9_war3_shadow.cpp`
  - `src/d3d9/d3d9_war3_hook.cpp`

### 4.2 War3 专有代码仍与 D3D9 主体交织
- `d3d9_war3_*` 仍大量平铺在 `src/d3d9` 根层。
- `src/d3d9/war3` 与 `src/d3d9/d3d9_war3_*` 双轨并行，入口边界仍不清晰。

### 4.3 非源码资产混放
- `src/d3d9/war3/native` 内仍存在多个报告/备份类文件（如 `*.md`, `*.backup`）。
- 建议继续向 `docs/war3/*` 收敛，仅保留源码与必要 README。

## 5. 下一阶段整改计划（可直接执行）

1. 阶段A（低风险，1-2天）  
   - 清理 `src/d3d9/war3/native` 非源码文档到 `docs/war3/native/`。  
   - 保留 `README.md` 与 `README_DEVELOPER.md`，其余报告迁出。  

2. 阶段B（中风险，1-2周）  
   - 拆分 `src/d3d9/d3d9_war3_hook.cpp`：  
     - `war3/hooks/war3_dispatch_hooks.cpp`  
     - `war3/hooks/war3_ui_hooks.cpp`  
     - `war3/hooks/war3_patch_callsites.cpp`  
   - 目标：先不改行为，仅做搬迁与命名清理。  

3. 阶段C（中风险，2-3周）  
   - 拆分 `src/d3d9/d3d9_war3_shadow.cpp` 到 `war3/render/shadow/*`。  
   - 优先拆调试与工具路径，后拆主渲染逻辑。  

## 6. 验证
- 构建命令：`ninja -C build32`
- 状态：通过（本轮整改未引入构建中断）。

