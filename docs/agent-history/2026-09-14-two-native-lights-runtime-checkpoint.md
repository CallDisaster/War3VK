# 2026-09-14 — 双原生灯候选与混合场景尾段修复

## 结论与适用范围

用户要求解决导入灯不参与、点阴影开头出现后消失，并提高测试上限。
候选将自动原生灯接管/点阴影从每帧1盏提高到2盏，显式路径注册优先于内置规则，
同级按投影中心距离选择。仍需真实当前draw、实例代际及完整发布证明；不会凭路径猜灯。
作者显式创建灯的总预算仍为16灯/4个阴影cube，自动灯共享其中的剩余容量。

已修复已证明的选灯优先级和`lit-draw-outside-world`过度拒绝：world结束后，符合原有
FFP/资源/覆盖证明的draw将相同源颜色传播到独立颜色端点，不再无条件撤销整帧。
尾段不认领新灯，不移除未知光照；所有其它不支持状态仍安全回退。
两灯各自可见度使用原色/去A/去B/去AB四个独立端点，不能用饱和颜色直接相减替代。
公式和GPU资源边界见`../research/2026-09-14-two-native-lights-and-tail-transport.md`。

## 构建与纯离线验证

- authoritative树：`dxvk-v1.22-integration-20260914`，公开v1.21独立集成线；未合入Water。
- exact DLL构建58/58，父进程BelowNormal，总并行-j2，exit0及exact no-work。
  日志：`AutoTest/artifacts/v122_native_lights_20260914/two_light_build_r20/build.log`。
- DLL：PE32/pei-i386/i386，35,486,415 bytes，
  `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`。
- 148/148定向Python/static复跑通过；包含新11项端点代数/饱和反例/优先级/尾段/布局检查。
  旧native-light事务检查按2灯/3私有MRT更新，身份/覆盖/原色/回退门未删除。
- 三文件py_compile：multi-endpoint test、mixed-map builder、native-light gate runner。
  新构建Win32`registration-core.exe` exit0；这是synthetic解析/路径测试，`real=0`。
- 生成receiver SPIR-V 166,620 bytes及R23实际dump的10个SPIR-V全部
  `spirv-val --target-env vulkan1.3 --scalar-block-layout`通过；后者含5个FF VS/FS。
  不是32位Vulkan validation layer实机无警告证明。
- `git diff --check`通过。没有提交、tag或发布。

## 实机对照与证据边界

复用玩家原地图和实际ED122导入资源的冻结副本，只插入测试JASS；不改原地图/MDX/MPQ。
保留原场景灯，注册导入火把与原版TorchHumanOmni，在其附近增加两个无敌步兵。
四阶段：两灯阴影开→关→开并换选中单位→保持开并再次选择。
选择操作由地图JASS执行，不使用系统全局输入。每阶段相隔25秒，最终再保留数秒观察。

所有文件前缀：`AutoTest/artifacts/v122_native_lights_20260914/`。

| 观测 | 旧3B0B / native_mixed_old_r22 | 新74CC / native_mixed_two_r23 |
| --- | --- | --- |
| fresh process | 1 | 1 |
| JASS四阶段 | 通过 | 通过 |
| color fallback抽样日志 | 78 | 0 |
| receiver fallback抽样日志 | 0 | 0 |
| receiver commit抽样日志 | 9，后续失去接管 | 86，持续双灯/12面 |
| 最后截图后仍有双灯/尾段租约 | 不通过 | 通过 |
| isolated 2560×1440 / global input | 通过 / 0 | 通过 / 0 |
| exact native exit /恢复 | 0 /通过 | 0 /通过 |
| 新GPU事件/dump | 0/0 | 0/0 |

抽样日志条数不是实际提交帧数。worldSerial与receiver frame不是可任意相减的同一时钟。
最后截图证据采用日志顺序：`[AsyncScreenshot] saved id=4`之后仍出现两源选择、
`tailDraws=1`及`automatic=2 cubeLights=2 cubeFaces=12`，不是以开头一次提交判通过。
R23最后receiver抽样frame=10321。阴影关闭阶段会让其它内置灯获得空出的槽，不能把
那一阶段的automatic=2误读为两盏已禁用阴影的火把继续投影。

R23末段实际光点：

- 导入资源policy=4294967295，generation16，(-58.1924,163.395,150.935)，range600。
- 原版Omni policy2，generation17，(295.873,177.994,76.2476)，range200。

这些是runtime位置，不只是读到MDX的本地Z。没有把旧32059406模型遮挡证据转移到ED122。

已看旧R22 stage-3、新R23 stage-1与stage-3三张视图；没有为恢复上下文重复看图。
新stage-3导入火把附近可见持续投射的步兵轮廓，与stage-1阴影关的照明有明确区别。
右侧原版火把仍有强烈角状遮暗；本轮没有证明具体哪个三角形造成该区域，不能称全模型
阴影视觉已解决。继承的policy8精确发光体排除没有扩大到policy2/任意导入模型。
这不是锁定动画/时间的逐像素A/B，也不是前台FPS基准；3个私有MRT增加带宽和shader开销。

## 精确身份

| 文件（上述证据根目录下） | bytes | SHA-256 |
| --- | ---: | --- |
| native_mixed_old_r22/receipt.json | 30,812 | 66006F3C7046FB2B0DFA45388816541FD045F35043422F13C509B4DA51799ADA |
| native_mixed_two_r23/receipt.json | 41,129 | E2FA05598F9539F718A2A55020870E662B341D37E0CD14D64DF8B974529FE3B2 |
| native_mixed_two_r23/after-11-war3_d3d9.log | 128,346 | 91E1EAD4DC3E8EE9C717152B6C4D237AFAAF8F95613D34F08B4CB74419A5BD67 |
| native_mixed_two_r23/stage-1.png | 4,374,905 | 805F37E3342FC2E317EBF5FA83EE4597311147EB42A4A51EAB720152002D1652 |
| native_mixed_two_r23/stage-3.png | 4,406,565 | AF2932FEE559647BF96F2EA7F674A9B20EAD51F217FF0F5427B8FE2AFF7789F5 |

原地图5,859,965 /DF4482EE2A585CB4FE0814D9D199A4E3ABA59B5C7EA6CEAF0B5158F3718746F4；
原MDX4,351 /ED12243B5560E39BD118DF6591738EDF22C2131DA575E8202C62ADB4EF749C3A。
两者收尾未变。R21/R23 fixture地图各5,879,834，SHA分别
265CF063BB7FB778A7F4C44857B636BD4A81CC6D1C3CF96E734EE5C491548007 /
F9DBA25BBCEA3D52A9FB9975C1D4E27D45F538C006D266D1E5A4FF3CDBFE7632；
同一JASS harness，仅receipt文件前缀不同。

## 准备失败、恢复与交付边界

- 最初发现用户worldeditydwe进程，按零进程门停止，没有关闭编辑器；随后用户退出，复核零进程。
- R20 fixture准备因JASS定义顺序pjass失败，未运行；修正为已有API定义之后、main之前。
- 测试现场实际上已是3B0B，旧A570默认身份门停止；明确参数化当前baseline/player身份。
  R21又由一处仍硬编码A570的pre-deploy门停止，runCount=0，没有部署/游戏。失败工件保留。
- runner现仅从本次runCount=1、时间有效的精确`E:/Work/War3/war3_d3d9.log`统计，
  不再从其它旧日志借用提交。R21失败receipt里旧实现带入的统计不属于实机证据。
- R22/R23均结束并关闭隔离桌面。测试目录恢复为35,476,894 /
  3B0B548A5BE9573005424D31FCD62B36B419C59B8C10D343F209B60EC957452A；
  玩家`E:/Work/Warcraft III/d3d9.dll`同一3B0B始终未改，绝不恢复成历史A570/B1FCC。
- 收尾游戏/编辑器/编译/实际runner均零。编译资源已释放；游戏资源已释放。
- 候选CreateNew打包到独立Desktop目录，不附带用户模型，不自动部署、不覆盖旧包。
  当前`productAccepted=false`、`pointShadowVisualAccepted=false`；待用户前台验证、
  原版灯遮暗几何定位、更多模型/相机、长时/跨图及性能门，不能据此发布v1.22稳定版。
