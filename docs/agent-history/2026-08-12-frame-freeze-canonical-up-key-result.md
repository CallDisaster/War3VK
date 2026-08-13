# Frame-freeze canonical UP key A/B 结果

开发 observer DLL 曾以单变量方式，从已验证 stable UP key 中移除中间
DXVK upload ring buffer 指针；source owner、identity/allocation/content
generation、canonical range、map/frame 和 stream identity 全部保持。

“生与死”隔离桌面 300 秒 A/B 没有显示收益：

- A：756,349,507,866 unique bytes / 15,671 diagnostics frames，约
  48.26 MiB/frame；
- B：818,619,103,280 unique bytes / 16,512 diagnostics frames，约
  49.58 MiB/frame；
- duplicate bytes 比例基本不变，producer incomplete/replay reject 没有改善；
- 两轮均无新 GPU event、incident 或 TDR，且测试后稳定 DLL 已恢复。

因此代码实验已经撤销，没有进入 Release 或开发默认。剩余 Arena 压力来自
大量相互独立的 terrain/dynamic exact ranges，不能通过忽略 transport pointer
解决，也不能继续放宽 source generation 或范围身份。
