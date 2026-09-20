# 2026-09-15 — 可复用逐帧证据记录器，第一阶段离线闭合

## 状态

已完成CPU/control骨架及元数据/单次截图关联接线，source-consistent DLL可构建。
**完整裂缝记录器未完成，尚不能让玩家重新捕捉样本。** 本轮没有游戏实机、部署、
覆盖玩家DLL、关闭编辑器、改地图/MPQ或发布。原版模型灯路线继续搁置。

## 新能力

- 独立纯CPU ring核心，Idle/Armed/Triggered/Frozen；75%事件前窗+25%预留后窗，
  严格session token、容量/序号边界、显式discard。单位是事件容量，不虚报固定秒数。
- 默认关闭`DXVK_WAR3_FRAME_EVIDENCE`，控制端arm才分配事件缓存，无新增后台线程。
  采集只try-lock；争用计数，不阻塞渲染，不写盘、不做JSON/资源hash。
  已冻结数据由现有pipe worker CreateNew导出；重复路径、坏会话、错状态被拒绝。
- Present、pipeline/pass开始结束、完整相机矩阵bits、ring-slot与业务frame分离记录。
  阴影reconciliation在公共Run入口清零后用scope-exit记录，覆盖早退/常规/测试调用。
- 独立casters开关默认0；最多512个normal finalized INPUT，元数据和backing分类型保存。
  rawcode与geometry hash分别存储，64-bit offset/size不截断；禁用/截断显式记录。
  这是提交前输入，不是每级联实际draw证明；fallback、完整palette/纹理/动态字节仍缺失。
- 异步截图保留请求session，prepare/CS记录copy+signal/save三个事件分开；旧请求不写新会话。
  Present end引用截图服务实际ordinal，不创建一个只在录制时递增的新“伪Present”计数。
  CS复制事件携带明确观察的frame/epoch，离线关联不能按+1或最近时间猜算。
- schema2明确区分`data`的uint64字符串与`words32`类型化位数据；schema1旧CPU归档只读兼容。
  reader递归拒绝重复键，校验事件连续、session/父子身份、容量、QPC与有限字段集合。
  可控截图关联要求唯一prepared/copy/saved、同目标值/尺寸/PID/owner及精确pipeline映射。

## 验证

- 198/198相关Python/static通过：148项原有相关门+35项reader/canary+10项新静态边界+
  5项前序像素测量测试。不是全库回归。
- 5-file py_compile通过：analyzer/control/runner及两份新Python tests。
- Win32共享Ring核心重新构建并执行：39,984次断言检查通过；包含2,500轮保留窗参考对账，
  不是39,984个独立测试场景。
- Win32真实runtime/control实现测试：gate-off3项，gate-on27项通过；四线程总10,000次
  append在最后测试中accepted2,098+explicitLosses7,902=10,000。故意的最大竞争压力，
  **不是实际游戏丢失率或低开销证明**。冻结、CreateNew重复导出、旧scope跨重启均覆盖。
- 上述Win32测试实际导出的6-event schema1及schema2文档均通过严格读取；最后DLL采用schema2，
  Python synthetic截图canary及schema1兼容均通过。真实DLL游戏输出尚未验证。
- exact DLL数轮局部构建通过：初始Meson重生成及4对象/link，后6-edge、3-edge；审查修订后
  9-edge，最终schema2独立2-edge。全程BelowNormal、最多-j2，无并发游戏测试。
  最终exact no-work；PE32/pei-i386/i386。
- 最终DLL：35,562,502 bytes /
  `E21DCACE7E7A756C94CB3BFBB9BD71F11BFAAFFDDF5977C3033BC35EA7D33541`。
  冻结前中间产物4E74/F2D0等未部署，也不作为最终身份使用。
- `git diff --check`通过。原图没有重复打开；没有改动原4张PNG。

证据根：`AutoTest/artifacts/frame_evidence_foundation_20260915/`，保留各次build日志、
核心/runtimes exe和CPU-only导出。保留的schema1 runtime-v2测试实例：
`WarVK/Log/FrameEvidence/cpu-13004-4310302729541-1.json`。
最新runtime-v3实际schema2运行导出：`cpu-36868-4317910159398-1.json`（2,507 bytes）；其SHA为
183814FF7A8190144BB04689F9AEE70AB04FC8EB6735A36094C13EA949422910。

## 当前实机阻挡及下一步

多次只读确认`worldeditydwe.exe` PID38544仍运行；未终止用户编辑器，未越过零进程准入。
`run_frame_evidence_gate.py`已准备为一次隔离2560×1440 canary流程（默认仅preflight，
显式apply才执行），但未运行。它冻结当前地图副本、核精确candidate/live/player/Game/map，
使用既有保留原生句柄/隔离桌面工具，验证envOverrides/effective双矩阵，关闭原生自动灯
实验，单张异步截图与事件关联，正常退出并三重身份恢复。它不是随机裂缝搜索或FPS测试。

玩家与测试目录DLL均仍35,486,415 /
74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73。
编译进程已结束；本轮未持有游戏事务或GPU租约，不存在要替玩家恢复的部署。

下一步先让canary在真实DLL中证明事件/图片一一关联。之后才接GPU环形历史、可配置手动
触发/启发式、完整动态输入字典、CSM/depth/vis/fog多层图和burn-in。第一阶段的单次截图
关联不等于已实现这些能力。根`captureComplete`/`rootCauseReady`恒false，缺失能力显式列明。
新版schema字段与GPU复制一手依据见`../research/2026-09-15-frame-evidence-schema.md`。
