# 2026-10-10 天气查询时钟审查与小范围共存/启停压力门设计

本批只读生产、canary-c保存的证据与当前测试脚本，只写本设计；
没有编译、运行游戏、修改API/production/测例或修改玩家目录。主线程决定后续实施及唯一实机lease。

## 实施与 stress-a 复核补记（保留旧失败，不追认）

以上是初始设计批次的边界；本补记对应主线程后续授权的测试实现和离线复核。
本 lane 仅修改独立 JASS fixture、纯 Python contract/tests 与本设计文档；
没有改生产天气代码、编译 DLL、运行游戏或覆盖任何玩家文件。
实际实机 stress-a 由主线程执行，以下结论来自其冻结的原始回执，只读投影，
并非本 lane 重跑了实机。

已实施的固定压力套件**不包含下面原始阶段 C 的尺寸请求**：
启动25s，四个共存阶段各30s，32轮on4s/off2s，8次销毁/新建实例，销毁尾段60s，
预计397s，runner硬上限600s；结束标记stage69，共70份JASS回执、1083次明确检查。
active/coexistence阶段至少3个不同实际业务帧；off至少一次真实停止消费，
不要求关闭后的诊断持续逐帧更新。Destroy/Create实例不能认证GPU资源重建。

### stress-a 的真实失败类型

冻结源：

- AutoTest/artifacts/weather-night-20261010/stress-a/run.json
- 2,111,435 bytes；
  SHA-256 9CF863C2826C0E704A37FF9835705EDF2EC37D2F3CCF59B89635950CB996B88C。
- 原结果为exit2，70段每段恰好新增1次JASS错误，最后checks1083/errors70。
  首段原文唯一失败为 WEATHER_STRESS_FAILURES=ordinary-postfx,；
  后续累计字符串因JASS字符串长度截断，不能当作完整无限日志。

确定的机制：既有 WarVKSetPostfxEnabled 包装调用 postfx.setEnabled，
spec声明feature 0x40，但 kImplementedFeatureMask 明确不包含该族，实际dispatch也没有该case；
请求被 UnsupportedFeature=18 拒绝。**不是天气API未收到请求，也不是旧ID污染了新实例。**
不能只添加feature bit便声称完整postfx控制已实现。

原 newInstanceParamsSurviveOldIdRefusals=false 依赖 _rows_ok(errors==0)，
因此与上述70次JASS错误一起失败。独立检查8个重建阶段10/18/26/34/42/50/58/66：
每阶段5份夹持样本，共40份；runtime和frozenInput分别检查
snowfall=.65、windAngle=30、windSpeed=160、gust=.4、haze=.45、lowSnow=.5，
**480个标量比较中0次偏离**（有限值与1e-4容差）。
旧ID1..8分别被拒，新公开ID2..9单调，8份旧写拒标记均true。
这支持限定样本中“旧写拒后参数保持”，但不能把整轮JASS验收改成通过。

### 新测试契约：显式验证未实现族拒绝

保留真实 WarVKSetPostfxEnabled 调用，随后保存并检查实际错误码必须为18，
标签改为 ordinary-postfx-unsupported-refused，再明确clear；
每份回执新增 postfxRefusalCode，读方拒绝缺失、错误码或错误类型。
成功误返回0也必须失败，不删除调用，不放宽其它JASS错误为0的门。
累计checks仍为1083。

这只是纠正测试对旧未实现API的预期；**不认证postfx总开关切换，也不宣称修了生产天气。**
Sun和普通High体积雾仍使用已实现API设置/回读，并按其实际同域提交单独验收。
旧stress-a缺少新字段、errors70，保留原始失败，只有新目录的新实机可以验收新契约。

### 读方反例修正与旧run离线回评

只读审计找到三项真实误通过，本次已以实际contract调用反例修复：

1. stage34前三份好消费掩盖第4份Snow backing增长，同时第4份draw.frameSerial滞后，
   旧active过滤丢掉该byte证据。现在同owner/map/device/已知尺寸的current backing
   独立于“是否与frozen同帧”校验，不能因draw滞后隐藏资源增长。
2. 所有active render.resourceBytes=None，旧读方以默认0回落仍判稳定。
   现在已知字节必须是非负typed整数；active资源必须为正，None/bool/负数/字符串/浮点不能冒充known0。
3. stage34第4份当前实例frozen.params.snowfall=.2，旧phase过滤掉该坏值，
   仍用前三份好值认证旧写拒后未改。现在已明确命名当前实例的冻结参数必须逐份正确，
   不允许筛掉反例后认证其不变。

合法CPU/render异步样本仍不能充当同帧消费证据：active有效帧计数继续复用真实frame_alignment，
不读保存的“alignment=true”替代实时字段；32次off、3个不同active帧及8次旧写拒门均未放宽。

新contract离线重新分析**同一份未修改**stress-a：
allStressJassStagesAndChecks=false、newInstanceParamsSurviveOldIdRefusals=false，
其余8项限定机器门观察仍true。五项能力未认证字段仍false。
这是新读方对旧证据的独立回评，**不是追认旧契约或新实机总PASS**。
新contract SHA-256：
D4CB870F57CF66507F36CC8AF1DF4086BB6A22821BD9E19A941EF5F9648496D7。
纯Python tests **39/39通过**，没有在本lane做JASS编译或实机。

allSevenCapturesPhaseBracketed只验证status前后阶段夹持，不单独认证截图文件path/SHA/size
或截图精确天气业务帧；文件有效性由主线程launcher真实BMP校验与独立视觉审查负责。
普通雾当前资源账未提供，GPU完成/像素艺术验收/进程泄漏/GPU资源重建均不由这些机器布尔认证。

## 时钟异常：可排除什么，不能声称什么

本节保留最初的canary-c审查记录，不用后来的API修正追认旧异常。当时未定位的carrier问题，
后续证据与修正见2026-10-10-weather-query-investigation.md及主线程原版JASS回归回执。

真实canary-c原始证据为 `AutoTest/artifacts/weather-night-20261010/canary-c/run.json`，
及该canary独立复制的 `readback-war3map.j`。只读提取各stage的receipt与status得：

| stage | JASS receipt秒 | 该stage第一个→最后一个live runtime秒 | historyRevision |
| --- | ---: | ---: | ---: |
| 10，已关闭并编辑时间线 | 17.999 | 17.9990216→17.9990216 | 4 |
| 11，PlayTimeline(0) | 9.730 | 0.7172398→7.7178683（10项） | 5→5 |
| 12，仅查询播放进度 | 8.015 | 8.4974155→15.6149219（10项） | 5→5 |

stage11前两次render/冻结输入分别0.7160122、1.4940110；live runtime相应0.7172398、1.4955238。
这部分约1–2ms的owner领先render是正常采样顺序，但**不能**用它解释JASS9.730与后续0.717的反向跳变。

生产 `WeatherRuntime` 的线程和时钟路径：

- `steadySeconds()`（runtime.cpp约45行）使用 `steady_clock` 的double秒。
- `advanceLocked()`（约199行）在同一mutex下以now-lastClock结算，再更新lastClock；
  有限性和倒退防护会拒绝错误采样，不增加负时间或重复经过的墙钟。
- `playTimeline()`（约347行）取得mutex、advance当前时钟、设置timeline/enabled、seek(0)。
- `getTime()`（约396行）同一mutex、advance、返回当前float秒。
- `snapshot()`（约405行）同一mutex、advance、按值返回当前状态。

没有额外seek/reset时，多线程query/snapshot交错只会把同一时间区间切成若干段，
时间增量总和是终点减起点，不会因为采样次数而双计。假设stage11 Query真的返回了runtime9.730，
随后相同history=5的snapshot不可能降回0.717。原脚本stage11→12没有另一个Play或Seek；
能重置时间的create/play/setTime/reset/destroy都会改变history身份，保存样本没有这种变化。

因此本次源码与证据已经推翻“纯CPU正常异步领先”“Play后的真实9.73s阻塞”“snapshot频率双计”
这三种单独解释。**尚未定位根因**：JASS query carrier、VM返回值签名/缓存、receipt取值和边界时间需要
由主线程/API lane继续检查。不能因为大多数stage查询一致就容忍该反例；CPU离线时间线测例也不认证JASS读方。

建议后续诊断给Play/GetTime加有界同线程次序见证（request序号/id、开始与结束QPC、
runtime返回float位和字符串）；JASS receipt在一次tick中分别记Play前/后GetTime和对应错误，
与同frame的内部runtime身份对照。直到typed carrier回执闭合，独立启动天气计时验收不得给总PASS。

## 新测试的共同边界

- 独立canary复制地图和候选DLL；不覆盖玩家DLL/Game.dll/原地图，不写系统配置或注册表。
- 单一 owned 隔离desktop、2560×1440初始client，不发送全局鼠标/键盘；摄像机由JASS定时控制。
- Sun/ordinary fog/weather参数与错误码全部由真实JASS API设置，并保存raw receipt，不由宿主直接替换runtime。
- 读真实 weather-status：live runtime仅作者状态；render/volumetric与frozenInput要求同frame/map/device。
- 截图严格phase前后夹持；只是phase绑定，不声称截图精确天气帧或GPU完成。视觉仍独立审查。
- 所有计数门和自洽反例先离线验证classifier，不得空样本all()通过或隐藏早期error。
- 各阶段至少3个有效同域消费样本，sample不足/字段不存在标未覆盖；异常不是“没有崩溃就绿”。

### 总时长上限

一轮设计预算：准备进入场地30s + 共存120s + 启停192s + 尺寸请求120s + 尾段60s + cleanup≤30s，
共552s。实际launcher须有600s硬上界，超时停止本owned实例并保存失败与恢复证据，不延长成无界挂机。
构建/地图制作不占这段游戏运行预算；也不能以启动慢为由吞掉cleanup时间。若夜班截止不足，先做更小的单项门。

## 阶段A：Sun + ordinary fog + weather的小范围共存（120s）

场地沿用已知模型/地形，固定相机，普通sun/雾保持相同设置，天气区域覆盖当前小场地，四段各30s：

1. 普通Sun+ordinary global fog、天气关闭，建立普通介质基线与资源规模。
2. 同一sun/fog不改，开启雪粒子+haze+lowSnow，天气强度固定中等，取得普通介质与独立Medium同帧提交。
3. 仅关闭ordinary volumetric medium，保持sun与weather；证明独立天气仍提交，而普通介质明确disabled。
4. 恢复ordinary medium，天气继续；证明原设置恢复后与天气重新共存，不因交替High/Medium反复重建。

机器门：

- 每段所有JASS请求错误为0；create positive id、同段active id不变、pause false。
- 普通介质enabled段至少3帧ordinary compositeSubmitted、独立天气段至少3帧weather compositeSubmitted；
  这些证据要同frame/map/device，不拼接两个异步不同帧的成功标记。
- 关闭ordinary段其disabled真实可见，但Snow和独立Volume继续提交；天气off基线不虚称其旧diagnostic已清。
- 固定目标下snowfall/haze/lowSnow和区域符合freeze值；原sun/fog的author设置在stage2不变，stage4回到原值。
  当前status若不暴露这些普通设置，必须给出可审查设置回读，否则“配置互不改写”标未覆盖，不能用截图推断。
- 资源当前backing在相同尺寸/格式连续观察下稳定，不每次ordinary/weather交替都改变尺寸/增大；
  weather固定Medium、ordinary选择的High保持各自owner，不能只观察总值再猜两者隔离。
- DeviceLost/GPU fault/新增fatal dump/producer required caster遗漏计数增长为0；若原地图本身有基线缺口，
  必须明确baseline并判增量门，不把已存在缺口消失或fail-visible当“完整投影”。

截图至少基线、共存、仅天气、恢复共存四张；独立视觉看雾色与雪可辨识、建筑遮挡和地面/单位不被错误洗白。
机器提交门不认证光学画面正确。

## 阶段B：32轮启停/实例重建（192s）

一轮6秒：enabled4s、disabled2s；固定同尺寸同格式，每4轮做一次destroy→create新id，共8次，
剩余轮只SetEnabled off/on。每次新建设置同seed/区域/参数然后Play；先保持普通介质关闭的only-weather调度，
后半可恢复普通介质作为共存压力，但两类样本要分组，不能混合统计。

机器门：

- 当前id活跃，每次重建public id与旧id不同且单调，不额外多实例；旧id查询alive false。
  旧id写入测试必须收到预期拒绝，并核实当前新id全部作者字段不变；错误被明确消费再clear，不混成零错误绿灯。
- 未seek的enabled阶段live时间单调；paused/disabled阶段稳定容差0.08s。
  Play(start0)的Query/receipt与首消费输入要能解释0附近时间，以原始QPC边界给定上界；
  若出现canary-c的9.73→0.717模式立即fail，不认作正常CS滞后。
- only-weather关闭后drain真实发生：至少1帧Snow disabled与Volume disabled同域记录，命令提交false；
  后续不再需要每帧BeforeUi，不能以持续空Pass证明零关闭成本。诊断停更后应保持最后关闭态而非旧commands true。
- Snow depth-copy若在本target使用，off后当前copyBytes应归0；Snow pipeline/sampler小缓存允许保留。
  Volume当前图像/UBO保持一份是当前设计，off不要求归0，后续enable稳定回到同一current resourceBytes。
- 至少在8次实例重建后有效Snow/Volume提交重新出现，不仅API alive true；新历史身份不得沿用旧seek/旧实例。
- 每轮固定强度下activeParticles等于CPU可见估算，capacity不增长；不冒充真实屏幕存活粒子数。

## 阶段C：独立客户端尺寸变更请求（120s，12段×10s）

不能把Destroy/Create叫“重建GPU图像压力”：生产保留同尺寸Volume缓存，Snow直接采depth时也可能不新建图像。
真正触发extent相关资源替换，要走现有owned实例client resize能力，不用全局输入/注册表。

尺寸序列建议1920×1080→2560×1440→1920×1080→2560×1440，每4段一组，共3组，
第1组当warmup，后2组是重复同key压力；最后必须回2560×1440。
如果现有owned resize不能使真实渲染extent改变，停止这项、标未覆盖，不增加隐藏env或伪造输入extent。

机器门：

- 每段实际color/depth extent、weatherVolume执行width/height与client resize目标一致，
  新尺寸有至少3个有效消费提交；不把仅窗口外框改变当GPU资源重建。
- extent变更后资源失败可见：allocationFailed或retryWait必须被记录，120业务帧后同key获准重试；
  新extent/map/device key不能被旧key的冷却永久锁死。没有真实allocation failure时该分支标未覆盖。
- 所有恢复提交color/depth复制与draw同域，未进入UndefinedLayout/UnsupportedTarget粘滞失败；
  extent回到旧值能继续提交，不仅不崩溃。
- 当前resourceBytes必须在每种extent的稳定后回到该key可复现的值；不要求resize过渡帧的总显存等于current值。
- 生产当前图像owner数量固定，退休在途旧图由DXVK追踪；若没有资源generation/创建计数见证，
  应报告“extent请求/消费及缓存规模观察”通过，而非声称所有GPU图像重建/退休物理证明通过。

## 阶段D：关闭尾段和内存趋势（60s）

最后destroy天气，ordinary medium关闭；保存最少30s的关闭稳定样本，末尾离开owned实例并核实正常退出。
每0.5–1s采private bytes/VA/working set和当前weather/Snow资源账；来源字段必须单列。

- 资源账：相同固定key的稳定current bytes精确回到warmup后的基线，不随实例次数上涨。
- 进程趋势：用第2/3尺寸组的同key稳定窗口做成对比较，输出median/p95/max和各组增量，
  排除初次Shader/driver缓存建立；不把working set下降证明GPU退休，也不把Volume保留缓存当泄漏。
- 若要给private bytes硬阈值，先保存同一配方无天气/只有天气的warmup噪声，再在开跑前冻结阈值。
  没有可解释噪声基线时只报趋势观察，不临场选择一个让结果变绿的MiB阈值。
- 持续正斜率、same-key资源账每轮增加、旧id仍可消费、关闭后旧提交标记粘滞、或压力后不恢复都是失败。
  斜率必须提供窗口/采样数/原始数据，不只看起终点两个数。

## 最终交付与不允许声称

交付候选DLL/map原始身份、JASS读回、启动与cleanup回执、完整分阶段status/内存记录、phase夹持截图、
判定器结果和所有未覆盖门。保护玩家DLL/Game.dll/map哈希，尺寸设置/全局输入不变。

允许逐门写：API/时间/同域记录/共存/启停恢复/extent消费/内存趋势分别通过或未覆盖。
禁止由“没有崩溃”、CPU微基准零new、最后一张截图或单帧提交，推导10分钟GPU完全正确、
总显存硬cap、跨地图/所有GPU认证、音频同步或正式稳定发布。
