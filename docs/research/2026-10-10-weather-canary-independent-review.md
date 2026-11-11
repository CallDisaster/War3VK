# 2026-10-10 天气 canary 独立审查与下一批测试

审查 lane：`/root/weather_render`。初始任务只读；发现误通过后主线程明确授权
修订以下三个测试文件和本报告，生产源码、构建、实机与部署仍由主线程拥有。

- `AutoTest/weather_canary_20261010.j`
- `AutoTest/run_weather_canary_20261010.py`
- `AutoTest/test_weather_canary_classifier.py`

旧 `canary-a` 工件保留不改，作为此前 smoke 版本，不归入新合同。以下实现是
`canary-b` 的测试合同，不是已经取得的实机验收。

## 结论：旧 smoke 可以证明 API 基础调用，但不足以证明效果隔离和停止

旧图只有渐进混合的 light-snow/full-weather。轻雪阶段的 t>0 已经把 haze/lowSnow
逐步拉高，没有持续 snow-only，也没有 haze-only。旧 classifier 只核对时间和
命令标志，不核对实际参数、暂停/seek 后的渲染输入或销毁后的渲染停用。

用真实 `classify()` 做五个合成变异，旧版所有 required 键均保持 true：

1. full 阶段实际 frozen snowfall=0.01、haze=0、lowSnow=0。
2. seek 后 frozen 时间仍为旧值且 paused=false。
3. owner 已 disabled，但旧 render/frozen/volume 继续 active。
4. owner 已 destroyed，但旧 render/frozen/volume 继续 active。
5. 不同阶段截图沿用相同 hash。

这是测试合同缺口，不是五个生产故障已经发生的断言。发现后立即通知主线程，
没有以 smoke 结果追认效果已正确。

## 已修订的 15 阶段

每阶段持续约 8 秒，地图 main 后延迟 25 秒开始，launcher readiness 后最多
观察 240 秒。完整阶段 0..14，真实累计 JASS Check/Error 调用为
`9,10,11,12,13,14,17,18,20,22,25,26,27,29,30`，由 Python 从实际 JASS
源解析验证，不能把零检查计数当 PASS。

| 阶段 | 场景 | 六参数 `(snowfall, angle, speed, gust, haze, lowSnow)` | 主要验收 |
|---|---|---|---|
| 0 | baseline，实例未开 | 预设 `(0.35,350,40,0.10,0,0)` | 不绘雪、不合成天气介质 |
| 1 | held snow-only | `(0.35,350,40,0.10,0,0)` | 雪命令，同帧 frozen；介质不提交 |
| 2 | held haze-only | `(0,350,40,0.10,0.65,0)` | 无雪命令，体积合成同帧 |
| 3 | held lowSnow-only | `(0,90,100,0.20,0,0.80)` | 无雪命令，低空介质同帧 |
| 4 | held full | `(1,10,420,0.80,0.75,0.80)` | 雪与体积、六参数及帧/epoch 一致 |
| 5 | paused | 保持 full | owner 时间冻结、render frozen paused=true |
| 6 | seek-paused 至 2 秒 | 保持 full | render 真正消费 2 秒，仍 paused |
| 7 | resume | 保持 full | 两个递增 frozen 帧内时间推进 >0.25 秒 |
| 8 | disable | 配置保留 | owner 时间冻结，雪与体积停用 |
| 9 | re-enable | full | 正常恢复绘制且 frozen 时间推进 |
| 10 | disabled 后编辑 keyframes | 0秒 light、6秒 full、60秒 full | 禁用状态合法编辑；调用无错 |
| 11 | timeline 初段 | 按实际 frozen 时间求 0..6秒插值 | 六参数插值、350→10最短方向、时间推进 |
| 12 | timeline full | full | 实际时间>=6，雪与体积正常提交 |
| 13 | destroy | 空状态 | owner 消失，雪与体积停用 |
| 14 | destroyed 确认 | 空状态 | 旧句柄持续无效，旧渲染不再活跃 |

阶段0、1、2、3、4、5、6、7、8、9、11、12、13采13张2560×1440图。
独立设置关闭 authored sun/global fog/volumetric，因此天气介质不是旧雾参数
偷偷代替；底层 shading/材质仍然存在，不能据此宣称完整灯光组合已验收。

## 输入身份与异步取样口径

- 生产 `weather-status` 由主线程扩到全部六个 runtime/frozen 参数。
- owner live snapshot 可以领先 render：不要求它与 render 同帧。
- 实际渲染证据只用 `render-consumed-value-snapshot` 的 frozenInput；snow 和
  volume 需分别与它 frame/map/device 一致，snow 另需 id/time 一致。
- held 参数按0.001容差核对，角度按圆周差；NaN/Inf/缺字段拒绝。
- 暂停/seek严格核对实际 frozen paused/time，不拿 JASS 返回值替代。
- 所有状态采样都记录查询前后的 JASS stage；跨阶段采样不作为该阶段证据。
- disable/destroy 的 off draw frame 必须晚于前一 active 阶段已观测帧；volume
  也必须 disabled/no-submit，非零 map/device与off draw一致。off时生产会清空
  frozenInput，故不伪造其不存在的帧身份。
- `particleCount`仍是CPU预估，核对16384容量与强度算术只能发现接线漂移，
  不代表GPU实际通过深度/区域/视锥的雪片数。

## 截图能证明什么、不能证明什么

新 launcher 只有在当前真实 frozen 参数和预期 snow/medium 提交状态匹配后
请求截图；截图前后重新读取 status 与stage，保持同场景、同id/map/device，
并保留原始captureDetails。它要求BMP非截断、实际文件size/hash/2560×1440，
拒绝缺图、重复阶段、错误标签、失败截图及跨stage结果。

baseline与snow/haze/full截图hash必须不同，仅排除直接复用相同文件。HUD、
单位idle或其它变化也能造成hash不同，因此这不是天气像素正确性证明。

当前capture回执只有shadowFrameSerial等现有域，不含weather frozen witness。
前后取样只是**阶段包围**，不是精确截图帧绑定。因此三个键永久区分：

- `captureExactWeatherFrameCertified=false`
- `gpuCompletionCertified=false`
- `pixelOrArtisticAcceptance=false`

不得因为 runtimeAcceptance=true 把上述三个门追认为通过。若要精确帧绑定，
下一批应在真正的capture safe point按值附带weather frozen输入与该帧提交
标志，再通过同帧回读证明；本批没有改这个生产交接。

## 同pid内存及故障证据

每条取样在已有psutil可用时读该pid的private/vms/rss（字节）。没有库、没有
Windows private字段或读取失败都明确covered=false，不装包，不写外部路径。
这些是进程CPU/VA账户，不等于天气总GPU resident，也不含驱动退休图像峰值。

收尾只读取当前owned session、过滤同pid的最多8000条runtime事件，并列出
该隔离instance的gpu_incident文件及hash；报告若发现DeviceLost/READ_INVALID等
关键词或incident则不允许runtimeAcceptance。消息采集失败、截断或更早被环
淘汰的历史都不宣称已覆盖。没有已记录异常**不等于GPU执行正确**。

## 实际离线结果

`python AutoTest/test_weather_canary_classifier.py`：**30/30，EXIT 0**。
包括六参数缺失/错误/非有限值、snow/haze/lowSnow隔离、真实frozen seek、
pause、停用仍绘制、销毁仍绘制、恢复不推进、frame/epoch/id/time错配、
stage名称错配、实际粒子预估/容量、截图阶段/输入/体积帧错配、重复/无效
JASS计数、相同图片、event pid过滤和取证不可用反例。

`git diff --check`（三个测试文件）：EXIT 0。
本lane没有运行pjass、DLL构建、游戏或部署；新实机数据由root另行追加。

## 下一批可执行顺序（root拥有lease）

1. 保存当前候选DLL size/SHA，确认生产扩充六参数后的重链接及no-work完成。
2. 新建`canary-b`工件，不能复用或覆盖`canary-a`：

   ```powershell
   python AutoTest/run_weather_canary_20261010.py --prepare --name canary-b
   python AutoTest/run_weather_canary_20261010.py --check-jass --name canary-b
   python AutoTest/run_weather_canary_20261010.py --run --name canary-b --dll <exact-dll-path> --expected-dll-sha <exact-sha256>
   ```

3. 若任何classification键失败，先查对应真实snapshot和stage/hash；不要增时间
   容差、删除失败检查或把owner live参数冒充render输入。合法异步推进只需
   等待正确阶段内的真实frozen快照。
4. 只看baseline/snow-only/haze-only/full四张关键图，确认场景、建筑深度遮挡、
   雪片可见及介质差异后，再看pause/seek/disable/destroy；艺术可读性和性能
   单列。不重复打开没有变化的已审图片。
5. 对比snow-only/haze-only/full各稳定阶段的同pid内存曲线；它不是GPU预算
   闭合。实际GPU工作量、帧时尾延迟和停用后退役资源需后续独立量测。
6. 当前固定镜头不能覆盖相机旋转/快速移动/远缩放、换图、设备Reset、
   透明水面/原生天气/战争迷雾和多种显卡。下一批分别用受控A/B场景，
   不把本图默认1440p的一次成功外推为上述门。
