# WarVK 1.22.00 发布范围与证据边界

2026-09-20用户完成RC试玩、认可目前没有大问题并明确授权正式README/更新日志和GitHub Release；
随后要求先修复Render Stats显示。最终仅增加统计刷新/上一提交代读数及正式版本资源，不改变渲染预算、
caster准入、缓存淘汰或GPU回收条件。该显示改动必须重跑离线门，不把它假称玩家已经看过。

## 构建与源码

- 主目录dxvk、codex/v1.22-main-tree-20260920承接B-primary整合；只向origin/main和v1.22.00推送，
  不上传本地备份/实验分支，不force，不改旧标签。
- 固定gitlink依赖；StormBreaker工作目录的既有dirty不用作构建输入或提交。
- 产品release/O3、NDEBUG，D3D9 Win32；关闭internal frame recorder与全部未批准dev/observer选项，
  保留warvk_skin_palette_contract_candidate=true。Shader API1.2.0、JASS wire v1不变。
- 断言开启的独立CPU测试配置与产品配置分开。精确DLL目标、BelowNormal/-j2、no-work和PE核验。
- MinHook由固定C源码编译；正式使用的源码/配置清单与测试记录在本地冻结，不包含游戏资产。

## 资产白名单

- `d3d9.dll`：32位正式配置DLL，Release版本资源，无RC/PRERELEASE；剥离仅作用于包副本。
- `WarVK-1.22.00-win32.zip`：DLL、英文/中文README、CHANGELOG、许可证、第三方声明及发布manifest。
- `WarVK-1.22.00-author-kit.zip`：WarVK/action.txt、call.txt、define.txt、jass目录、atom图标、
  WarVK/README.md、MATH_CURVE_API.md及许可证/manifest。不包含DLL或Loader。
- `SHA256SUMS.txt`：上述独立DLL和两个ZIP摘要。GitHub另自动提供标签源码归档。

不包含MPQ/MDX、地图、截图原始证据、IDA数据库、dump、日志、令牌、个人会话或x64 helper。
旧RC包、旧失败证据与备份只留本地，不覆盖或删除。

## 验收口径

RC1有93项Meson CPU、273个静态脚本及配置/ZIP离线门；用户试玩后授权发布。
最终显示改动已重新通过93项Meson CPU、273个静态脚本、372项Render Stats断言、正式配置审计与exact DLL no-work。
DLL身份及最终包校验单独登记于发布回执，不转移旧哈希验证；最终显示增量未重新实机。
发行状态不是所有GPU、长期、同进程跨地图、间歇撕裂及109个API已全部验证的保证。
已知边界随README与CHANGELOG发布。没有本轮v1.21与v1.22正式版性能同比。
