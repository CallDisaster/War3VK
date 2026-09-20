# v1.22.01 发布事务回执

## 当前状态：离线验证完成，远程发布尚未执行

用户单轮试玩后授权主线程选择继续优化或推送修复版；主线程选择冻结范围并发布补丁。
玩家现场只读哈希与交付候选一致：36,065,732B /
028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3。
未修改玩家目录，未启动游戏或自动部署。反馈不是全量运行矩阵。

## 冻结输入与离线结果

- 主目录dxvk，基于24140e4；固定依赖、用户StormBreaker原dirty保留且排除构建/提交。
- 构建输入 `E:/WarVK-Builds/v1.22-maintenance-20260920-r1/source-release-1.22.01.json`，2893文件。
  收尾文档不反向修改构建快照。运行时与已试玩r9差异仅版本/JAPI字符串、版本探针及对应测试。
- BelowNormal/-j2产品构建成功（含Meson版本重生成），exact DLL no-work；PE32/i386，
  1.22.01、PRERELEASE=false；产品player-release审计ok=true。
- 断言开启CPU99/99；全量静态275/275；配置测试20/20、实际正式包工具9/9、旧离线包严格读方25/25。
  py_compile和diff空白通过。版本发布不复用NDEBUG删除断言的测试结果。
- 未剥离DLL36,065,732B /
  D7A540926C02904E96F25ADE9D643B0E768B711CC41EE5A12EFC0A87400C1BC3。
- 日志位于同一build root：release-1.22.01-build.log、release-1.22.01-test-build.log、
  release-1.22.01-meson.log、static-release/results.json、release-configuration-audit.json、release-no-work.log。

## 事务约束

仅白名单源码与新v1.22.01标签fast-forward/atomic推送，不force、不修改旧标签或附件。
正式包副本strip --preserve-dates，核原产物不变、PE与导入/导出、ZIP成员/CRC/字节/SHA；
先draft，核四份白名单远端附件摘要后公开latest。公开之后再另添成功回执，不移动发布标签。
原始日志/游戏资产/证据/凭据/本地输出不上传。独立输出目录D:/WarVK-Releases/1.22.01-20260920。

发布范围及未覆盖项见docs/RELEASE_1.22.01.md；本文件的计划与本地绿灯不代表远程发布成功。
