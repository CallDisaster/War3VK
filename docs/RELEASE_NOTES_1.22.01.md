# WarVK v1.22.01 — 内存维护修复

本版在v1.22.00基础上改善内存预算、快照空间复用及分配失败清理，不新增视觉特性。

## 主要修复

- **动态扩页额度**：结合实际显存预算、物理后备占用和32位地址空间余量，压力大时停止继续扩页；不回收GPU仍在使用的数据。
- **快照空洞复用**：解决同类小切片长期占住整页可用空间的机制问题。最后使用者释放后即可复用对应区间，不必等待整页全部过期；极端碎片与元数据耗尽仍有保守回退。
- **Arena渐进回落**：已退役代际的空闲尾页逐步缩减，减少长期保留峰值容量。驱动缓存释放可能滞后。
- **失败路径清理**：补齐图像创建/登记失败的清理，修复D3D9分配器小尾段丢失及零长度/溢出边界。
- **截图按需分配**：没有截图请求时不预热三槽读回缓冲；任务完成后可归还空闲槽引用。
- **统计解释完善**：Render Stats补充最近扩页额度、切片复用与预算拒绝次数。

快照池在可信驱动预算下默认最多512 MiB、无可信预算时回退384 MiB；显式上限仍生效。
Arena保持每代384/总1152 MiB硬界限。它们不是预分配量，也不是整张显卡的显存上限。

## 安装与验证

普通玩家下载 **WarVK-1.22.01-win32.zip**。完全退出游戏/编辑器，备份旧DLL，再将新DLL放到
`war3.exe`同目录。作者可下载author-kit。仅支持Warcraft III 1.27a / 32位；无需修改游戏EXE、地图或MPQ。

对应候选已获用户单轮试玩“未见问题”的反馈；正式版本重新通过CPU99/99、静态脚本275/275和
配置/包检查。最终增量只更新版本与匹配探针、文档和包；没有重新实机，不宣称全GPU或长时验收。

仍建议每张地图重启游戏。极端工作集、同进程换图、所有历史间歇阴影异常仍需持续回归；
诊断跨attempt恢复归因及慢磁盘退出另列后续工作。不包含64位渲染器、未完成Water、自动原版模型灯。

Shader API 1.2.0与JASS wire v1不变。本版没有新增FPS提升百分比承诺。

---

Memory maintenance patch: adaptive pool growth budgets, safe retired snapshot-hole
reuse, gradual Arena tail retirement, allocation-failure cleanup and on-demand
screenshot buffers. CPU tests 99/99 and static scripts 275/275 pass; one matching
candidate received positive player feedback. Not universal GPU, cross-map or
long-duration certification. Back up your DLL and exit the game before upgrading.
