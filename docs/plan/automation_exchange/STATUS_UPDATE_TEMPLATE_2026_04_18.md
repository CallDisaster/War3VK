# Status Update Template

每一轮有效推进结束后，按下面格式回写 `CURRENT_STATUS_2026_04_18.md` 或新增同目录后续状态页。

## Template

### Date

`YYYY-MM-DD HH:mm`

### What Changed

1. 本轮改了什么代码
2. 哪些文件被改动
3. 改动针对的具体问题是什么

### What Was Verified

1. 是否编译通过
2. 是否走了隔离桌面后台验证
3. 最新 control plane summary 是什么
4. 最新截图/报告路径是什么

### Current Result

必须明确写成四选一：

1. 功能前进
2. 性能前进
3. 路线无效
4. 功能前进但性能未前进

### Current Blocker

只写一个当前最主要的 blocker，不要写发散愿望清单。

### Next Mandatory Step

下一轮必须先做什么，要求足够具体，不能写成空泛目标。

## Example

### Current Result

功能前进但性能未前进。

### Current Blocker

单位当前帧 dynamic geometry contract 仍不完整，阴影形状错误。

### Next Mandatory Step

继续逆向 `meshData` 的 index/topology/auxiliary stream 语义，并在后台验证单位阴影是否从方块/撕裂变成正确模型阴影。
