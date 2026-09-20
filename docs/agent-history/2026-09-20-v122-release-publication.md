# WarVK v1.22.00 正式发布构建回执

## 远程完成回执（覆盖下方事务前状态）

2026-09-20T08:15:30Z（台北16:15）已正式公开；Release ID392359706、draft=false、prerelease=false、latest。
`v1.22.00`标签解引用为`0df497914c3352539f14fea2af40a600ea7b25fb`；该commit已以fast-forward推送origin/main，未force或覆盖旧标签。
四附件远端digest/size与本地完全一致：

| 附件 | 字节 | SHA-256 |
| --- | ---: | --- |
| d3d9.dll | 31,213,491 | `ED4BFAEEC775B6E2F088351A4F0F164609E23300056EECC16294171E9F4D8812` |
| WarVK-1.22.00-win32.zip | 7,267,655 | `F10C32878E72BD0CCC6F4AEF000D8BA2158676B96CCCC5CAEBA495B6C15679A8` |
| WarVK-1.22.00-author-kit.zip | 94,698 | `E23348CAB63659E9E12A9E8795629E9FC3BB1CFF5C4AFC178634115F621F38FB` |
| SHA256SUMS.txt | 260 | `CCCACE071E2322AEED205D799EF06B6493143506A36FCE568D18A43766CACD01` |

最终显示改动未重新实机，玩家目录仍是用户原先安装的RC1；没有自动替换。
本成功回执后续单独提交main；不移动标签、不改包内sourceCommit、不重打资产。

## 授权与验收边界

用户在正式配置RC1试玩后明确表示没有大问题，要求更新README/CHANGELOG并推送稳定Release；随后追加先修Render Stats显示。
本次没有部署、启动游戏或改写玩家现场。最终统计增量未再实机，不能冒充最终DLL的GPU/长时/所有API已全部验收。
用户接受的RC1为31,213,491B / `62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3`。

## 最终增量

1. Stage11 resident/used/reclaimed由真实存活页池重新填入每次scene重置；缓存命中但无分配的帧不再发布假零。
2. Arena新增上一提交代使用量，保留当前代瞬时值；容量栏明确为可复用的保留容量，不是帧用量，尚不自动缩容。
3. 无预算、caster准入、回收/fence或剔除策略变化；有界owner线程读取，不让UI遍历可变页表。
4. Windows资源1.22.00、PRERELEASE=false；Shader API1.2.0与JASS wire v1不变。

## 最终身份

| 产物 | 字节 | SHA-256 |
| --- | ---: | --- |
| 玩家DLL（剥离包副本） | 31,213,491 | `ED4BFAEEC775B6E2F088351A4F0F164609E23300056EECC16294171E9F4D8812` |
| 原始含调试符号DLL | 36,051,280 | `FC7EA415E222E0B16C1C27707779D25489AD86633B3B5FAE752E951E050C45A1` |

PE32/i386。剥离不作用于原始构建；正式包审计还要检查导入/导出不变及ZIP所有成员字节。
ZIP摘要由附件`SHA256SUMS.txt`提供，源码commit由包内manifest记载。
首轮预览/打包曾受strip当前时间戳影响（84190E1D/98BD6B28，仅PE时间戳/校验和4字节不同），均未上传。
最终使用--preserve-dates，两次独立剥离SHA相同；正式包另存r2，旧包原样保留，不覆盖旧证据。

## 本轮离线验证

- 精确DLL：50/50 edge，BelowNormal、最多-j2；再次exact target无工作。
- 独立assert开启CPU构建：Meson 93/93，无跳过/失败。
- 完整静态脚本：273/273；Render Stats生产辅助函数372检查/0失败，含100帧无分配复用、真实回收与清池。
- 正式配置审计ok=true；正式ZIP/PE读方8/8，py_compile、git diff --check通过。
- 常见凭据模式扫描：拟提交文件及17ecf66/a5c42c0集成历史零命中；生成物/原始证据/游戏二进制不进入发布。

日志仅留本地：`E:/WarVK-Builds/v1.22.00-rc1-20260920-r2/`下的release-build.log、release-no-work.log、
release-meson-tests.log、static-release/results.json、release-configuration-audit.json、source-release-1.json。
后续只更新文档/打包工具的冻结清单与构建清单分开，不重写已有receipt。

## 发布事务规则

本文件写入时远程发布尚未执行，不能把计划当成功。计划将最终commit及新`v1.22.00`标签以fast-forward/atomic推送至origin/main；
先建立draft上传白名单资产，核对远程摘要后再公开为stable/latest。不得force或覆盖旧标签。
成功状态以GitHub release及对应标签commit为准：
[WarVK v1.22.00](https://github.com/CallDisaster/War3VK/releases/tag/v1.22.00)。
本地最终事务回执保存到新建的`D:/WarVK-Releases/1.22.00-20260920-r2/`，不把环境变量/测试全日志上传。

## 保留边界

同进程跨地图、全部随机撕裂/崩溃、极端预算、所有API与所有GPU视觉门并未完全闭合。
未完成Water、自动原版模型灯、64位产品及未批准Consume实验不纳入。
StormBreaker原dirty、原A/B分支、stash、旧候选、玩家DLL和坏帧证据保留；没有删除或覆盖回退点。
