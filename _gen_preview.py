"""从修改后的 war3_perf_monitor.cpp 提取 HTML 模板，注入模拟数据以生成可预览的文件"""
import re, os

SRC = os.path.join(os.path.dirname(__file__),
                   'src', 'd3d9', 'war3', 'tools', 'war3_perf_monitor.cpp')

with open(SRC, 'r', encoding='utf-8') as f:
    content = f.read()

# 找到 html << R"( 后的 HTML 内容
# 模板在多个 R"(...)" 段中，由 __DATE__, __TIME__, jsonData 分隔

# 第一段: html << R"(<!DOCTYPE html>...) 到第一个 )"
m1_start = content.find('html << R"(<!DOCTYPE html>')
if m1_start == -1:
    raise RuntimeError("Cannot find HTML start")
# 找 << R"( 后的内容起始
block_start = content.find('R"(', m1_start) + 3

# 找下一个 )"
block_end = content.find(')"', block_start)
part1 = content[block_start:block_end]

# __DATE__ << " " << __TIME__ << R"(
# 找第二个 R"( 之后到 )"
pos2_start = content.find('R"(', block_end + 2) + 3
pos2_end = content.find(')"', pos2_start)
part2 = content[pos2_start:pos2_end]

# jsonData << R"(;
pos3_start = content.find('R"(', pos2_end + 2) + 3
pos3_end = content.find(')"', pos3_start)
part3 = content[pos3_start:pos3_end]

# 模拟数据
mock_data = """{
  "avgFps": 58.3,
  "avgFrameTimeMs": 17.15,
  "activeFrameTimeMs": 14.2,
  "p95CpuMs": 22.4,
  "p99CpuMs": 28.1,
  "stddevCpuMs": 3.2,
  "avgGpuTimeMs": 8.5,
  "p95GpuMs": 12.1,
  "avgTrackedActiveCpuMs": 12.8,
  "avgIdleWaitCpuMs": 2.9,
  "avgUntrackedActiveCpuMs": 1.4,
  "cpuCoveragePct": 74.5,
  "cpuCoverageWithIdlePct": 91.2,
  "idleActiveOverlapLikely": false,
  "jank16": 42,
  "jank33": 3,
  "frameCount": 3200,
  "windowSec": 53.5,
  "mainLoopCycle": {
    "present": true,
    "avgCycleMs": 16.8,
    "avgActiveMs": 13.9,
    "avgIdleMs": 2.9,
    "phases": [
      {"name":"Dispatch","path":"War3MainLoop/Pump/Cycle/Dispatch","avgCpuMs":8.5,"callsPerFrame":1,"sharePct":50.6},
      {"name":"Engine","path":"War3MainLoop/Pump/Cycle/Engine","avgCpuMs":3.2,"callsPerFrame":1,"sharePct":19.0},
      {"name":"IdleWait","path":"War3MainLoop/Pump/Cycle/IdleWait","avgCpuMs":2.9,"callsPerFrame":1,"sharePct":17.3},
      {"name":"Render","path":"War3MainLoop/Pump/Cycle/Render","avgCpuMs":2.2,"callsPerFrame":1,"sharePct":13.1}
    ]
  },
  "mainLoopStages": [
    {"name":"Dispatch","suffix":"Dispatch","avgCpuMs":8.5,"callsPerFrame":1,"shareInFramePct":49.6},
    {"name":"Engine","suffix":"Engine","avgCpuMs":3.2,"callsPerFrame":1,"shareInFramePct":18.7},
    {"name":"Render","suffix":"Render","avgCpuMs":2.2,"callsPerFrame":1,"shareInFramePct":12.8}
  ],
  "frameTimes": [16,17,15,18,16,22,17,16,15,19,16,17,28,16,15,17,16,18,17,16,15,33,16,17,16,15,18,17,16,15],
  "frameTimesGpu": [8,9,7,10,8,11,9,8,7,10,8,9,12,8,7,9,8,10,9,8,7,14,8,9,8,7,10,9,8,7],
  "rootSections": [
    {"name":"War3MainLoop","avgCpuMs":14.2,"isIdleWait":false},
    {"name":"DxvkPresent","avgCpuMs":1.5,"isIdleWait":false},
    {"name":"IdleWait","avgCpuMs":2.9,"isIdleWait":true}
  ],
  "sections": [
    {"name":"War3MainLoop","path":"War3MainLoop","parentPath":"","avgCpuMs":14.2,"avgSelfCpuMs":0.3,"avgGpuMs":0,"totalCpuMs":45440,"totalSelfCpuMs":960,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"Pump","path":"War3MainLoop/Pump","parentPath":"War3MainLoop","avgCpuMs":13.9,"avgSelfCpuMs":0.1,"avgGpuMs":0,"totalCpuMs":44480,"totalSelfCpuMs":320,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"Dispatch","path":"War3MainLoop/Pump/Dispatch","parentPath":"War3MainLoop/Pump","avgCpuMs":8.5,"avgSelfCpuMs":0.5,"avgGpuMs":0,"totalCpuMs":27200,"totalSelfCpuMs":1600,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"ExecuteFrame","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame","parentPath":"War3MainLoop/Pump/Dispatch","avgCpuMs":5.3,"avgSelfCpuMs":0.2,"avgGpuMs":0,"totalCpuMs":16960,"totalSelfCpuMs":640,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"CWorld_RenderScene","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene","parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame","avgCpuMs":3.8,"avgSelfCpuMs":0.15,"avgGpuMs":6.2,"totalCpuMs":12160,"totalSelfCpuMs":480,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"DrawTerrain","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene/DrawTerrain","parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene","avgCpuMs":1.2,"avgSelfCpuMs":1.2,"avgGpuMs":2.1,"totalCpuMs":3840,"totalSelfCpuMs":3840,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"DrawUnits","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene/DrawUnits","parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene","avgCpuMs":1.8,"avgSelfCpuMs":0.3,"avgGpuMs":3.5,"totalCpuMs":5760,"totalSelfCpuMs":960,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"BatchSubmit","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene/DrawUnits/BatchSubmit","parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene/DrawUnits","avgCpuMs":1.5,"avgSelfCpuMs":1.5,"avgGpuMs":3.5,"totalCpuMs":4800,"totalSelfCpuMs":4800,"calls":9600,"callsPerFrame":3,"isIdleWait":false},
    {"name":"DrawEffects","path":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene/DrawEffects","parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene","avgCpuMs":0.65,"avgSelfCpuMs":0.65,"avgGpuMs":0.6,"totalCpuMs":2080,"totalSelfCpuMs":2080,"calls":6400,"callsPerFrame":2,"isIdleWait":false},
    {"name":"CommandManager","path":"War3MainLoop/Pump/Dispatch/CommandManager","parentPath":"War3MainLoop/Pump/Dispatch","avgCpuMs":2.5,"avgSelfCpuMs":2.5,"avgGpuMs":0,"totalCpuMs":8000,"totalSelfCpuMs":8000,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"Engine","path":"War3MainLoop/Pump/Engine","parentPath":"War3MainLoop/Pump","avgCpuMs":3.2,"avgSelfCpuMs":1.0,"avgGpuMs":0,"totalCpuMs":10240,"totalSelfCpuMs":3200,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"PhysicsUpdate","path":"War3MainLoop/Pump/Engine/PhysicsUpdate","parentPath":"War3MainLoop/Pump/Engine","avgCpuMs":1.4,"avgSelfCpuMs":1.4,"avgGpuMs":0,"totalCpuMs":4480,"totalSelfCpuMs":4480,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"AIUpdate","path":"War3MainLoop/Pump/Engine/AIUpdate","parentPath":"War3MainLoop/Pump/Engine","avgCpuMs":0.8,"avgSelfCpuMs":0.8,"avgGpuMs":0,"totalCpuMs":2560,"totalSelfCpuMs":2560,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"IdleWait","path":"War3MainLoop/Pump/IdleWait","parentPath":"War3MainLoop/Pump","avgCpuMs":2.1,"avgSelfCpuMs":2.1,"avgGpuMs":0,"totalCpuMs":6720,"totalSelfCpuMs":6720,"calls":3200,"callsPerFrame":1,"isIdleWait":true},
    {"name":"DxvkPresent","path":"DxvkPresent","parentPath":"","avgCpuMs":1.5,"avgSelfCpuMs":1.5,"avgGpuMs":1.2,"totalCpuMs":4800,"totalSelfCpuMs":4800,"calls":3200,"callsPerFrame":1,"isIdleWait":false},
    {"name":"Render","path":"War3MainLoop/Pump/Render","parentPath":"War3MainLoop/Pump","avgCpuMs":0.1,"avgSelfCpuMs":0.1,"avgGpuMs":0,"totalCpuMs":320,"totalSelfCpuMs":320,"calls":3200,"callsPerFrame":1,"isIdleWait":false}
  ],
  "sectionEdges": [
    {"parentPath":"War3MainLoop","parentName":"War3MainLoop","childPath":"War3MainLoop/Pump","childName":"Pump","avgCpuMs":13.9,"callsPerFrame":1,"sharePct":97.9},
    {"parentPath":"War3MainLoop/Pump","parentName":"Pump","childPath":"War3MainLoop/Pump/Dispatch","childName":"Dispatch","avgCpuMs":8.5,"callsPerFrame":1,"sharePct":61.2},
    {"parentPath":"War3MainLoop/Pump/Dispatch","parentName":"Dispatch","childPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame","childName":"ExecuteFrame","avgCpuMs":5.3,"callsPerFrame":1,"sharePct":62.4},
    {"parentPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame","parentName":"ExecuteFrame","childPath":"War3MainLoop/Pump/Dispatch/ExecuteFrame/CWorld_RenderScene","childName":"CWorld_RenderScene","avgCpuMs":3.8,"callsPerFrame":1,"sharePct":71.7},
    {"parentPath":"War3MainLoop/Pump/Dispatch","parentName":"Dispatch","childPath":"War3MainLoop/Pump/Dispatch/CommandManager","childName":"CommandManager","avgCpuMs":2.5,"callsPerFrame":1,"sharePct":29.4}
  ]
}"""

# 组合完整 HTML
html = part1 + "2026-02-23 21:25:00" + part2 + mock_data + part3

OUT = os.path.join(os.path.dirname(__file__), '_preview_dark_theme.html')
with open(OUT, 'w', encoding='utf-8') as f:
    f.write(html)
print("✅ 预览文件已生成:", OUT)
