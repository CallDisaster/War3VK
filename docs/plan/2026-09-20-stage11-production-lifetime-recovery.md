# Stage11 生产页寿命分组与失败安全（主线程实施）

## 范围与历史

用户要求主线程直接推进缓存/预算恢复，不扩大384MiB验收预算。前一玩家E42仍失败，不能把页政策纯组件算成修复。改前精确文件备份在 `AutoTest/artifacts/day-recovery-20260920/before/`，保留全部其他dirty内容；不覆盖玩家DLL，不提交/合并/发布。

只读历史复核：B HEAD ae890542已含744e6fa（4MiB页/384MiB）及6a4b100（16MiB页）。因此不能把分页本身说成最近两天新引入。旧A仍为独立buffer，静态GC总量目标与B近期工作集保护不同；但未绑定用户上一可恢复DLL，尚不能定位唯一回归提交。当前GC/静态分类相对B HEAD未扩大。近期无UV释放/失败寿命修正的方向不等于已经证明整体运行收益。

384MiB是软件活动页上限，不是全项目或整卡VRAM总量；未找到其按硬件容量自动推导的依据。可以另行评估更大配置，但它会增加资源/地址空间压力且不修复钉页；本批不增预算。16MiB粒度原因为24次页创建可低于既有32次单帧创建门。

## 实施合同

1. 使用已有 `War3PlanStage11LifetimePage` 纯政策，而非第三套策略。生产页记录出生map/device epoch与保留意图，position/UV/index三个实际申请点传同一个当场分类。分类不改变caster资格或缓存静态判定，未知不伪称动态。
2. 严格同类页尾优先；没有同类尾部时，在共享预算和既有创建门内创建新页。受预算/创建门限制，或可恢复的创建/发布失败时，仍尝试其它同代活动页的合法尾部，不拆成互不借用的硬子池。
3. 每次申请最多一次真实buffer创建。只捕获具名allocation failure且确认device健康；device lost/未知错误不转成成功、不无限重试。明确不吞整个createBuffer的std::bad_alloc：底层子区间/对象池尚无完整宿主机异常回滚，该类失败继续向上传播。页内不压实、不复用空洞、不倒退used，不改已有CS/GPU退休机制。
4. 底层构造先分配/初始化后注册资源；空allocation在解引用前抛具名错误。新VkBuffer在移交Rc allocation前有栈上清理，日志或内存分配抛出也不遗留裸handle；移交后bind失败由既有Rc清理。
5. 仅新建或跨类借用时在既有CPU证据环写 `snapshot-lifetime/v1`（已启用session才写）；不新增大环/worker/矩阵扫描。事件含请求与页类别、出生代际、创建/借用、range、resident/cap，不证明GPU完成。
6. 不保证所有合法工作集都能塞进384MiB，也不声称三个寿命标签涵盖每个中途变化；本批处理已确认的跨寿命混页机制，效果以压力后恢复门裁决。已有不完整候选拒绝与最多8帧旧图保留均不改。

## 验证

- 生产共用初始化/裸handle清理函数的故障测试；null、assign抛出、publish抛出、Rc退出释放、未知错误不转allocation类型。
- 生产页政策：固定长期小锚点+短期大分配，旧混页策略与分组策略对照；共享上限、尾部保底、epoch/对齐、旧图完整性规则仍由原测试覆盖。
- 接线守卫覆盖三个真实流、唯一创建点、具名catch/device健康、出生epoch、无倒退used/无同步等待；这不是GPU路径实机证明。
- 编译父BelowNormal、最多-j2、exactDLL和显式CPU target；玩家进程运行时不部署/自动实机。新身份不继承E42验收。
- 实机后续：同地图正常→低视角→多地点→返回，比较页resident、拒绝、完整图序号增长、合法caster及画面；生产分组必须有实际覆盖。源码/CPU通过不计实机通过。

## 一手同步依据

- [Khronos vkDestroyBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyBuffer.html)：提交中引用buffer的命令必须完成才能销毁。本补丁的裸handle仅限未发布、从未提交的构造期；已发布buffer保留DXVK原跟踪。
- [Khronos vkFreeMemory](https://docs.vulkan.org/refpages/latest/refpages/source/vkFreeMemory.html)：关联提交完成是释放前提；无cache引用不等于可覆写GPU使用中的空间，因此本批不回收页内洞。
- [Khronos VkResult](https://docs.vulkan.org/refpages/latest/refpages/source/VkResult.html)：host/device OOM与DEVICE_LOST分别处理，不用catch-all重试设备丢失。

## 当前检查点

### 2026-09-20 离线候选闭合

- 主线程实现；只读审查指出不能吞整个allocator的`std::bad_alloc`、也不能丢弃底层非OOM VkResult。本次已去掉前者的恢复catch、保留后者错误类型并继续传播。没有宣称通用allocator具备完整异常回滚。
- 首轮CPU测试失败于旧策略夹具：将新best-fit策略的全部页标Unknown并不等于旧的逆序首个可用尾部策略。已修夹具为改前真实逆序策略，生产策略未因此更改。80周期、每周期15MiB短期+256KiB长期锚点：旧策略56次拒绝，新策略0次、峰值48MiB；这是合成CPU序列，不是玩家显存收益。
- 新CPU生产共用helper/策略测试841断言通过；旧纯政策120断言通过；Meson `--no-rebuild --num-processes 2` 92/92，AutoTest根目录`test_*_static.py`脚本271/271。不是完整Python或GPU回归。
- 首轮静态270/271：旧页池守卫要求旧的`publication != Success`文本形态且禁止任何catch。已更新为成功后才记创建次数、仅typed OOM可catch且device不健康立即重抛；未删除标量提交/未知错误传播合同。另保持publication失败具名值，避免新尾部回退丢掉诊断。最终全量静态重跑271/271。
- 原始Meson重生成后exact DLL+显式CPU目标54/54；修夹具仅CPU2/2；最后保留publication原因的DLL重建2/2，均BelowNormal/-j2。exact DLL和CPU target no-work；两Python文件py_compile、git diff --check通过。
- 最终DLL：PE32/i386，36,412,825 bytes / SHA-256 `FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529`。
- 玩家`E:/Work/Warcraft III/d3d9.dll`仍36,400,299 / `E42CD88CD266CAF950E77001386C71B3B79105DE0A8AD41272C414B87124E814`；未部署、未游戏、未提交、未发布，结束检查游戏/编译相关进程0。
- 本轮相对原文件备份的六个既有源码/测试差异：device.cpp +113/-56、device.h +7、meson +8、dxvk_buffer +8/-6、dxvk_memory +34/-3、旧页池static +9/-2；另新增guard头、CPU测试、接线static和本文。保留原dirty内容。
- 首次构建前冻结2976个文件，构建后逐SHA无变化。后续有意修改仅CPU夹具、旧static和device诊断原因；最终重新冻结，文档与开发日志为本轮明确更新。
- 日志：`AutoTest/artifacts/day-recovery-20260920/`的build/rebuild、meson-tests-final、static-final。旧失败未追认为通过。

### 剩余门与长期边界

实际大图的压力→移动→回原地点恢复、合法caster完整性、GPU无故障、帧耗时/尾延迟均未验证；不保证384MiB能承载任意工作集。寿命分类是保留意图而不是所有权/几何证明，中途变化与借用仍可能混页。生产选择扫描至多32页，身份校验含有界二重循环，其CPU热路径成本尚待实测。

本批收拢到已有纯页政策并把三个流的寿命/出生代际一起交接，不另建释放系统；仍依赖原缓存GC、DXVK最后使用跟踪。未做页内压实、共享模型包扩面、实际剔除或M3/M4迁移，不能将本修复计为长期计划完成。
