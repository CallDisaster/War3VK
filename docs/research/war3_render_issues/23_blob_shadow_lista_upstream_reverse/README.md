# 23 - Blob 阴影 / ListA 上游写入链逆向

## 目标
1. 搞清楚 Warcraft III 原生 blob 阴影里，为什么“动态单位阴影”能靠 `ListB` 关掉，而“建筑静态阴影”还会残留。  
2. 找到真正往 `ListA` 混入建筑静态阴影的上游模块，避免在 `ListA` 末端误伤战争迷雾、边界、地形变暗。  

## 先给结论
1. `ListA` 不是“建筑阴影专表”，而是**地形阴影/贴花/边界/雾的混合结果层**。  
2. `0x6F713250(TerrainShadow_RegisterImageEntry)` 分配的是 `0xA0` 条目池，不是 `0x94` 的 `ListAEntry` 本体；它更接近“image/stamp 注册层”，不是 `ListA` 网格初始化函数。  
3. `ListA` 的真正网格构建链是：
   - `0x6F738ED0`
   - `0x6F73DC00`
   - `0x6F73D9F0`
4. 建筑静态阴影真正的“上游写入模块”不是 `ListA` 渲染，而是三条更上层的 stamp/splat 路径：
   - `ShadowPath_StaticStamp_Toggle(0x6F74E420)`  
   - `TerrainShadow_ToggleStaticStampFromObject(0x6F74DB30)`  
   - `TerrainShadow_ToggleEmitterStamp(0x6F74DE40)`  
5. 这三条路径又被同一个对象级调度层统一驱动：
   - `0x6F74D500`
   - `0x6F751290`
   - `0x6F759880`
   - `0x6F7599F0`
   - `0x6F75C5F0`
6. 如果目标是“只禁建筑静态阴影，不碰雾/边界”，**正确拦截点应当在 stamp/splat 上游，而不是 ListA 末端**。  

## 关键链路

### 1. ListA 真正的构建链
1. `0x6F738ED0`
   - 职责：重建当前活跃地形阴影区域。
   - 关键动作：
     - 计算裁剪后的活跃矩形；
     - 重建顶点/高度相关网格；
     - 调 `0x6F729920` 更新世界包围盒；
     - 调 `0x6F73DC00` 批量初始化 `ListAEntry`；
     - 调 `0x6F71D690 / 0x6F739370 / 0x6F7395C0 / 0x6F72A140 / 0x6F72F980` 重建辅助状态与 dirty 区。
   - 结论：它是 `ListA` 的“区域重建总控”，不是单个阴影条目注册入口。

2. `0x6F73DC00`
   - 职责：按 `((width-1)>>2)+1` / `((height-1)>>2)+1` 建立 `0x94` stride 的 `ListAEntry` 网格。
   - 关键字段：
     - `this + 0x104` 对应 `listA_ptr/base`
     - 每个 entry 大小 `0x94`

3. `0x6F73D9F0`
   - 职责：初始化单个 `ListAEntry`。
   - 已确认字段：
     - `entry + 0x30 = gridX`
     - `entry + 0x34 = gridY`
     - `entry + 0x38/+0x3C = worldPosX/worldPosY`
     - `entry + 0x4C = flagsWord`
     - `entry + 0x4E = flagsByte`
     - `entry + 0x50/+0x54/+0x58/+0x5C` 为 group 相关字段
     - `entry + 0x80 = layer->sharedBlobLikeResource`
     - `entry + 0x90 = ownerTerrainShadowLayer`
   - 结论：`ListAEntry + 0x80` 不是“某个建筑对象专属阴影”，而是 layer 级共享资源入口。

### 2. ListA 末端渲染链
1. `0x6F737500(TerrainShadow_RenderListA)`
   - 先看 `entry->typeId` 查 `ShadowTypeInfo`
   - 若 `entry + 0x80 != 0`，直接提交单块纹理
   - 再遍历 `entry->groupListB`
2. `0x6F737860`
   - 遍历 `entry->groupListA`
3. `0x6F7376E0`
   - 按 `flags_970`/dirty 状态决定是否整理 `ListA`

结论：
1. `ListA` 到了渲染阶段已经是“混合后结果”，很难再区分建筑阴影、边界和雾。  
2. 所以 `Hook_Terrain_RenderListA` 永远只能当兜底，不适合作为主治理点。  

## 建筑静态阴影的真正上游

### A. 直写静态 stamp 路径
1. `0x6F74E420(ShadowPath_StaticStamp_Toggle)`
   - enable 时直接调用 `0x6F713B20(ShadowStamp_WriteByName)`
   - 按 `ReplaceableTextures\\Shadows\\<name>` 写入
   - 这条链**不经过 RegisterImage**
2. `0x6F713B20`
   - 把名字拼成 `ReplaceableTextures\\Shadows\\*`
   - 再进入 `ShadowStamp_WriteCore`
3. `0x6F713920(ShadowStamp_WriteCore)`
   - 直接改写 layer 内部的静态 stamp 字节网格（`this + 0x240`）
   - 尾部调用 `0x6F72FA40`，把受影响像素矩形换算到 `4x4` 的 `ListAEntry` 网格并打脏标记
4. `0x6F7395C0`
   - 在活跃区域重建时把 `this + 0x240` 的字节网格裁剪/复制到当前窗口
   - 然后调用 `0x6F72ABD0 / 0x6F742470` 刷新 `ListA` 相关辅助结构

结论：
1. 这条链是建筑静态阴影最容易漏掉的旁路。  
2. 它不仅“写名字”，而且会直接把结果烘进 `ListA` 依赖的字节网格，再驱动 `ListAEntry` 脏区刷新。  
3. 只 hook `RegisterImage` 而不 hook `StaticStampPath`，仍可能有建筑阴影残留。  

### B. 对象静态 stamp 注册路径
1. `0x6F74DB30(TerrainShadow_ToggleStaticStampFromObject)`
   - enable 时：
     - 通过虚函数拿到名字和矩形
     - 调 `TerrainShadow_RegisterImageEntry(..., type=0)`
     - 返回索引写到 `object + 136`
   - disable 时调 `0x6F736750` 释放

### C. emitter stamp 注册路径
1. `0x6F74DE40(TerrainShadow_ToggleEmitterStamp)`
   - enable 时：
     - 解析资源路径与半径
     - 调 `TerrainShadow_RegisterImageEntry(..., type=4)`
     - 返回索引写到 `object + 144`
   - disable 时同样走 `0x6F736750`

### D. UberSplat / splat manager 路径
1. `0x6F76D790(ShadowProjector_Add_Simple)`
2. `0x6F76D800(ShadowProjector_Add_FromObject)`
3. 两者最终都进入 `0x6F713CA0`
4. `0x6F713CA0`
   - 文件来源直接落在 `CTerrainUberSplats.cpp`
   - 内部调用 `TerrainShadow_RegisterImageEntryWithParams`
   - 再进 `TerrainShadow_RegisterImageEntry`

结论：
1. 这条链就是之前自动场景里稳定命中的 `WithParams + UberSplat`。  
2. 进一步实机对照后，应把它更谨慎地定义为“建筑/矿点等对象的地面贴花样式路径”，而不是当前主结论里的“静态阴影本体”。  
3. 它虽然会和地形阴影系统混在一起，但默认不应该当作要优先拦掉的建筑 blob 阴影主路径。  
4. 这也解释了为什么误拦 `WithParams + UberSplat` 时，先消失的是人类/兽族/亡灵等建筑底座贴花，而不是我们真正想去掉的静态阴影。  

## 对象级总调度层
这轮最重要的新发现，是建筑静态阴影并不是零散函数各管各的，而是被同一个对象级模块统一开关：

1. `0x6F74D500`
   - 创建/激活对象时：
     - `ShadowPath_StaticStamp_Toggle(enable)`
     - `TerrainShadow_ToggleStaticStampFromObject(enable)`
     - `TerrainShadow_ToggleEmitterStamp(enable)`

2. `0x6F751290`
   - 删除/禁用对象时，统一反向关闭这三条链。

3. `0x6F759880`
   - 根据 feature mask 开启对象的 shadow/stamp 相关功能。

4. `0x6F7599F0`
   - 根据 feature mask 关闭对象的 shadow/stamp 相关功能。

5. `0x6F75C5F0`
   - 参数变化后先关后开，触发一次 retoggle。

结论：
1. 如果要在“更上层”禁用建筑静态阴影，这一层就是目前已知最接近对象语义、同时又能一把卡住三条路径的调度层。  
2. 但它的对象类型语义还没完全收口，所以工程上更稳的是：
   - 先 hook `StaticStampPath`
   - 再 hook `RegisterImage` 的来源分类
   - 让 `ListA` 渲染末端继续保持关闭或仅兜底。  

## 对 `RegisterImage = ListA 上游` 的修正
旧结论里把 `0x713250` 直接视作“ListA 上游写入点”，这个说法不够精确。  

更准确的说法是：
1. `0x713250` 是 **stamp/image 注册池入口**；
2. `ListA` 自己的网格条目是 `0x94`，由 `0x73DC00/0x73D9F0` 批量初始化；
3. 建筑静态阴影之所以“看起来在 ListA”，是因为它们最终写进了同一套地形阴影/贴花混合层；
4. 所以真正该禁的是**上游 stamp/splat 生产者**，而不是 `ListA` 自己。  

## 推荐拦截点

### 推荐主路径
1. `ShadowPath_StaticStamp_Toggle(0x6F74E420)`
   - 理由：直接覆盖 `ReplaceableTextures\\Shadows\\*` 写入旁路。

2. `TerrainShadow_RegisterImageEntry(0x6F713250)`
   - 但必须按返回地址来源分类：
     - `0x74DBFA = StaticStamp`
     - `0x74DF55 = EmitterStamp`
     - `0x7291DC = WithParams`
     - `0x76D44A/0x76D69A/0x76D719 = object/from-point/from-two-points`

### 推荐兜底
1. `ShadowUpdate_WriteEntry(0x6F73F7A0)`
   - 适合做最后的 callback 级精确封堵。
   - 不适合作为第一主路径，因为太靠后，且已经进入混合层写入。

2. `TerrainShadow_RenderListA(0x6F737500)`
   - 仅保留兜底观察，不建议作为生产治理入口。

## 当前工程落点
1. 代码里已经有专门的 `StaticStampPath` 窄拦截：
   - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
   - `Hook_ShadowPath_StaticStamp_Toggle`
2. 地址和安装链也已经纳入：
   - `src/d3d9/war3/hooks/war3_hook_address_book.cpp`
   - `src/d3d9/d3d9_war3_hook.cpp`
3. 当前配置默认是：
   - `kNativeShadowStaticStampPathHookEnabled = true`
   - `kNativeShadowBlockStaticStampPathWhenMode1 = true`
   - `kNativeShadowBlockStaticMaskLoadWhenMode1 = true`
   - `kNativeShadowRegisterBlockShadowTextureKeyWhenMode1 = true`
   - `kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1 = false`
4. 因此，这轮专题的主要价值不再是“再找一个新的末端过滤点”，而是把事实基线统一到
   `StaticStampPath -> shared mask -> ListA`
   这条真正的上游链上。

## 置信度
1. `ListA` 网格初始化链：高  
2. `RegisterImage/WithParams/UberSplat` 是建筑静态阴影上游之一：高  
3. `StaticStampPath` 是独立旁路：高  
4. `0x74D500/0x751290/0x759880/0x7599F0/0x75C5F0` 是统一对象调度层：中高  
5. `ListAEntry + 0x80` 的确切对象类型名：中  

## 当前工程结论
1. 不要继续试图“从 ListA 里分离建筑阴影”。  
2. 应把治理点前移到：
   - `StaticStampPath`
   - `RegisterImage` 来源分类
   - `WithParams + UberSplat` 仅保留诊断，不再默认当作阴影本体拦截
3. `ListA` 应继续视作“最终混合层”，只保留保守兜底，不做主拦截。  
