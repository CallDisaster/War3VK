# v1.22.00 RC1 正式配置玩家复测交付 — 2026-09-20

## 结论和边界

用户要求发布配置 DLL 做最后一轮玩家测试。本轮交付的是 **正式配置 RC1，不是已发布稳定版**。
没有自动替换游戏 DLL、启动游戏、修改 War3.exe、提交本轮源码、推送、打标签或发布。
新组合的游戏/GPU/视觉验收仍待用户完成；旧 FAC 玩家反馈不转移到本包。

## 源码与配置

- 主树：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`。
- 分支：`codex/v1.22-main-tree-20260920`，HEAD `a5c42c050b49c48a7ccb392b69ea5d629b467129`。
- 本轮版本/构建修正尚未提交；package 的 pack.json 明确记录工作树增量，不把 HEAD 冒充完整构建源码。
- 独立冻结目录：`E:/WarVK-Builds/v1.22.00-rc1-20260920-r2/source`；3911个输入逐文件冻结于 `source-final-2.json`。
- 依赖从锁定gitlink提取；不使用主目录旧build32、StormBreaker脏文件或旧未跟踪MinHook二进制。
- 产品build32-product：release/O3、debug=false、b_ndebug=if-release（实际有DNDEBUG），重型帧取证默认0，skin palette correctness合同保留，其余开发/observer/culling实验关闭。
- 独立build32-tests：assert开启（b_ndebug=false），不把产品NDEBUG消除assert后的零退出当测试通过。
- 版本资源/JAPI显示1.22.00；PE PRERELEASE/RC1。Shader ABI 1.2.0及JASS wire v1不改。
- 未完成Water、自动模型灯和x64渲染产品不纳入；现有作者显式点光接口保留。

## 产物

| 项目 | bytes | SHA-256 |
| --- | ---: | --- |
| Desktop/WarVK-v1.22.00-RC1-20260920/d3d9.dll | 31,213,491 | 62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3 |
| Desktop/WarVK-v1.22.00-RC1-20260920.zip | 7,265,923 | F9BD285B2CC7D0E93C29196892F77A48B93BA718F962382066F239CD980F7DBA |
| build32-product/src/d3d9/d3d9.dll（未剥离） | 36,051,099 | 2499810D6E1B9EF987C771FAAA1655275C8CEDF73D76D528C7AEAD6FBD215098 |

Desktop指 `C:/Users/Administrator/Desktop`，build目录相对上述r2目录。
仅新包副本执行strip；PE32/i386、33导出/20导入及整个导出表哈希与未剥离版一致。
7个包成员：DLL、README、VERSION、CHANGELOG候选稿、原始许可证合辑、pack.json和manifest.json。
严格包审计覆盖成员穷举、size/SHA、PE、ZIP CRC及解压成员原字节一致。
包审计的buildConfig是自声明；独立配置审计与构建日志另列，不能仅凭manifest证明编译选项。

## 最终离线验证

- CPU Meson **93/93**；84个测试EXE从本轮源码编译，首轮共221构建步骤。
- 静态脚本 **273/273**；定向配置20、包25、源输入4通过，源输入4已包含在273中。
- 6个相关Python文件py_compile、git diff --check、exact DLL no-work通过。
- 产品配置审计ok=true/errors=[]；包审计ok=true/errors=[]，全部接受标记false。
- 构建父进程BelowNormal、最多-j2，产品/测试串行占有编译资源。
- 没有本轮实机、GPU、性能或视觉结果；尤其不宣称所有阴影撕裂、跨地图或API已修复。

证据均在r2目录：`configuration-audit-final.json`、`build-dll-final-2.log`、
`no-work-final.log`、`meson-tests-final.log`、`build32-tests/meson-logs/testlog.json`、
`static-accepted/results.json`、`package-audit.json`。

## 失败与纠正（不追认）

1. 初次独立Meson setup因缺失recorder模板失败；补回B原始模板并加源输入守卫。初次目录原样保留。
2. 首次461-step产品构建链接失败：旧MinHook静态库未跟踪。现用锁定MinHook的buffer/hook/trampoline/hde源码构建，不复制旧库。
3. 首轮静态265/273：7项原始SHA失败源于Git换行转换，另1项测试EXE缺失。恢复原字节并加.gitattributes，不改冻结SHA。
4. 首次测试目标列表误把Python解释器当Ninja目标，立即失败；改用Meson test依赖到exe目标映射后构建。
5. 第二轮静态270/273：新快照缺git历史基线及4个模块仍受换行转换影响；补只读本地历史元数据、恢复字节、重编相应目标，再最终273/273。
6. PE导出解析首次不适配当前objdump格式，在生成包前拒绝；适配观察到的格式并以整个导出表哈希补核后打包通过。

旧失败日志及中间输入清单保留。没有放宽检查去获得通过。

## 玩家复测与回退

交付时游戏现场仍为 `E:/Work/Warcraft III/d3d9.dll`：36,412,825B /
`FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529`，本轮未改动。

完全退出游戏后自行备份并替换；用普通启动方式，不用历史强制诊断启动器。建议2560×1440，同地图/画质，新进程每图：

1. 大地图多地点、低视角、离开再返回，检查阴影持续显示及压力解除后的恢复。
2. 多单位移动/俯仰并开体积雾，检查Issue #8短时异常拉长与合法对象持续投影。
3. Ctrl+F1 Render Stats数据；性能录制关/开帧率；原生截图和正常退出。
4. 有条件再验太阳JAPI、局部雾与Froxel组合。

失败保留报告/截图/地图与操作顺序，退出游戏后恢复自行备份的DLL。无需先强开重型取证。
正式发布前仍需用户对本组合的评估及作者包范围收口；稳定根CHANGELOG保持未改。
