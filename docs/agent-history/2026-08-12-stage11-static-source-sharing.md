# Stage11 静态源共享候选

## 结论

高压“生与死”隔离桌面 A/B 表明，严格的 Stage11 静态源直绑可以停止静态
缓存工作集的循环驱逐，并显著减少 32 次分配门造成的 required-caster omission。
该路径现在成为 Release 源码默认；开发 observer 构建仍可用
物理画面复查发现建筑/树木可能被上层静态身份误判；当 Warcraft 复用父模型
VB/IB 时，直接绑定会把后续写入暴露给阴影 replay。该路线已重新冻结为仅开发
构建显式 `DXVK_WAR3_STAGE11_DIRECT_STATIC_SOURCE_MODE=1` 可达；Release 默认
使用有序 copy 写入 WarVK 自有 Stage11 snapshot page。离线 generation proof
不能替代动画子部件的物理不变性证明。

这不是恢复旧的跨帧 VB/IB fingerprint cache。准入仍要求：

- 当前 draw 被证明为 rigid/static，动态单位、蒙皮与动画附件不进入；
- position/index 拥有有效的 buffer identity、allocation/content generation；
- map/device epoch 与当前会话一致；
- capture 保存具体 allocation 的强引用，P0 replay binding 在 CS 线程解析并由
  CSM、terrain mask、volume sun、point shadow 与 outline 跟踪到 consumer fence。

任一证明失败继续走 exact copy/Arena。动态 UP 直绑仍是开发态功能，Release
没有启用。

## A/B 证据和边界

同一 300 秒低视角巡航中，开发态静态源共享把静态缓存驱逐从约 4.4 GiB/8373
项降到 0，并把 producer-incomplete 帧从 283 降到约 67。平均 Arena 仍约
75 MiB，说明剩余压力主要来自动态/地形来源，不能用静态共享掩盖。

该数据来自隔离桌面，只证明相对稳定性和机会率；尚不能视为玩家前台视觉或
绝对 FPS 验收。后续必须继续通过完整构建、定向合同、高压长门以及用户物理
画面确认。
