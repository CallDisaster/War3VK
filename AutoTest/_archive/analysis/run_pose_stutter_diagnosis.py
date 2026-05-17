#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Phase 7.38 阴影 Pose 卡顿根因诊断脚本

目标：收集以下关键数据来定位"流畅 10 帧然后卡 10 帧"的根因：
1. palette capture 的 frameTag 分布（Hook_RuntimeMatrixWrite 写入时的 frameTag）
2. PublishCurrentDrawContract 的 frameTag 分布（消费端读取时的 frameTag）
3. 两者之间的 delta 分布
4. lease restore 时 palette 的帧龄分布
5. 每帧 submit 的 skinned packet 中 palette source 分布

使用方式：
  python AutoTest/run_pose_stutter_diagnosis.py

前置条件：
  - build32/src/d3d9/d3d9.dll 已编译
  - E:\Work\War3\Maps\ShadowTest\光影测试.w3x 存在
"""

import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AUTOTEST_DIR = Path(__file__).resolve().parent
ARTIFACT_DIR = AUTOTEST_DIR / "artifacts"
BUILD_DLL = REPO_ROOT / "build32" / "src" / "d3d9" / "d3d9.dll"
WAR3_DIR = Path(r"E:\Work\War3")
WAR3_DLL = WAR3_DIR / "d3d9.dll"
TEST_MAP = Path(r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x")
DIAGNOSIS_DIR = ARTIFACT_DIR / f"pose_stutter_diagnosis_{datetime.now().strftime('%Y%m%d_%H%M%S')}"


def deploy_dll():
    """部署最新编译的 DLL"""
    if not BUILD_DLL.exists():
        print(f"[ERROR] 编译产物不存在: {BUILD_DLL}")
        return False
    print(f"[DEPLOY] {BUILD_DLL} -> {WAR3_DLL}")
    shutil.copy2(BUILD_DLL, WAR3_DLL)
    return True


def run_autotest_with_hot_shadow(duration_sec=60, samples=None):
    """
    运行 AutoTest 并收集 hot shadow poll 数据。
    使用 MCP 工具链。
    """
    # 直接调用 war3_autotest_mcp.py 的核心逻辑
    sys.path.insert(0, str(AUTOTEST_DIR))
    
    # 导入核心模块
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "war3_autotest_mcp", AUTOTEST_DIR / "war3_autotest_mcp.py"
        )
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as e:
        print(f"[ERROR] 无法加载 AutoTest 模块: {e}")
        return None
    
    return mod


def main():
    print("=" * 70)
    print("Phase 7.38 阴影 Pose 卡顿根因诊断")
    print("=" * 70)
    print()
    print(f"诊断目录: {DIAGNOSIS_DIR}")
    print(f"测试地图: {TEST_MAP}")
    print()
    
    # 创建诊断目录
    DIAGNOSIS_DIR.mkdir(parents=True, exist_ok=True)
    
    # 部署 DLL
    if not deploy_dll():
        return 1
    
    # 记录当前配置
    config_info = {
        "timestamp": datetime.now().isoformat(),
        "build_dll": str(BUILD_DLL),
        "test_map": str(TEST_MAP),
        "diagnosis_goal": "定位阴影 Pose 卡顿根因：流畅 10 帧然后卡 10 帧",
        "key_questions": [
            "1. Hook_RuntimeMatrixWrite 的 frameTag 和 PublishCurrentDrawContract 的 frameTag 差多少？",
            "2. lease restore 时 palette 的帧龄分布如何？",
            "3. 每帧有多少 skinned packet 走 trusted vs raw arena？",
            "4. submitPaletteFrameLag 的分布是否呈现周期性？",
            "5. 引擎 0x12E600 每帧被调用多少次？vs 我们 publish 多少次？",
        ],
        "hypothesis": (
            "根因假设：引擎的 palette 写入（0x12E600）和我们的 PublishCurrentDrawContract "
            "之间存在时序错位。引擎在帧初期做骨骼计算并写入 palette arena，但我们的 "
            "PublishCurrentDrawContract 在 RenderQueue_UpdateItemWorldMatrix 之后才触发。"
            "如果引擎在某些帧跳过了对某些对象的 palette 写入（因为 LOD/culling/优先级），"
            "那么 s_slotBlendedPaletteCache 里的 frameTag 就会停在旧帧，导致 Exact query "
            "连续多帧 miss，palette 只能走 raw arena fallback。"
            "而 raw arena 在这些帧里可能已经被其他对象覆盖了。"
        ),
    }
    
    config_path = DIAGNOSIS_DIR / "diagnosis_config.json"
    with open(config_path, "w", encoding="utf-8") as f:
        json.dump(config_info, f, indent=2, ensure_ascii=False)
    
    print("[INFO] 诊断配置已保存")
    print()
    print("[NEXT STEPS]")
    print("  1. 使用 MCP run_quick_autotest 启动游戏并收集 hot_shadow_poll 数据")
    print("  2. 使用 IDA 逆向确认 0x12E600 的调用时序")
    print("  3. 分析 frameTag mismatch 的周期性模式")
    print()
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
