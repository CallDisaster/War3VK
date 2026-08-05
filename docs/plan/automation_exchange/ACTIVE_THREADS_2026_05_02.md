# Active Threads

Date: 2026-05-02

## Active

1. Thread: `dynamic-shadow-fix`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-05-02 04:30:00.000 +08:00`
   LastHeartbeat: `2026-05-02 04:45:00.000 +08:00`
   Status: `active`
   Goal: `修复动态阴影姿态问题，确保阴影姿态与模型动画同步`
   Notes: `当前问题：阴影位置会跟着 caster 移动，但姿态始终是初始姿态。已确认 runtimeGroupPalette 构建逻辑正确，pose.matrixPalette 从 CModel + 0x60 读取。下一步需要解决 AutoTest 加载问题（unitCount = 0），然后验证 pose 矩阵是否随动画变化。`

## Closed

（无）
