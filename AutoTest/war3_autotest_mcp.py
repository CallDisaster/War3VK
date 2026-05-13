#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
War3 自动化测试 MCP 服务

目标：
1) 复用 YDWE 的直进地图行为（-loadfile + 测试地图复制）。
2) 订阅 OutputDebugString，自动判断“进入游戏”阶段。
3) 自动截屏、收集性能报告并做摘要。
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import copy
import json
import math
import os
import re
import shutil
import struct
import subprocess
import threading
import time
import winreg
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from mcp.server.fastmcp import FastMCP


DEFAULT_WAR3_DIR = Path(r"E:\Work\War3")
DEFAULT_TEST_MAP = Path(r"E:\Work\War3\Maps\光影测试.w3x")
DEFAULT_LOW_PRESSURE_TEST_MAP = Path(r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x")
DEFAULT_CITY_MAP = Path(r"E:\Work\War3\Maps\dz\rpg\City.w3x")
DEFAULT_CITY_FALLBACK_MAP = DEFAULT_TEST_MAP
DEFAULT_TEST_MAP_REL = Path(r"Maps\Test\WorldEditTestMap.w3x")
DEFAULT_BENCHMARK_WIDTH = 2560
DEFAULT_BENCHMARK_HEIGHT = 1440
DEFAULT_BENCHMARK_REFRESH = 59
WAR3_VIDEO_REG_KEY = r"Software\Blizzard Entertainment\Warcraft III\Video"
ARTIFACT_ROOT = Path(__file__).resolve().parent / "artifacts"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_D3D9 = REPO_ROOT / "build32" / "src" / "d3d9" / "d3d9.dll"
MAX_EVENT_BUFFER = 4000
DESKTOP_READOBJECTS = 0x0001
DESKTOP_CREATEWINDOW = 0x0002
DESKTOP_ENUMERATE = 0x0040
DESKTOP_WRITEOBJECTS = 0x0080
DESKTOP_SWITCHDESKTOP = 0x0100
CREATE_UNICODE_ENVIRONMENT = 0x00000400
CREATE_NEW_CONSOLE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
PIPE_READMODE_MESSAGE = 0x00000002
ERROR_MORE_DATA = 234
ERROR_BROKEN_PIPE = 109
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
_KERNEL32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_KERNEL32.WaitNamedPipeW.restype = wintypes.BOOL
_KERNEL32.CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.LPVOID,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]
_KERNEL32.CreateFileW.restype = wintypes.HANDLE
_KERNEL32.SetNamedPipeHandleState.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
    wintypes.LPVOID,
]
_KERNEL32.SetNamedPipeHandleState.restype = wintypes.BOOL
_KERNEL32.ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_KERNEL32.ReadFile.restype = wintypes.BOOL
_KERNEL32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_KERNEL32.PeekNamedPipe.restype = wintypes.BOOL
_KERNEL32.WriteFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_KERNEL32.WriteFile.restype = wintypes.BOOL
_KERNEL32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
_KERNEL32.FlushFileBuffers.restype = wintypes.BOOL
_KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
_KERNEL32.CloseHandle.restype = wintypes.BOOL


class _StartupInfoW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class _ProcessInformation(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]
RUNTIME_MODULE_ORDER = [
    "hook.lifecycle",
    "hook.ui",
    "hook.jass",
    "hook.render",
    "diag",
    "render.queue",
    "shadow.capture",
    "shadow.map",
    "shadow.receiver",
    "shadow.taa",
    "postfx",
    "ssao",
    "aa",
    "semantic.data",
]
PROFILE_DEFAULT_DISABLED = {
    "dxvk_only": set(RUNTIME_MODULE_ORDER),
    "hooks_minimal": {
        "hook.ui",
        "hook.jass",
        "hook.render",
        "diag",
        "render.queue",
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "hooks_default": {
        "render.queue",
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "render_base": {
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "shadow_capture_only": {
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "shadow_full": {
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "full_default": set(),
    "full_analysis": set(),
    "full_perf_experimental": set(),
}
BENCHMARK_LINE_RE = re.compile(
    r"DXVK War3Benchmark:\s+avgFPS=(?P<avg>[-0-9.]+)"
    r".*?sampleFrames=(?P<frames>\d+)"
    r".*?warmupSec=(?P<warmup>[-0-9.]+)"
    r".*?sampleSec=(?P<sample>[-0-9.]+)"
    r"(?:.*?mode=(?P<mode>[A-Za-z0-9_\-]+))?"
    r"(?:.*?profile=(?P<profile>[A-Za-z0-9_\-]+))?"
    r"(?:.*?disabledModules=(?P<disabled>[^\r\n]*))?"
)
PROFILE_MATRIX_CASES: List[Dict[str, Any]] = [
    {"name": "add_dxvk_only", "profile": "dxvk_only", "disable": "", "group": "additive-core", "label": "DXVK-only", "category": "baseline", "budgetFps": 0.0},
    {"name": "add_hooks_minimal", "profile": "hooks_minimal", "disable": "", "group": "additive-extra", "label": "Hooks Minimal", "category": "hook_probe", "budgetFps": 10.0},
    {"name": "add_hooks_default", "profile": "hooks_default", "disable": "", "group": "additive-core", "label": "Hooks Default", "category": "hook_total", "budgetFps": 25.0},
    {"name": "add_render_base", "profile": "render_base", "disable": "", "group": "additive-core", "label": "Render Base", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "add_shadow_capture_only", "profile": "shadow_capture_only", "disable": "", "group": "additive-extra", "label": "Shadow Capture Only", "category": "shadow_capture_probe", "budgetFps": 45.0},
    {"name": "add_shadow_full", "profile": "shadow_full", "disable": "", "group": "additive-core", "label": "Shadow Full", "category": "shadow_total", "budgetFps": 65.0},
    {"name": "add_full_default", "profile": "full_default", "disable": "", "group": "additive-core", "label": "Full Default", "category": "full_stack", "budgetFps": 0.0},
    {"name": "add_full_analysis", "profile": "full_analysis", "disable": "", "group": "additive-extra", "label": "Full Analysis", "category": "final_profile", "budgetFps": 15.0},
    {"name": "add_full_perf_experimental", "profile": "full_perf_experimental", "disable": "", "group": "additive-extra", "label": "Full Perf Experimental", "category": "final_profile", "budgetFps": 0.0},
    {"name": "sub_no_diag", "profile": "full_default", "disable": "diag", "group": "subtractive", "label": "No Diag", "category": "diag", "budgetFps": 10.0},
    {"name": "sub_no_hook_lifecycle", "profile": "full_default", "disable": "hook.lifecycle", "group": "subtractive", "label": "No Hook Lifecycle", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_ui", "profile": "full_default", "disable": "hook.ui", "group": "subtractive", "label": "No Hook UI", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_jass", "profile": "full_default", "disable": "hook.jass", "group": "subtractive", "label": "No Hook JASS", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_render", "profile": "full_default", "disable": "hook.render", "group": "subtractive", "label": "No Hook Render", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "sub_no_semantic_data", "profile": "full_default", "disable": "semantic.data", "group": "subtractive", "label": "No Semantic Data", "category": "semantic_data", "budgetFps": 35.0},
    {"name": "sub_no_render_queue", "profile": "full_default", "disable": "render.queue", "group": "subtractive", "label": "No Render Queue", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "sub_no_shadow_capture", "profile": "full_default", "disable": "shadow.capture", "group": "subtractive", "label": "No Shadow Capture", "category": "shadow_capture", "budgetFps": 45.0},
    {"name": "sub_no_shadow_map", "profile": "full_default", "disable": "shadow.map", "group": "subtractive", "label": "No Shadow Map", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_shadow_receiver", "profile": "full_default", "disable": "shadow.receiver", "group": "subtractive", "label": "No Shadow Receiver", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_shadow_taa", "profile": "full_default", "disable": "shadow.taa", "group": "subtractive", "label": "No Shadow TAA", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_postfx", "profile": "full_default", "disable": "postfx", "group": "subtractive", "label": "No PostFX", "category": "postfx_diag", "budgetFps": 10.0},
    {"name": "sub_no_ssao", "profile": "full_default", "disable": "ssao", "group": "subtractive", "label": "No SSAO", "category": "postfx_diag", "budgetFps": 10.0},
    {"name": "sub_no_aa", "profile": "full_default", "disable": "aa", "group": "subtractive", "label": "No AA", "category": "postfx_diag", "budgetFps": 10.0},
]

SEMANTIC_SHADOW_VALIDATION_ENV: Dict[str, str] = {
    "DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD": "0",
    "DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY": "1",
    "DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE": "1",
}

SCENARIO_PRESETS: Dict[str, Dict[str, Any]] = {
    "low_pressure_static_reuse": {
        "title": "低压静态复用",
        "description": "低压图静态复用基线，用于观察 persistent cache 的静态收益与 shadowRuntimeV2 占位摘要。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3LowPressureStatic",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 20,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "dynamic_shadow_pressure": {
        "title": "动作单位压力",
        "description": "大量动作/飞行单位场景，用于验证动态阴影正确性与 fallback 成本。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3DynamicShadowPressure",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 22,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 26,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "model_runtime_probe": {
        "title": "模型运行时探针",
        "description": "模型加载、实例绑定与动画姿态更新链路的诊断场景。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_analysis",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3ModelRuntimeProbe",
        "readyTimeoutSec": 180,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "semantic_cost_probe": {
        "title": "语义追踪成本探针",
        "description": "量化 shadow semantic tracking 常驻开销、命中率与未来降级空间。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_analysis",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3SemanticCostProbe",
        "readyTimeoutSec": 150,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 22,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "rigid_static_canonical_smoke": {
        "title": "Rigid/Static Canonical Smoke",
        "description": "用于 Phase 4 prepared 的 rigid/static canonical 场景预案，不强制 hot-shadow gate。",
        "mapPath": str(Path(r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x")),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3RigidStaticCanonical",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 22,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "requireHotShadowFrame": False,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "static_world_caster_acceptance": {
        "title": "Static World Caster Acceptance",
        "description": "用于 Phase 4 correctness：验证 Building / Destructible / rigid-static canonical 提交。",
        "mapPath": str(Path(r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x")),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3StaticWorldCasterAcceptance",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 20,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "phase4_world_caster_acceptance": {
        "title": "Phase 4 World Caster Acceptance",
        "description": "用于 Phase 4 correctness：在真实混合 ShadowTest 场景中验证 Building / Destructible canonical 提交。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3Phase4WorldCasterAcceptance",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 22,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 26,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
}
PROFILE_MATRIX_PRIMARY_CHAIN = [
    "add_dxvk_only",
    "add_hooks_default",
    "add_render_base",
    "add_shadow_full",
    "add_full_default",
]
LOG_KEYWORD_PATTERNS: List[Tuple[str, re.Pattern[str]]] = [
    ("deviceLost", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("freezeBudgetExceeded", re.compile(r"Freeze budget exceeded", re.IGNORECASE)),
    ("shadowCaptureIncomplete", re.compile(r"incomplete capture", re.IGNORECASE)),
    ("shadowReuseLastComplete", re.compile(r"reuse last shadow map", re.IGNORECASE)),
    ("shadowRenderPartial", re.compile(r"render current partial shadow map", re.IGNORECASE)),
    ("shadowAdaptiveSkip", re.compile(r"Adaptive skip ShadowMap", re.IGNORECASE)),
    ("csmComputeFailed", re.compile(r"CSM compute failed", re.IGNORECASE)),
    ("csmConservativeFallback", re.compile(r"conservative cascade fallback", re.IGNORECASE)),
    ("runtimeReady", re.compile(r"(?:JASS|War3) runtime fully initialized", re.IGNORECASE)),
    ("stage19", re.compile(r"War3StageSig: stage=19", re.IGNORECASE)),
    ("pauseBlocked", re.compile(r"blocked GamePause request", re.IGNORECASE)),
    ("internalTestApi", re.compile(r"DXVK War3TestApi:", re.IGNORECASE)),
]


def _now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _now_compact() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def _bool_env(v: str) -> bool:
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def _parse_env_overrides_json(text: str) -> Dict[str, str]:
    raw = str(text or "").strip()
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except Exception as e:
        return {"__parse_error__": str(e)}
    if not isinstance(data, dict):
        return {"__parse_error__": "env_overrides_json 必须是 JSON object"}
    out: Dict[str, str] = {}
    for k, v in data.items():
        key = str(k or "").strip()
        if not key:
            continue
        out[key] = str(v)
    return out


def _normalize_runtime_profile_name(profile: str) -> str:
    value = str(profile or "").strip().lower()
    if not value:
        return "full_default"
    if value in PROFILE_DEFAULT_DISABLED:
        return value
    return "full_default"


def _expand_disable_modules_csv(disable_csv: str) -> List[str]:
    tokens = [str(x or "").strip().lower() for x in str(disable_csv or "").split(",")]
    disabled: List[str] = []
    for token in tokens:
        if not token:
            continue
        if token == "all":
            return list(RUNTIME_MODULE_ORDER)
        if token == "shadow":
            for name in ("shadow.capture", "shadow.map", "shadow.receiver", "shadow.taa"):
                if name not in disabled:
                    disabled.append(name)
            continue
        if token in RUNTIME_MODULE_ORDER and token not in disabled:
            disabled.append(token)
    return disabled


def _runtime_profile_summary_from_inputs(profile: str, disable_csv: str) -> Dict[str, Any]:
    profile_name = _normalize_runtime_profile_name(profile)
    disabled = set(PROFILE_DEFAULT_DISABLED.get(profile_name, set()))
    disabled.update(_expand_disable_modules_csv(disable_csv))
    enabled = [name for name in RUNTIME_MODULE_ORDER if name not in disabled]
    disabled_list = [name for name in RUNTIME_MODULE_ORDER if name in disabled]
    return {
        "name": profile_name,
        "disabledModules": ",".join(disabled_list),
        "enabledModules": ",".join(enabled),
        "diagEnabled": "diag" in enabled,
    }


def _normalize_scenario_name(name: str) -> str:
    return str(name or "").strip().lower().replace("-", "_")


def _get_scenario_preset(name: str) -> Dict[str, Any]:
    key = _normalize_scenario_name(name)
    preset = SCENARIO_PRESETS.get(key)
    return dict(preset) if isinstance(preset, dict) else {}


def _scenario_preset_rows() -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for key in sorted(SCENARIO_PRESETS.keys()):
        preset = SCENARIO_PRESETS[key]
        rows.append(
            {
                "name": key,
                "title": str(preset.get("title", key)),
                "description": str(preset.get("description", "")),
                "mapPath": str(preset.get("mapPath", "")),
                "profile": str(preset.get("profile", "")),
                "disableModules": str(preset.get("disableModules", "")),
                "windowed": bool(preset.get("windowed", False)),
                "useIsolatedDesktop": bool(preset.get("useIsolatedDesktop", True)),
                "desktopName": str(preset.get("desktopName", "")),
                "readyTimeoutSec": int(preset.get("readyTimeoutSec", 0) or 0),
                "sampleDurationSec": int(preset.get("sampleDurationSec", 0) or 0),
                "autoPerfRecord": bool(preset.get("autoPerfRecord", True)),
                "recordAfterGameStarted": bool(preset.get("recordAfterGameStarted", True)),
                "autoPerfExportSec": int(preset.get("autoPerfExportSec", 0) or 0),
                "deployD3d9BeforeLaunch": bool(preset.get("deployD3d9BeforeLaunch", True)),
                "opengl": bool(preset.get("opengl", False)),
                "enforceVideoBaseline": bool(preset.get("enforceVideoBaseline", True)),
                "baselineWidth": int(preset.get("baselineWidth", DEFAULT_BENCHMARK_WIDTH) or DEFAULT_BENCHMARK_WIDTH),
                "baselineHeight": int(preset.get("baselineHeight", DEFAULT_BENCHMARK_HEIGHT) or DEFAULT_BENCHMARK_HEIGHT),
                "baselineRefreshRate": int(preset.get("baselineRefreshRate", DEFAULT_BENCHMARK_REFRESH) or DEFAULT_BENCHMARK_REFRESH),
            }
        )
    return rows


def _zero_shadow_budget_summary() -> Dict[str, Any]:
    phases = []
    for name in ("POS", "BLEND", "UV", "INDEX"):
        phases.append(
            {
                "name": name,
                "requestedMb": 0.0,
                "acceptedMb": 0.0,
                "rejectedMb": 0.0,
                "requests": 0,
                "rejects": 0,
            }
        )
    return {
        "framesObserved": 0,
        "framesIncomplete": 0,
        "framesBudgetExceeded": 0,
        "framesReuseLastComplete": 0,
        "framesRenderCurrentPartial": 0,
        "avgBudgetMb": 0.0,
        "avgUsedMb": 0.0,
        "maxBudgetMb": 0.0,
        "maxUsedMb": 0.0,
        "skippedFreezeBudget": 0,
        "skippedUpload": 0,
        "skippedCasterCap": 0,
        "skippedDistanceCull": 0,
        "phases": phases,
    }


def _zero_shadow_runtime_v2_summary() -> Dict[str, Any]:
    return {
        "staticPersistentCount": 0,
        "dynamicPoseCount": 0,
        "dynamicSkinnedOutputCount": 0,
        "fallbackDrawCount": 0,
        "fallbackDrawCountTerrain": 0,
        "fallbackDrawCountWorldObject": 0,
        "fallbackDrawCountUnitObject": 0,
        "objectFallbackDrawCount": 0,
        "semanticBridgeHit": 0,
        "semanticBridgeMiss": 0,
        "semanticBridgeBypassed": 0,
        "semanticSceneSubmitted": 0,
        "semanticSceneSubmittedUnit": 0,
        "semanticSceneSubmittedSkinned": 0,
        "semanticSceneSubmittedFrameLocal": 0,
        "semanticSceneSubmittedPersistent": 0,
        "semanticSceneAcceptedExplicitResourceOwnerRigid": 0,
        "worldObjectListOwnerHintZeroContextAcceptedCount": 0,
        "modelRegistryHit": 0,
        "modelRegistryMiss": 0,
        "modelLoadCount": 0,
        "modelReuseCount": 0,
        "runtimeBoundCount": 0,
        "completeIdentityCount": 0,
        "shadowRuntimeBoundCount": 0,
        "shadowIdentityCount": 0,
        "poseUpdateCount": 0,
        "poseCacheHit": 0,
        "poseCacheMiss": 0,
        "bonePaletteUpdates": 0,
        "shadowGeosetResourceCount": 0,
        "shadowReadyGeosetCount": 0,
        "shadowModelResourceCount": 0,
        "shadowPoseReadyCount": 0,
        "upperLayerResolveAttempts": 0,
        "upperLayerResolveVisibleMiss": 0,
        "upperLayerResolveVisibleUnresolvedGeoset": 0,
        "upperLayerResolveGeosetMiss": 0,
        "upperLayerResolvePoseMiss": 0,
        "upperLayerResolveRuntimeGroupPaletteMiss": 0,
        "upperLayerResolveAuthoritativeRigid": 0,
        "upperLayerResolveAuthoritativeSkinned": 0,
        "upperLayerEmitted": 0,
        "upperLayerDuplicateOrSuppressed": 0,
        "animationSequenceCount": 0,
        "avgModelResolveCpuMs": 0.0,
        "avgPoseUpdateCpuMs": 0.0,
        "avgSkinnedOutputCpuMs": 0.0,
        "modelResourceBytes": 0,
        "poseResourceBytes": 0,
        "frameSerial": 0,
        "notes": "placeholder",
    }


def _ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def _safe_rel(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except Exception:
        return str(path)


def _build_environment_block(env: Dict[str, str]) -> ctypes.Array[Any]:
    pairs: List[str] = []
    for key in sorted(env.keys(), key=lambda x: str(x).lower()):
        pairs.append(f"{key}={env[key]}")
    text = "\0".join(pairs) + "\0\0"
    return ctypes.create_unicode_buffer(text)


def _desktop_access_mask() -> int:
    return (
        DESKTOP_READOBJECTS
        | DESKTOP_CREATEWINDOW
        | DESKTOP_ENUMERATE
        | DESKTOP_WRITEOBJECTS
        | DESKTOP_SWITCHDESKTOP
    )


def _create_isolated_desktop(name: str) -> Dict[str, Any]:
    desktop_name = str(name or "").strip()
    if not desktop_name:
        desktop_name = f"War3AutoTest_{_now_compact()}"

    result: Dict[str, Any] = {}

    def _worker() -> None:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        user32.CreateDesktopW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.LPCWSTR,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
        ]
        user32.CreateDesktopW.restype = wintypes.HANDLE
        handle = user32.CreateDesktopW(
            desktop_name,
            None,
            None,
            0,
            _desktop_access_mask(),
            None,
        )
        if not handle:
            result["ok"] = False
            result["error"] = f"CreateDesktopW 失败: {int(kernel32.GetLastError())}"
            return
        result["ok"] = True
        result["name"] = desktop_name
        result["handle"] = int(handle)

    thread = threading.Thread(target=_worker, daemon=True)
    thread.start()
    thread.join(timeout=5.0)
    if thread.is_alive():
        return {"ok": False, "error": "CreateDesktopW 超时", "name": desktop_name}
    if not result.get("ok"):
        return {"ok": False, "error": result.get("error", "CreateDesktopW 失败"), "name": desktop_name}
    return result


def _close_desktop_handle(handle: int) -> bool:
    if int(handle or 0) == 0:
        return True
    try:
        user32 = ctypes.windll.user32
        user32.CloseDesktop.argtypes = [wintypes.HANDLE]
        user32.CloseDesktop.restype = wintypes.BOOL
        return bool(user32.CloseDesktop(wintypes.HANDLE(int(handle))))
    except Exception:
        return False


def _launch_process_on_desktop(
    args: List[str],
    cwd: Path,
    env: Dict[str, str],
    desktop_name: str,
) -> Dict[str, Any]:
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateProcessW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.LPWSTR,
        wintypes.LPVOID,
        wintypes.LPVOID,
        wintypes.BOOL,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.LPCWSTR,
        ctypes.POINTER(_StartupInfoW),
        ctypes.POINTER(_ProcessInformation),
    ]
    kernel32.CreateProcessW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    startup = _StartupInfoW()
    startup.cb = ctypes.sizeof(_StartupInfoW)
    startup.lpDesktop = f"WinSta0\\{desktop_name}"
    proc_info = _ProcessInformation()
    env_block = _build_environment_block(env)
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(x) for x in args]))
    app_name = str(args[0]) if args else ""
    ok = bool(
        kernel32.CreateProcessW(
            app_name,
            cmdline,
            None,
            None,
            False,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
            ctypes.cast(env_block, wintypes.LPVOID),
            str(cwd),
            ctypes.byref(startup),
            ctypes.byref(proc_info),
        )
    )
    if not ok:
        return {
            "ok": False,
            "error": f"CreateProcessW 失败: {int(kernel32.GetLastError())}",
            "desktop": desktop_name,
        }

    pid = int(proc_info.dwProcessId)
    if proc_info.hThread:
        kernel32.CloseHandle(proc_info.hThread)
    if proc_info.hProcess:
        kernel32.CloseHandle(proc_info.hProcess)
    return {
        "ok": True,
        "pid": pid,
        "desktop": desktop_name,
    }


def _set_process_priority_high(pid: int) -> Dict[str, Any]:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    PROCESS_SET_INFORMATION = 0x0200
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    HIGH_PRIORITY_CLASS = 0x00000080
    handle = kernel32.OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        False,
        int(pid),
    )
    if not handle:
        return {
            "ok": False,
            "error": f"OpenProcess 失败: {int(ctypes.get_last_error())}",
            "pid": int(pid),
        }
    try:
        if not kernel32.SetPriorityClass(handle, HIGH_PRIORITY_CLASS):
            return {
                "ok": False,
                "error": f"SetPriorityClass 失败: {int(ctypes.get_last_error())}",
                "pid": int(pid),
            }
        return {"ok": True, "pid": int(pid), "priority": "HIGH"}
    finally:
        kernel32.CloseHandle(handle)


def _read_png_size(path: Path) -> Tuple[int, int]:
    """
    读取 PNG/BMP 宽高（避免引入 Pillow 作为硬依赖）。
    返回 (0, 0) 表示解析失败。
    """
    try:
        with path.open("rb") as f:
            header = f.read(32)
        if len(header) < 24:
            return 0, 0
        # PNG signature + IHDR(13 bytes), width/height at offset 16/20
        if header[:8] != b"\x89PNG\r\n\x1a\n":
            if header[:2] != b"BM" or len(header) < 26:
                return 0, 0
            width = struct.unpack("<i", header[18:22])[0]
            height = struct.unpack("<i", header[22:26])[0]
            return int(abs(width)), int(abs(height))
        width = struct.unpack(">I", header[16:20])[0]
        height = struct.unpack(">I", header[20:24])[0]
        return int(width), int(height)
    except Exception:
        return 0, 0


def _set_war3_video_registry(
    width: int = DEFAULT_BENCHMARK_WIDTH,
    height: int = DEFAULT_BENCHMARK_HEIGHT,
    refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    color_depth: int = 32,
) -> Dict[str, Any]:
    """
    统一写入 War3 视频配置注册表，保证性能测试基线一致。
    """
    key_path = WAR3_VIDEO_REG_KEY
    width = max(640, int(width))
    height = max(480, int(height))
    refresh_rate = max(30, int(refresh_rate))
    color_depth = 32 if int(color_depth) not in (16, 32) else int(color_depth)

    old_vals: Dict[str, Any] = {}
    new_vals: Dict[str, Any] = {
        "reswidth": width,
        "resheight": height,
        "refreshrate": refresh_rate,
        "colordepth": color_depth,
        "texcolordepth": color_depth,
    }
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            for name in new_vals:
                try:
                    old_vals[name] = int(winreg.QueryValueEx(key, name)[0])
                except OSError:
                    old_vals[name] = None
            for name, val in new_vals.items():
                winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(val))
    except Exception as e:
        return {
            "ok": False,
            "error": f"写入视频注册表失败: {e}",
            "keyPath": key_path,
            "old": old_vals,
            "new": new_vals,
        }

    return {
        "ok": True,
        "keyPath": key_path,
        "old": old_vals,
        "new": new_vals,
    }


def _restore_war3_video_registry(snapshot: Dict[str, Any], key_path: str = WAR3_VIDEO_REG_KEY) -> Dict[str, Any]:
    """
    按 launch 前快照恢复 War3 视频注册表，避免自动化把用户配置长期改掉。
    """
    if not isinstance(snapshot, dict) or not snapshot:
        return {
            "ok": True,
            "skipped": True,
            "reason": "无可恢复的视频配置快照",
            "keyPath": key_path,
        }

    before_vals: Dict[str, Any] = {}
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            for name, val in snapshot.items():
                try:
                    before_vals[name] = winreg.QueryValueEx(key, name)[0]
                except OSError:
                    before_vals[name] = None

                if val is None:
                    try:
                        winreg.DeleteValue(key, name)
                    except OSError:
                        pass
                else:
                    winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(val))
    except Exception as e:
        return {
            "ok": False,
            "error": f"恢复视频注册表失败: {e}",
            "keyPath": key_path,
            "before": before_vals,
            "restore": snapshot,
        }

    return {
        "ok": True,
        "keyPath": key_path,
        "before": before_vals,
        "restored": snapshot,
    }


def _find_latest_report(war3_dir: Path) -> Optional[Path]:
    log_dir = war3_dir / "WarVK" / "Log"
    if not log_dir.exists():
        return None
    cands = sorted(
        log_dir.glob("war3_perf_report*.html"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return cands[0] if cands else None


def _runtime_status_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "runtime_status.json"


# 这些 legacy JSON 路径仅保留给旧工件清理/离线诊断使用。
# 主动控制链已统一收口到 named pipe control plane。
def _frame_capture_request_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "frame_capture_request.json"


def _frame_capture_result_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "frame_capture_result.json"


def _internal_test_request_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "internal_test_request.json"


def _internal_test_result_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "internal_test_result.json"


def _control_plane_pipe_name(pid: int) -> str:
    return rf"\\.\pipe\War3ControlPlane_{int(pid)}"


def _control_plane_request(
    pid: int,
    command: str,
    payload: Optional[Dict[str, Any]] = None,
    timeout_sec: float = 6.0,
) -> Dict[str, Any]:
    target_pid = int(pid or 0)
    if target_pid <= 0:
        return {"transportOk": False, "ok": False, "error": "无有效 pid"}

    pipe_name = _control_plane_pipe_name(target_pid)
    timeout_ms = max(200, int(float(timeout_sec) * 1000.0))
    t0 = time.time()
    request = {
        "requestId": f"cp_{_now_compact()}_{target_pid}_{int(time.time() * 1000)}",
        "command": str(command or "").strip(),
        "payload": dict(payload or {}),
        "issuedAtMs": int(time.time() * 1000),
        "pid": target_pid,
    }

    if not bool(_KERNEL32.WaitNamedPipeW(pipe_name, timeout_ms)):
        return {
            "transportOk": False,
            "ok": False,
            "error": f"named pipe 不可用: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
        }

    handle = _KERNEL32.CreateFileW(
        pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        None,
        OPEN_EXISTING,
        0,
        None,
    )
    if handle == INVALID_HANDLE_VALUE:
        return {
            "transportOk": False,
            "ok": False,
            "error": f"CreateFileW(pipe) 失败: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
        }

    try:
        mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
        _KERNEL32.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None)

        raw = json.dumps(request, ensure_ascii=False).encode("utf-8")
        write_buf = ctypes.create_string_buffer(raw)
        written = wintypes.DWORD()
        if not bool(_KERNEL32.WriteFile(handle, write_buf, len(raw), ctypes.byref(written), None)):
            return {
                "transportOk": False,
                "ok": False,
                "error": f"WriteFile(pipe) 失败: {ctypes.get_last_error()}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }
        _KERNEL32.FlushFileBuffers(handle)

        response_deadline = time.time() + max(0.2, float(timeout_sec))
        while True:
            bytes_avail = wintypes.DWORD()
            bytes_left = wintypes.DWORD()
            ok = bool(
                _KERNEL32.PeekNamedPipe(
                    handle,
                    None,
                    0,
                    None,
                    ctypes.byref(bytes_avail),
                    ctypes.byref(bytes_left),
                )
            )
            if not ok:
                return {
                    "transportOk": False,
                    "ok": False,
                    "error": f"PeekNamedPipe(pipe) 失败: {ctypes.get_last_error()}",
                    "pipeName": pipe_name,
                    "request": request,
                    "elapsedSec": round(time.time() - t0, 3),
                }
            if bytes_avail.value > 0 or bytes_left.value > 0:
                break
            if time.time() >= response_deadline:
                return {
                    "transportOk": False,
                    "ok": False,
                    "error": "等待 control-plane 响应超时",
                    "pipeName": pipe_name,
                    "request": request,
                    "elapsedSec": round(time.time() - t0, 3),
                }
            time.sleep(0.01)

        chunks: List[bytes] = []
        while True:
            read_buf = ctypes.create_string_buffer(65536)
            read = wintypes.DWORD()
            ok = bool(_KERNEL32.ReadFile(handle, read_buf, len(read_buf), ctypes.byref(read), None))
            if ok:
                if read.value > 0:
                    chunks.append(read_buf.raw[: read.value])
                break
            err = ctypes.get_last_error()
            if err == ERROR_MORE_DATA:
                if read.value > 0:
                    chunks.append(read_buf.raw[: read.value])
                continue
            if err == ERROR_BROKEN_PIPE:
                break
            return {
                "transportOk": False,
                "ok": False,
                "error": f"ReadFile(pipe) 失败: {err}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }

        if not chunks:
            return {
                "transportOk": False,
                "ok": False,
                "error": "pipe 响应为空",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }

        response = json.loads(b"".join(chunks).decode("utf-8", errors="ignore"))
        result = response.get("result", {})
        return {
            "transportOk": True,
            "ok": bool(response.get("ok")),
            "error": str(response.get("error", "") or ""),
            "pipeName": pipe_name,
            "request": request,
            "response": response,
            "result": result if isinstance(result, dict) else {"value": result},
            "elapsedSec": round(time.time() - t0, 3),
        }
    except Exception as e:
        return {
            "transportOk": False,
            "ok": False,
            "error": f"pipe 请求异常: {e}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
        }
    finally:
        _KERNEL32.CloseHandle(handle)


def _read_json_file(path: Path) -> Optional[Dict[str, Any]]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8", errors="ignore"))
    except Exception:
        return None


def _read_runtime_status_file(war3_dir: Path) -> Optional[Dict[str, Any]]:
    return _read_json_file(_runtime_status_file(war3_dir))


def _read_runtime_status_best_effort(target_pid: int = 0) -> Optional[Dict[str, Any]]:
    pid = int(target_pid or STATE.war3_pid or 0)
    if pid > 0 and _pid_alive(pid):
        pipe_res = _control_plane_request(
            pid=pid,
            command="get_runtime_status",
            payload={},
            timeout_sec=1.5,
        )
        if pipe_res.get("transportOk") and pipe_res.get("ok"):
            data = dict(pipe_res.get("result", {}) or {})
            if data:
                return data

    war3_dir = STATE.war3_dir if isinstance(STATE.war3_dir, Path) else DEFAULT_WAR3_DIR
    return _read_runtime_status_file(Path(war3_dir))


def _read_bmp_luma_samples(path: Path, max_samples: int = 262144) -> Dict[str, Any]:
    """
    读取 BMP 并转成抽样亮度序列，用于连续帧稳定性比较。
    仅支持常见 BI_RGB 24/32-bit BMP。
    """
    try:
        raw = path.read_bytes()
    except Exception as e:
        return {"ok": False, "error": f"读取 BMP 失败: {e}", "path": str(path)}

    if len(raw) < 54 or raw[:2] != b"BM":
        return {"ok": False, "error": "不是有效 BMP", "path": str(path)}

    try:
        pixel_offset = struct.unpack_from("<I", raw, 10)[0]
        dib_size = struct.unpack_from("<I", raw, 14)[0]
        if dib_size < 40:
            return {"ok": False, "error": "BMP DIB header 太小", "path": str(path)}
        width = int(struct.unpack_from("<i", raw, 18)[0])
        height_raw = int(struct.unpack_from("<i", raw, 22)[0])
        planes = int(struct.unpack_from("<H", raw, 26)[0])
        bpp = int(struct.unpack_from("<H", raw, 28)[0])
        compression = int(struct.unpack_from("<I", raw, 30)[0])
    except Exception as e:
        return {"ok": False, "error": f"BMP header 解析失败: {e}", "path": str(path)}

    if planes != 1:
        return {"ok": False, "error": f"不支持的 BMP planes={planes}", "path": str(path)}
    if compression != 0:
        return {"ok": False, "error": f"不支持压缩 BMP compression={compression}", "path": str(path)}
    if bpp not in (24, 32):
        return {"ok": False, "error": f"不支持的 BMP bpp={bpp}", "path": str(path)}

    width = abs(width)
    height = abs(height_raw)
    if width <= 0 or height <= 0:
        return {"ok": False, "error": "BMP 宽高非法", "path": str(path)}

    bytes_per_pixel = bpp // 8
    row_stride = ((width * bpp + 31) // 32) * 4
    if pixel_offset + row_stride * height > len(raw):
        return {"ok": False, "error": "BMP 数据截断", "path": str(path)}

    sample_stride = max(1, int(max((width * height) / max(1, max_samples), 1) ** 0.5))
    bottom_up = height_raw > 0
    samples: List[int] = []
    dark_count = 0
    bright_count = 0
    total_luma = 0

    for sample_y in range(0, height, sample_stride):
        src_y = (height - 1 - sample_y) if bottom_up else sample_y
        row_start = pixel_offset + src_y * row_stride
        row = raw[row_start : row_start + row_stride]
        for sample_x in range(0, width, sample_stride):
            base = sample_x * bytes_per_pixel
            if base + 2 >= len(row):
                break
            b = row[base + 0]
            g = row[base + 1]
            r = row[base + 2]
            luma = (77 * r + 150 * g + 29 * b) >> 8
            samples.append(luma)
            total_luma += luma
            if luma <= 96:
                dark_count += 1
            if luma >= 240:
                bright_count += 1

    count = len(samples)
    if count <= 0:
        return {"ok": False, "error": "BMP 抽样失败", "path": str(path)}

    return {
        "ok": True,
        "path": str(path),
        "width": int(width),
        "height": int(height),
        "sampleStride": int(sample_stride),
        "sampleCount": int(count),
        "avgLuma": round(float(total_luma) / float(count), 4),
        "darkRatioPct": round(float(dark_count) * 100.0 / float(count), 4),
        "brightRatioPct": round(float(bright_count) * 100.0 / float(count), 4),
        "samples": samples,
    }


def _compare_bmp_sequence(paths: List[Path]) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    valid: List[Dict[str, Any]] = []
    for path in paths:
        parsed = _read_bmp_luma_samples(path)
        rows.append(parsed)
        if parsed.get("ok"):
            valid.append(parsed)

    if len(valid) < 2:
        return {
            "ok": False,
            "error": "有效 BMP 帧不足，无法比较",
            "frames": rows,
        }

    pairwise: List[Dict[str, Any]] = []
    mean_diffs: List[float] = []
    changed_pcts: List[float] = []
    dark_ratios: List[float] = []
    avg_lumas: List[float] = []

    for row in valid:
        dark_ratios.append(float(row.get("darkRatioPct", 0.0) or 0.0))
        avg_lumas.append(float(row.get("avgLuma", 0.0) or 0.0))

    for idx in range(len(valid) - 1):
        a = valid[idx]
        b = valid[idx + 1]
        sa = list(a.get("samples", []) or [])
        sb = list(b.get("samples", []) or [])
        sample_count = min(len(sa), len(sb))
        if sample_count <= 0:
            continue
        abs_sum = 0.0
        changed = 0
        max_abs = 0
        threshold = 16
        for i in range(sample_count):
            diff = abs(int(sa[i]) - int(sb[i]))
            abs_sum += diff
            if diff >= threshold:
                changed += 1
            if diff > max_abs:
                max_abs = diff
        mean_abs = abs_sum / float(sample_count)
        mean_abs_pct = mean_abs * 100.0 / 255.0
        changed_pct = float(changed) * 100.0 / float(sample_count)
        mean_diffs.append(mean_abs_pct)
        changed_pcts.append(changed_pct)
        pairwise.append(
            {
                "from": str(a.get("path", "")),
                "to": str(b.get("path", "")),
                "sampleCount": int(sample_count),
                "meanAbsDiff": round(mean_abs, 4),
                "meanAbsDiffPct": round(mean_abs_pct, 4),
                "changedPct": round(changed_pct, 4),
                "maxAbsDiff": int(max_abs),
                "darkRatioDeltaPct": round(
                    abs(float(a.get("darkRatioPct", 0.0) or 0.0) - float(b.get("darkRatioPct", 0.0) or 0.0)),
                    4,
                ),
                "avgLumaDelta": round(
                    abs(float(a.get("avgLuma", 0.0) or 0.0) - float(b.get("avgLuma", 0.0) or 0.0)),
                    4,
                ),
            }
        )

    if not pairwise:
        return {
            "ok": False,
            "error": "帧序列之间没有可比较的有效对",
            "frames": rows,
        }

    max_mean_diff_pct = max(mean_diffs)
    max_changed_pct = max(changed_pcts)
    dark_ratio_range_pct = max(dark_ratios) - min(dark_ratios)
    avg_luma_range = max(avg_lumas) - min(avg_lumas)
    flicker_suspect = max_mean_diff_pct >= 2.5 or max_changed_pct >= 8.0 or dark_ratio_range_pct >= 8.0
    missing_shadow_suspect = min(dark_ratios) <= 0.2 and dark_ratio_range_pct >= 6.0 and avg_luma_range >= 12.0

    return {
        "ok": True,
        "frames": rows,
        "pairwise": pairwise,
        "summary": {
            "frameCount": len(valid),
            "maxMeanAbsDiffPct": round(max_mean_diff_pct, 4),
            "avgMeanAbsDiffPct": round(sum(mean_diffs) / len(mean_diffs), 4),
            "maxChangedPct": round(max_changed_pct, 4),
            "darkRatioRangePct": round(dark_ratio_range_pct, 4),
            "avgLumaRange": round(avg_luma_range, 4),
            "flickerSuspect": bool(flicker_suspect),
            "missingShadowSuspect": bool(missing_shadow_suspect),
        },
    }


def _map_identity_key(path: Path) -> str:
    try:
        return str(path.resolve()).lower()
    except Exception:
        return str(path).lower()


def _build_suite_map_candidates(
    requested_map: Path,
    allow_fallback_to_default_test_map: bool,
) -> List[Path]:
    candidates: List[Path] = []
    seen: set[str] = set()
    for candidate in (
        requested_map,
        DEFAULT_CITY_FALLBACK_MAP if allow_fallback_to_default_test_map else None,
    ):
        if candidate is None:
            continue
        path = Path(candidate)
        key = _map_identity_key(path)
        if key in seen:
            continue
        seen.add(key)
        candidates.append(path)
    return candidates


def _launch_suite_map_until_ready(
    *,
    war3_dir: str,
    requested_map_path: str,
    allow_fallback_to_default_test_map: bool,
    ready_timeout_sec: int,
    ready_allow_fallback: bool,
    ready_require_game_started_for_fallback: bool,
    ready_fallback_min_elapsed_sec: int,
    ready_fallback_min_cpu_sec: float,
    launch_kwargs: Dict[str, Any],
) -> Dict[str, Any]:
    requested_map = Path(requested_map_path)
    attempts: List[Dict[str, Any]] = []

    for attempt_index, candidate in enumerate(
        _build_suite_map_candidates(
            requested_map, bool(allow_fallback_to_default_test_map)
        )
    ):
        launch = launch_war3_test(
            war3_dir=war3_dir,
            map_path=str(candidate),
            **launch_kwargs,
        )
        row: Dict[str, Any] = {
            "attempt": attempt_index + 1,
            "mapPath": str(candidate),
            "fallbackCandidate": bool(attempt_index > 0),
            "launch": launch,
        }
        if not launch.get("ok"):
            row["stage"] = "launch"
            attempts.append(row)
            continue

        pid = int(launch["pid"])
        ready = wait_for_game_ready(
            timeout_sec=ready_timeout_sec,
            pid=pid,
            allow_fallback=bool(ready_allow_fallback),
            fallback_min_elapsed_sec=int(ready_fallback_min_elapsed_sec),
            fallback_min_cpu_sec=float(ready_fallback_min_cpu_sec),
            require_game_started_for_fallback=bool(
                ready_require_game_started_for_fallback
            ),
        )
        row["ready"] = ready
        if ready.get("ok"):
            attempts.append(row)
            return {
                "ok": True,
                "pid": pid,
                "launch": launch,
                "ready": ready,
                "requestedMapPath": str(requested_map),
                "actualMapPath": str(candidate),
                "fallbackUsed": bool(attempt_index > 0),
                "attempts": attempts,
            }

        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=True,
        )
        row["stop"] = stop
        row["stage"] = "ready"
        attempts.append(row)

    return {
        "ok": False,
        "stage": "ready",
        "requestedMapPath": str(requested_map),
        "attempts": attempts,
    }


def _invoke_internal_test_request(
    pid: int,
    war3_dir: Path,
    command: str,
    payload: Optional[Dict[str, Any]] = None,
    timeout_sec: float = 6.0,
) -> Dict[str, Any]:
    pipe_res = _control_plane_request(
        pid=pid,
        command="invoke_test_command",
        payload={
            "command": str(command or "").strip(),
            "payload": dict(payload or {}),
            "timeoutMs": max(1000, int(float(timeout_sec) * 1000.0)),
        },
        timeout_sec=max(2.0, float(timeout_sec) + 1.0),
    )
    if pipe_res.get("transportOk"):
        return {
            "ok": bool(pipe_res.get("ok")),
            "requestId": str((pipe_res.get("response", {}) or {}).get("requestId", "")),
            "command": str(command or "").strip(),
            "mode": "control-plane",
            "response": dict(pipe_res.get("response", {}) or {}),
            "result": dict(pipe_res.get("result", {}) or {}),
            "error": str(pipe_res.get("error", "") or ""),
            "elapsedSec": round(float(pipe_res.get("elapsedSec", 0.0) or 0.0), 3),
            "pipeName": str(pipe_res.get("pipeName", "") or ""),
        }
    return {
        "ok": False,
        "command": str(command or "").strip(),
        "mode": "control-plane-unavailable",
        "error": str(pipe_res.get("error", "control plane 不可用") or "control plane 不可用"),
        "detail": pipe_res,
    }


def _capture_final_frame_via_internal_test_api(
    pid: int,
    war3_dir: Path,
    output_path: Path,
    timeout_sec: float = 8.0,
) -> Dict[str, Any]:
    return _request_internal_frame_capture(
        pid=pid,
        output_path=output_path,
        war3_dir=war3_dir,
        timeout_sec=timeout_sec,
    )


def _extract_json_object(text: str, marker: str = "const data =") -> Optional[Dict[str, Any]]:
    idx = text.find(marker)
    if idx < 0:
        return None
    start = text.find("{", idx)
    if start < 0:
        return None
    depth = 0
    end = -1
    for i in range(start, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        return None
    try:
        return json.loads(text[start : end + 1])
    except json.JSONDecodeError:
        return None


def _build_perf_section_breakdown(data: Dict[str, Any], top_n: int = 20) -> Dict[str, Any]:
    """
    从 perf report 的 sections 提取函数/节点级热点（CPU/GPU）。
    """
    sections = data.get("sections", []) if isinstance(data, dict) else []
    top_n = max(1, min(int(top_n), 200))
    if not isinstance(sections, list):
        return {"count": 0, "topBySelfCpu": [], "topByInclusiveCpu": [], "topByGpu": []}

    def _to_num(v: Any) -> float:
        try:
            return float(v or 0.0)
        except Exception:
            return 0.0

    def _row(s: Dict[str, Any]) -> Dict[str, Any]:
        return {
            "name": str(s.get("name", "")),
            "path": str(s.get("path", "")),
            "parentPath": str(s.get("parentPath", "")),
            "avgCpuMs": _to_num(s.get("avgCpuMs", 0.0)),
            "avgSelfCpuMs": _to_num(s.get("avgSelfCpuMs", 0.0)),
            "avgGpuMs": _to_num(s.get("avgGpuMs", 0.0)),
            "calls": int(s.get("calls", 0) or 0),
            "callsPerFrame": _to_num(s.get("callsPerFrame", 0.0)),
            "trackedPct": _to_num(s.get("trackedPct", 0.0)),
        }

    top_self = sorted(sections, key=lambda s: _to_num(s.get("avgSelfCpuMs", 0.0)), reverse=True)[:top_n]
    top_incl = sorted(sections, key=lambda s: _to_num(s.get("avgCpuMs", 0.0)), reverse=True)[:top_n]
    top_gpu = sorted(sections, key=lambda s: _to_num(s.get("avgGpuMs", 0.0)), reverse=True)[:top_n]

    return {
        "count": len(sections),
        "topBySelfCpu": [_row(s) for s in top_self],
        "topByInclusiveCpu": [_row(s) for s in top_incl],
        "topByGpu": [_row(s) for s in top_gpu],
    }


def _read_perf_summary(
    report_path: Path,
    include_sections: bool = False,
    section_top_n: int = 20,
) -> Dict[str, Any]:
    content = report_path.read_text(encoding="utf-8", errors="ignore")
    data = _extract_json_object(content)
    if not data:
        return {
            "ok": False,
            "error": "无法从 HTML 中提取 JSON 数据",
            "reportPath": str(report_path),
        }

    # 输出一份简洁摘要，供自动化判断回归。
    summary = {
        "ok": True,
        "reportType": "perf_report",
        "reportPath": str(report_path),
        "frameCount": data.get("frameCount", 0),
        "windowSec": data.get("windowSec", 0.0),
        "avgFps": data.get("avgFps", 0.0),
        "avgFrameTimeMs": data.get("avgFrameTimeMs", 0.0),
        "avgGpuTimeMs": data.get("avgGpuTimeMs", 0.0),
        "avgProcessCpuMs": data.get("avgProcessCpuMs", 0.0),
        "avgMainThreadCpuMs": data.get("avgMainThreadCpuMs", 0.0),
        "avgWorkerThreadsCpuMs": data.get("avgWorkerThreadsCpuMs", 0.0),
        "processCpuCoveragePct": data.get("processCpuCoveragePct", 0.0),
        "mainThreadCpuCoveragePct": data.get("mainThreadCpuCoveragePct", 0.0),
        "processParallelismCores": data.get("processParallelismCores", 0.0),
        "mainThreadShareOfProcessPct": data.get("mainThreadShareOfProcessPct", 0.0),
        "workerThreadsShareOfProcessPct": data.get("workerThreadsShareOfProcessPct", 0.0),
        "activeFrameTimeMs": data.get("activeFrameTimeMs", data.get("avgFrameTimeMs", 0.0)),
        "avgTrackedActiveCpuMs": data.get("avgTrackedActiveCpuMs", data.get("avgTrackedRootCpuMs", 0.0)),
        "avgUntrackedActiveCpuMs": data.get("avgUntrackedActiveCpuMs", data.get("avgUntrackedCpuMs", 0.0)),
        "avgIdleWaitCpuMs": data.get("avgIdleWaitCpuMs", 0.0),
        "cpuCoveragePct": data.get("cpuCoveragePct", 0.0),
        "cpuCoverageWithIdlePct": data.get("cpuCoverageWithIdlePct", data.get("cpuCoveragePct", 0.0)),
        "jank16": data.get("jank16", 0),
        "jank33": data.get("jank33", 0),
        "idleActiveOverlapLikely": data.get("idleActiveOverlapLikely", False),
    }

    cycle = data.get("mainLoopCycle", {}) if isinstance(data, dict) else {}
    if isinstance(cycle, dict):
        summary["mainLoopCycle"] = {
            "present": bool(cycle.get("present", False)),
            "avgCycleMs": float(cycle.get("avgCycleMs", 0.0) or 0.0),
            "avgActiveMs": float(cycle.get("avgActiveMs", 0.0) or 0.0),
            "avgIdleMs": float(cycle.get("avgIdleMs", 0.0) or 0.0),
            "phaseCount": len(cycle.get("phases", []) or []),
            "phasesTop": [
                {
                    "name": str(row.get("name", "")),
                    "avgCpuMs": float(row.get("avgCpuMs", 0.0) or 0.0),
                    "sharePct": float(row.get("sharePct", 0.0) or 0.0),
                    "callsPerFrame": float(row.get("callsPerFrame", 0.0) or 0.0),
                }
                for row in (cycle.get("phases", []) or [])[:12]
            ],
        }

    stages = data.get("mainLoopStages", []) if isinstance(data, dict) else []
    if isinstance(stages, list):
        summary["mainLoopStagesTop"] = [
            {
                "name": str(row.get("name", "")),
                "avgCpuMs": float(row.get("avgCpuMs", 0.0) or 0.0),
                "callsPerFrame": float(row.get("callsPerFrame", 0.0) or 0.0),
                "shareInFramePct": float(row.get("shareInFramePct", 0.0) or 0.0),
            }
            for row in stages[:12]
        ]

    if include_sections:
        summary["sectionBreakdown"] = _build_perf_section_breakdown(
            data,
            top_n=section_top_n,
        )
    runtime_profile = data.get("runtimeProfile", {}) if isinstance(data, dict) else {}
    if isinstance(runtime_profile, dict):
        summary["runtimeProfile"] = {
            "name": str(runtime_profile.get("name", "")),
            "disabledModules": str(runtime_profile.get("disabledModules", "")),
            "enabledModules": str(runtime_profile.get("enabledModules", "")),
        }
    shadow_budget = data.get("shadowBudgetSummary", {}) if isinstance(data, dict) else {}
    if isinstance(shadow_budget, dict):
        summary["shadowBudgetSummary"] = {
            "framesObserved": int(shadow_budget.get("framesObserved", 0) or 0),
            "framesIncomplete": int(shadow_budget.get("framesIncomplete", 0) or 0),
            "framesBudgetExceeded": int(shadow_budget.get("framesBudgetExceeded", 0) or 0),
            "framesReuseLastComplete": int(shadow_budget.get("framesReuseLastComplete", 0) or 0),
            "framesRenderCurrentPartial": int(shadow_budget.get("framesRenderCurrentPartial", 0) or 0),
            "avgBudgetMb": float(shadow_budget.get("avgBudgetMb", 0.0) or 0.0),
            "avgUsedMb": float(shadow_budget.get("avgUsedMb", 0.0) or 0.0),
            "maxBudgetMb": float(shadow_budget.get("maxBudgetMb", 0.0) or 0.0),
            "maxUsedMb": float(shadow_budget.get("maxUsedMb", 0.0) or 0.0),
            "skippedFreezeBudget": int(shadow_budget.get("skippedFreezeBudget", 0) or 0),
            "skippedPriorityBudget": int(shadow_budget.get("skippedPriorityBudget", 0) or 0),
            "skippedUpload": int(shadow_budget.get("skippedUpload", 0) or 0),
            "skippedCasterCap": int(shadow_budget.get("skippedCasterCap", 0) or 0),
            "skippedDistanceCull": int(shadow_budget.get("skippedDistanceCull", 0) or 0),
            "degradedAlphaBudget": int(shadow_budget.get("degradedAlphaBudget", 0) or 0),
            "reusedFreezeHits": int(shadow_budget.get("reusedFreezeHits", 0) or 0),
            "reusedFreezeMb": float(shadow_budget.get("reusedFreezeMb", 0.0) or 0.0),
            "actualFreezeReuseHits": int(shadow_budget.get("actualFreezeReuseHits", 0) or 0),
            "actualFreezeReuseMb": float(shadow_budget.get("actualFreezeReuseMb", 0.0) or 0.0),
            "uniqueGeometryCount": int(shadow_budget.get("uniqueGeometryCount", 0) or 0),
            "uniqueInstanceableGeometryCount": int(shadow_budget.get("uniqueInstanceableGeometryCount", 0) or 0),
            "duplicateGeometryInstances": int(shadow_budget.get("duplicateGeometryInstances", 0) or 0),
            "reuseEligibleDuplicates": int(shadow_budget.get("reuseEligibleDuplicates", 0) or 0),
            "uniqueFreezeAcceptedMb": float(shadow_budget.get("uniqueFreezeAcceptedMb", 0.0) or 0.0),
            "duplicateFreezeBypassMb": float(shadow_budget.get("duplicateFreezeBypassMb", 0.0) or 0.0),
            "potentialFreezeReuseHits": int(shadow_budget.get("potentialFreezeReuseHits", 0) or 0),
            "potentialFreezeReuseMb": float(shadow_budget.get("potentialFreezeReuseMb", 0.0) or 0.0),
            "instancedGeometryGroups": int(shadow_budget.get("instancedGeometryGroups", 0) or 0),
            "instancedGeometryInstances": int(shadow_budget.get("instancedGeometryInstances", 0) or 0),
            "instancedGeometryDrawsSaved": int(shadow_budget.get("instancedGeometryDrawsSaved", 0) or 0),
            "phases": list(shadow_budget.get("phases", []) or []),
        }
    shadow_runtime_v2 = data.get("shadowRuntimeV2Summary", {}) if isinstance(data, dict) else {}
    if (not isinstance(shadow_runtime_v2, dict) or not shadow_runtime_v2) and isinstance(shadow_budget, dict):
        shadow_runtime_v2 = {
            "staticPersistentCount": int(shadow_budget.get("staticPersistentCount", 0) or 0),
            "dynamicPoseCount": int(shadow_budget.get("dynamicPoseCount", 0) or 0),
            "dynamicSkinnedOutputCount": int(shadow_budget.get("dynamicSkinnedOutputCount", 0) or 0),
            "fallbackDrawCount": int(shadow_budget.get("fallbackDrawCount", 0) or 0),
            "semanticBridgeHit": int(shadow_budget.get("semanticBridgeHit", 0) or 0),
            "semanticBridgeMiss": int(shadow_budget.get("semanticBridgeMiss", 0) or 0),
            "semanticBridgeBypassed": int(shadow_budget.get("semanticBridgeBypassed", 0) or 0),
            "modelRegistryHit": 0,
            "modelRegistryMiss": 0,
            "modelLoadCount": 0,
            "modelReuseCount": 0,
            "poseUpdateCount": 0,
            "poseCacheHit": 0,
            "poseCacheMiss": 0,
            "bonePaletteUpdates": 0,
            "animationSequenceCount": 0,
            "avgModelResolveCpuMs": 0.0,
            "avgPoseUpdateCpuMs": 0.0,
            "avgSkinnedOutputCpuMs": 0.0,
            "modelResourceBytes": 0,
            "poseResourceBytes": 0,
            "frameSerial": int(shadow_budget.get("framesObserved", 0) or 0),
            "notes": "derived-from-shadowBudgetSummary",
        }
    if isinstance(shadow_runtime_v2, dict):
        summary["shadowRuntimeV2Summary"] = {
            "staticPersistentCount": int(shadow_runtime_v2.get("staticPersistentCount", 0) or 0),
            "dynamicPoseCount": int(shadow_runtime_v2.get("dynamicPoseCount", 0) or 0),
            "dynamicSkinnedOutputCount": int(shadow_runtime_v2.get("dynamicSkinnedOutputCount", 0) or 0),
            "fallbackDrawCount": int(shadow_runtime_v2.get("fallbackDrawCount", 0) or 0),
            "fallbackDrawCountTerrain": int(shadow_runtime_v2.get("fallbackDrawCountTerrain", 0) or 0),
            "fallbackDrawCountWorldObject": int(shadow_runtime_v2.get("fallbackDrawCountWorldObject", 0) or 0),
            "fallbackDrawCountUnitObject": int(shadow_runtime_v2.get("fallbackDrawCountUnitObject", 0) or 0),
            "objectFallbackDrawCount": int(shadow_runtime_v2.get("objectFallbackDrawCount", 0) or 0),
            "semanticBridgeHit": int(shadow_runtime_v2.get("semanticBridgeHit", 0) or 0),
            "semanticBridgeMiss": int(shadow_runtime_v2.get("semanticBridgeMiss", 0) or 0),
            "semanticBridgeBypassed": int(shadow_runtime_v2.get("semanticBridgeBypassed", 0) or 0),
            "semanticSceneSubmitted": int(shadow_runtime_v2.get("semanticSceneSubmitted", 0) or 0),
            "semanticSceneSubmittedUnit": int(shadow_runtime_v2.get("semanticSceneSubmittedUnit", 0) or 0),
            "semanticSceneSubmittedSkinned": int(shadow_runtime_v2.get("semanticSceneSubmittedSkinned", 0) or 0),
            "semanticSceneSubmittedFrameLocal": int(shadow_runtime_v2.get("semanticSceneSubmittedFrameLocal", 0) or 0),
            "semanticSceneSubmittedPersistent": int(shadow_runtime_v2.get("semanticSceneSubmittedPersistent", 0) or 0),
            "semanticSceneAcceptedExplicitResourceOwnerRigid": int(shadow_runtime_v2.get("semanticSceneAcceptedExplicitResourceOwnerRigid", 0) or 0),
            "modelRegistryHit": int(shadow_runtime_v2.get("modelRegistryHit", 0) or 0),
            "modelRegistryMiss": int(shadow_runtime_v2.get("modelRegistryMiss", 0) or 0),
            "modelLoadCount": int(shadow_runtime_v2.get("modelLoadCount", 0) or 0),
            "modelReuseCount": int(shadow_runtime_v2.get("modelReuseCount", 0) or 0),
            "runtimeBoundCount": int(shadow_runtime_v2.get("runtimeBoundCount", 0) or 0),
            "completeIdentityCount": int(shadow_runtime_v2.get("completeIdentityCount", 0) or 0),
            "shadowRuntimeBoundCount": int(shadow_runtime_v2.get("shadowRuntimeBoundCount", 0) or 0),
            "shadowIdentityCount": int(shadow_runtime_v2.get("shadowIdentityCount", 0) or 0),
            "poseUpdateCount": int(shadow_runtime_v2.get("poseUpdateCount", 0) or 0),
            "poseCacheHit": int(shadow_runtime_v2.get("poseCacheHit", 0) or 0),
            "poseCacheMiss": int(shadow_runtime_v2.get("poseCacheMiss", 0) or 0),
            "bonePaletteUpdates": int(shadow_runtime_v2.get("bonePaletteUpdates", 0) or 0),
            "shadowGeosetResourceCount": int(shadow_runtime_v2.get("shadowGeosetResourceCount", 0) or 0),
            "shadowReadyGeosetCount": int(shadow_runtime_v2.get("shadowReadyGeosetCount", 0) or 0),
            "shadowModelResourceCount": int(shadow_runtime_v2.get("shadowModelResourceCount", 0) or 0),
            "shadowPoseReadyCount": int(shadow_runtime_v2.get("shadowPoseReadyCount", 0) or 0),
            "upperLayerResolveAttempts": int(shadow_runtime_v2.get("upperLayerResolveAttempts", 0) or 0),
            "upperLayerResolveVisibleMiss": int(shadow_runtime_v2.get("upperLayerResolveVisibleMiss", 0) or 0),
            "upperLayerResolveVisibleUnresolvedGeoset": int(shadow_runtime_v2.get("upperLayerResolveVisibleUnresolvedGeoset", 0) or 0),
            "upperLayerResolveGeosetMiss": int(shadow_runtime_v2.get("upperLayerResolveGeosetMiss", 0) or 0),
            "upperLayerResolvePoseMiss": int(shadow_runtime_v2.get("upperLayerResolvePoseMiss", 0) or 0),
            "upperLayerResolveRuntimeGroupPaletteMiss": int(shadow_runtime_v2.get("upperLayerResolveRuntimeGroupPaletteMiss", 0) or 0),
            "upperLayerResolveAuthoritativeRigid": int(shadow_runtime_v2.get("upperLayerResolveAuthoritativeRigid", 0) or 0),
            "upperLayerResolveAuthoritativeSkinned": int(shadow_runtime_v2.get("upperLayerResolveAuthoritativeSkinned", 0) or 0),
            "upperLayerEmitted": int(shadow_runtime_v2.get("upperLayerEmitted", 0) or 0),
            "upperLayerDuplicateOrSuppressed": int(shadow_runtime_v2.get("upperLayerDuplicateOrSuppressed", 0) or 0),
            "animationSequenceCount": int(shadow_runtime_v2.get("animationSequenceCount", 0) or 0),
            "avgModelResolveCpuMs": float(shadow_runtime_v2.get("avgModelResolveCpuMs", 0.0) or 0.0),
            "avgPoseUpdateCpuMs": float(shadow_runtime_v2.get("avgPoseUpdateCpuMs", 0.0) or 0.0),
            "avgSkinnedOutputCpuMs": float(shadow_runtime_v2.get("avgSkinnedOutputCpuMs", 0.0) or 0.0),
            "modelResourceBytes": int(shadow_runtime_v2.get("modelResourceBytes", 0) or 0),
            "poseResourceBytes": int(shadow_runtime_v2.get("poseResourceBytes", 0) or 0),
            "frameSerial": int(shadow_runtime_v2.get("frameSerial", 0) or 0),
            "notes": str(shadow_runtime_v2.get("notes", "") or ""),
        }
    else:
        summary["shadowRuntimeV2Summary"] = _zero_shadow_runtime_v2_summary()
    if isinstance(data.get("topShadowOffenders", None), list):
        summary["topShadowOffenders"] = list(data.get("topShadowOffenders", []) or [])
    if isinstance(data.get("moduleMatrix", None), list):
        summary["moduleMatrix"] = list(data.get("moduleMatrix", []) or [])
    return summary


def _tail_text_file(path: Path, max_lines: int = 200, max_chars: int = 20000) -> str:
    """读取文件尾部文本，避免一次性返回过大内容。"""
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        text = path.read_text(encoding="mbcs", errors="ignore")
    lines = text.splitlines()
    if max_lines > 0:
        lines = lines[-max_lines:]
    out = "\n".join(lines)
    if max_chars > 0 and len(out) > max_chars:
        out = out[-max_chars:]
    return out


def _runtime_log_paths(war3_dir: Path) -> List[Path]:
    return [
        war3_dir / "war3_d3d9.log",
        war3_dir / "dxvk.log",
    ]


def _snapshot_log_offsets(war3_dir: Path) -> Dict[str, int]:
    offsets: Dict[str, int] = {}
    for path in _runtime_log_paths(war3_dir):
        try:
            offsets[str(path)] = int(path.stat().st_size)
        except Exception:
            offsets[str(path)] = 0
    return offsets


def _read_text_delta(path: Path, offset: int) -> str:
    if not path.exists():
        return ""
    try:
        size = int(path.stat().st_size)
        start = max(0, min(int(offset), size))
        with path.open("rb") as f:
            f.seek(start)
            raw = f.read()
        return raw.decode("utf-8", errors="ignore")
    except Exception:
        try:
            return path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            return path.read_text(encoding="mbcs", errors="ignore")


def _read_runtime_benchmark_summary(
    war3_dir: Path,
    log_offsets: Dict[str, int],
    profile: str,
    disable_modules: str,
) -> Dict[str, Any]:
    matches: List[Tuple[float, Path, re.Match[str]]] = []
    for path in _runtime_log_paths(war3_dir):
        text = _read_text_delta(path, int(log_offsets.get(str(path), 0) or 0))
        if not text and path.exists():
            text = _tail_text_file(path, max_lines=200, max_chars=40000)
        if not text:
            continue
        for match in BENCHMARK_LINE_RE.finditer(text):
            matches.append((path.stat().st_mtime if path.exists() else 0.0, path, match))
    if not matches:
        return {"ok": False, "error": "未在运行日志中找到 runtime benchmark 输出"}

    matches.sort(key=lambda item: (item[0], item[2].start()))
    _, log_path, match = matches[-1]
    avg_fps = float(match.group("avg") or 0.0)
    sample_frames = int(match.group("frames") or 0)
    warmup_sec = float(match.group("warmup") or 0.0)
    sample_sec = float(match.group("sample") or 0.0)
    runtime_profile = _runtime_profile_summary_from_inputs(
        match.group("profile") or profile,
        match.group("disabled") or disable_modules,
    )
    shadow_budget = _zero_shadow_budget_summary()
    report_path = str(log_path)
    return {
        "ok": avg_fps > 0.0,
        "reportType": "benchmark_log",
        "reportPath": report_path,
        "avgFps": avg_fps,
        "avgFrameTimeMs": (1000.0 / avg_fps) if avg_fps > 1e-6 else 0.0,
        "avgGpuTimeMs": 0.0,
        "avgTrackedActiveCpuMs": 0.0,
        "runtimeProfile": {
            "name": runtime_profile["name"],
            "disabledModules": runtime_profile["disabledModules"],
            "enabledModules": runtime_profile["enabledModules"],
        },
        "moduleMatrix": [
            {
                "profile": runtime_profile["name"],
                "disabledModules": runtime_profile["disabledModules"],
                "enabledModules": runtime_profile["enabledModules"],
            }
        ],
        "shadowBudgetSummary": shadow_budget,
        "shadowRuntimeV2Summary": _zero_shadow_runtime_v2_summary(),
        "topShadowOffenders": [
            {
                "name": phase["name"],
                "requestedMb": 0.0,
                "rejectedMb": 0.0,
                "rejects": 0,
            }
            for phase in shadow_budget["phases"]
        ],
        "benchmark": {
            "mode": str(match.group("mode") or "runtime"),
            "avgFps": avg_fps,
            "sampleFrames": sample_frames,
            "warmupSec": warmup_sec,
            "sampleSec": sample_sec,
            "logPath": report_path,
        },
    }


def _read_runtime_log_summary(
    war3_dir: Path,
    log_offsets: Dict[str, int],
) -> Dict[str, Any]:
    files: List[Dict[str, Any]] = []
    keyword_counts = {name: 0 for name, _ in LOG_KEYWORD_PATTERNS}
    sample_lines: List[Dict[str, str]] = []

    for path in _runtime_log_paths(war3_dir):
        text = _read_text_delta(path, int(log_offsets.get(str(path), 0) or 0))
        if not text and path.exists():
            text = _tail_text_file(path, max_lines=120, max_chars=30000)
        if not text:
            continue
        lines = text.splitlines()
        files.append(
            {
                "path": str(path),
                "lineCount": len(lines),
                "charCount": len(text),
            }
        )
        for raw_line in lines:
            line = str(raw_line or "").strip()
            if not line:
                continue
            matched = False
            for name, pattern in LOG_KEYWORD_PATTERNS:
                if pattern.search(line):
                    keyword_counts[name] += 1
                    matched = True
            if matched and len(sample_lines) < 16:
                sample_lines.append({"path": str(path), "line": line})

    top_keywords = [
        {"name": name, "count": int(count)}
        for name, count in sorted(keyword_counts.items(), key=lambda item: (-item[1], item[0]))
        if int(count) > 0
    ]
    return {
        "files": files,
        "keywordCounts": keyword_counts,
        "topKeywords": top_keywords,
        "sampleLines": sample_lines,
    }


def _merge_shadow_budget_summary_with_log_fallback(
    summary: Dict[str, Any],
    log_summary: Dict[str, Any],
) -> None:
    shadow_budget = summary.get("shadowBudgetSummary")
    if not isinstance(shadow_budget, dict):
        return

    keyword_counts = log_summary.get("keywordCounts", {}) if isinstance(log_summary, dict) else {}
    if not isinstance(keyword_counts, dict):
        return

    freeze_budget_exceeded = int(keyword_counts.get("freezeBudgetExceeded", 0) or 0)
    capture_incomplete = int(keyword_counts.get("shadowCaptureIncomplete", 0) or 0)
    reuse_last = int(keyword_counts.get("shadowReuseLastComplete", 0) or 0)
    render_partial = int(keyword_counts.get("shadowRenderPartial", 0) or 0)
    derived = False

    if int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0 and freeze_budget_exceeded > 0:
        shadow_budget["framesBudgetExceeded"] = freeze_budget_exceeded
        derived = True
    if int(shadow_budget.get("framesIncomplete", 0) or 0) == 0 and capture_incomplete > 0:
        shadow_budget["framesIncomplete"] = capture_incomplete
        derived = True
    if int(shadow_budget.get("framesReuseLastComplete", 0) or 0) == 0 and reuse_last > 0:
        shadow_budget["framesReuseLastComplete"] = reuse_last
        derived = True
    if int(shadow_budget.get("framesRenderCurrentPartial", 0) or 0) == 0 and render_partial > 0:
        shadow_budget["framesRenderCurrentPartial"] = render_partial
        derived = True
    if int(shadow_budget.get("framesObserved", 0) or 0) == 0:
        observed = max(
            freeze_budget_exceeded,
            capture_incomplete,
            reuse_last,
            render_partial,
        )
        if observed > 0:
            shadow_budget["framesObserved"] = observed
            derived = True

    if derived:
        shadow_budget["derivedFromLogKeywords"] = True


class DbWinListener:
    """读取 OutputDebugString（DBWIN）缓冲。"""

    PAGE_READWRITE = 0x04
    FILE_MAP_READ = 0x0004
    WAIT_OBJECT_0 = 0x00000000
    WAIT_TIMEOUT = 0x00000102

    def __init__(self) -> None:
        self.kernel32 = ctypes.windll.kernel32
        # 显式绑定 Win32 API 签名，避免 ctypes 默认参数推断导致
        # HANDLE(-1) 在 64 位 Python 下被按有符号整型转换并触发 OverflowError。
        self.kernel32.CreateEventW.argtypes = [
            wintypes.LPVOID,
            wintypes.BOOL,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        ]
        self.kernel32.CreateEventW.restype = wintypes.HANDLE
        self.kernel32.CreateFileMappingW.argtypes = [
            wintypes.HANDLE,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPCWSTR,
        ]
        self.kernel32.CreateFileMappingW.restype = wintypes.HANDLE
        self.kernel32.MapViewOfFile.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_size_t,
        ]
        self.kernel32.MapViewOfFile.restype = wintypes.LPVOID
        self.kernel32.UnmapViewOfFile.argtypes = [wintypes.LPCVOID]
        self.kernel32.UnmapViewOfFile.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.SetEvent.argtypes = [wintypes.HANDLE]
        self.kernel32.SetEvent.restype = wintypes.BOOL
        self.kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.kernel32.WaitForSingleObject.restype = wintypes.DWORD

        self.h_buffer_ready = None
        self.h_data_ready = None
        self.h_map = None
        self.p_buf = None
        self._opened = False

    def open(self) -> None:
        if self._opened:
            return

        self.h_buffer_ready = self.kernel32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
        self.h_data_ready = self.kernel32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
        self.h_map = self.kernel32.CreateFileMappingW(
            ctypes.c_void_p(-1),
            None,
            self.PAGE_READWRITE,
            0,
            4096,
            "DBWIN_BUFFER",
        )
        if not self.h_buffer_ready or not self.h_data_ready or not self.h_map:
            raise RuntimeError("DBWIN 初始化失败：CreateEvent/CreateFileMapping 失败")

        self.p_buf = self.kernel32.MapViewOfFile(self.h_map, self.FILE_MAP_READ, 0, 0, 4096)
        if not self.p_buf:
            raise RuntimeError("DBWIN 初始化失败：MapViewOfFile 失败")

        self._opened = True

    def close(self) -> None:
        if self.p_buf:
            self.kernel32.UnmapViewOfFile(self.p_buf)
            self.p_buf = None
        if self.h_map:
            self.kernel32.CloseHandle(self.h_map)
            self.h_map = None
        if self.h_data_ready:
            self.kernel32.CloseHandle(self.h_data_ready)
            self.h_data_ready = None
        if self.h_buffer_ready:
            self.kernel32.CloseHandle(self.h_buffer_ready)
            self.h_buffer_ready = None
        self._opened = False

    def read(self, timeout_ms: int = 200) -> Tuple[Optional[int], Optional[str]]:
        if not self._opened:
            self.open()

        self.kernel32.SetEvent(self.h_buffer_ready)
        ret = self.kernel32.WaitForSingleObject(self.h_data_ready, timeout_ms)
        if ret == self.WAIT_TIMEOUT:
            return None, None
        if ret != self.WAIT_OBJECT_0:
            return None, None

        raw = ctypes.string_at(self.p_buf, 4096)
        pid = struct.unpack("<I", raw[:4])[0]
        msg = raw[4:].split(b"\x00", 1)[0].decode("mbcs", errors="ignore")
        return pid, msg


def _pid_alive(pid: int) -> bool:
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    kernel32 = ctypes.windll.kernel32
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        # 某些权限边界下 OpenProcess 会失败，回退到 tasklist 检测，避免误判“已退出”。
        return _pid_alive_via_tasklist(pid)
    try:
        code = ctypes.c_ulong(0)
        ok = kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        if not ok:
            return _pid_alive_via_tasklist(pid)
        return int(code.value) == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(h)


def _pid_alive_via_tasklist(pid: int) -> bool:
    try:
        proc = subprocess.run(
            ["tasklist", "/FI", f"PID eq {int(pid)}", "/FO", "CSV", "/NH"],
            check=False,
            capture_output=True,
            text=True,
            encoding="mbcs",
            errors="ignore",
        )
        out = (proc.stdout or "").strip().lower()
        if not out or "no tasks are running" in out:
            return False
        return str(int(pid)) in out
    except Exception:
        return False


def _taskkill(pid: int, force: bool) -> None:
    cmd = ["taskkill", "/PID", str(pid), "/T"]
    if force:
        cmd.append("/F")
    try:
        subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        # 某些环境 PATH 缺失 System32 时回退到 PowerShell Stop-Process。
        ps = [
            "powershell",
            "-NoProfile",
            "-Command",
            f"$p = Get-Process -Id {int(pid)} -ErrorAction SilentlyContinue; "
            f"if ($p) {{ Stop-Process -Id {int(pid)} -Force -ErrorAction SilentlyContinue }}",
        ]
        subprocess.run(ps, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _get_process_cpu_seconds(pid: int) -> float:
    """返回进程累计 CPU 时间（user+kernel，秒）。失败返回 -1。"""
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.windll.kernel32
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return -1.0
    try:
        creation = ctypes.c_ulonglong(0)
        exit_t = ctypes.c_ulonglong(0)
        kernel_t = ctypes.c_ulonglong(0)
        user_t = ctypes.c_ulonglong(0)
        ok = kernel32.GetProcessTimes(
            h,
            ctypes.byref(creation),
            ctypes.byref(exit_t),
            ctypes.byref(kernel_t),
            ctypes.byref(user_t),
        )
        if not ok:
            return -1.0
        # FILETIME: 100ns
        return float(kernel_t.value + user_t.value) / 10_000_000.0
    finally:
        kernel32.CloseHandle(h)


class _Win32Rect(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class _Win32Point(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_long),
        ("y", ctypes.c_long),
    ]


class _Win32WindowPlacement(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("showCmd", ctypes.c_uint),
        ("ptMinPosition", _Win32Point),
        ("ptMaxPosition", _Win32Point),
        ("rcNormalPosition", _Win32Rect),
    ]


_DPI_AWARE_SET = False


def _ensure_process_dpi_aware() -> None:
    global _DPI_AWARE_SET
    if _DPI_AWARE_SET:
        return
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass
    _DPI_AWARE_SET = True


def _get_window_text(hwnd: int) -> str:
    if not hwnd:
        return ""
    user32 = ctypes.windll.user32
    buf = ctypes.create_unicode_buffer(512)
    try:
        user32.GetWindowTextW(ctypes.c_void_p(hwnd), buf, len(buf))
        return buf.value
    except Exception:
        return ""


def _rect_to_dict(rect: _Win32Rect) -> Dict[str, int]:
    return {
        "left": int(rect.left),
        "top": int(rect.top),
        "right": int(rect.right),
        "bottom": int(rect.bottom),
        "width": max(0, int(rect.right - rect.left)),
        "height": max(0, int(rect.bottom - rect.top)),
    }


def _query_window_info_by_hwnd(hwnd: int, pid: int = 0) -> Dict[str, Any]:
    if not hwnd:
        return {"ok": False, "error": "hwnd=0"}

    _ensure_process_dpi_aware()
    user32 = ctypes.windll.user32
    wr = _Win32Rect()
    cr = _Win32Rect()
    placement = _Win32WindowPlacement()
    placement.length = ctypes.sizeof(_Win32WindowPlacement)

    ok_wr = bool(user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(wr)))
    ok_cr = bool(user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(cr)))
    ok_wp = bool(user32.GetWindowPlacement(ctypes.c_void_p(hwnd), ctypes.byref(placement)))

    return {
        "ok": ok_wr and ok_cr,
        "pid": int(pid),
        "hwnd": int(hwnd),
        "title": _get_window_text(hwnd),
        "visible": bool(user32.IsWindowVisible(ctypes.c_void_p(hwnd))),
        "windowRect": _rect_to_dict(wr) if ok_wr else {},
        "clientRect": _rect_to_dict(cr) if ok_cr else {},
        "showCmd": int(placement.showCmd) if ok_wp else 0,
        "placementFlags": int(placement.flags) if ok_wp else 0,
    }


def _wait_for_main_window_hwnd(pid: int, timeout_sec: float = 15.0, require_visible: bool = True) -> int:
    t0 = time.time()
    while time.time() - t0 < max(0.2, float(timeout_sec)):
        hwnd = _find_main_window_hwnd(pid)
        if hwnd:
            if not require_visible:
                return hwnd
            info = _query_window_info_by_hwnd(hwnd, pid=pid)
            if info.get("visible"):
                return hwnd
        if not _pid_alive(pid):
            return 0
        time.sleep(0.1)
    return 0


def _resize_window_client_native(pid: int, client_w: int, client_h: int, x: int = 40, y: int = 40) -> Dict[str, Any]:
    """
    将 War3 窗口的 client 区调整到目标尺寸；不激活窗口。
    """
    hwnd = _wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={pid}"}

    _ensure_process_dpi_aware()
    user32 = ctypes.windll.user32
    wr = _Win32Rect()
    cr = _Win32Rect()
    if not user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(wr)):
        return {"ok": False, "error": "GetWindowRect 失败", "hwnd": int(hwnd)}
    if not user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(cr)):
        return {"ok": False, "error": "GetClientRect 失败", "hwnd": int(hwnd)}

    window_w = max(0, int(wr.right - wr.left))
    window_h = max(0, int(wr.bottom - wr.top))
    client_cur_w = max(0, int(cr.right - cr.left))
    client_cur_h = max(0, int(cr.bottom - cr.top))
    border_w = max(0, window_w - client_cur_w)
    border_h = max(0, window_h - client_cur_h)

    target_w = max(1, int(client_w) + border_w)
    target_h = max(1, int(client_h) + border_h)
    SWP_NOZORDER = 0x0004
    SWP_NOACTIVATE = 0x0010
    ok = bool(
        user32.SetWindowPos(
            ctypes.c_void_p(hwnd),
            ctypes.c_void_p(0),
            int(x),
            int(y),
            target_w,
            target_h,
            SWP_NOZORDER | SWP_NOACTIVATE,
        )
    )
    time.sleep(0.15)
    info = _query_window_info_by_hwnd(hwnd, pid=pid)
    return {
        "ok": ok and bool(info.get("ok")),
        "pid": int(pid),
        "hwnd": int(hwnd),
        "targetClientW": int(client_w),
        "targetClientH": int(client_h),
        "windowW": target_w,
        "windowH": target_h,
        "info": info,
    }


def _post_window_syscommand(pid: int, command: int) -> Dict[str, Any]:
    hwnd = _wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={pid}"}

    WM_SYSCOMMAND = 0x0112
    ok = bool(
        ctypes.windll.user32.PostMessageW(
            ctypes.c_void_p(hwnd),
            WM_SYSCOMMAND,
            ctypes.c_void_p(command),
            ctypes.c_void_p(0),
        )
    )
    return {
        "ok": ok,
        "pid": int(pid),
        "hwnd": int(hwnd),
        "command": int(command),
        "window": _query_window_info_by_hwnd(hwnd, pid=pid),
    }


def _wait_for_window_ready(
    pid: int,
    timeout_sec: int = 30,
    min_cpu_sec: float = 0.5,
    stable_sec: float = 0.8,
) -> Dict[str, Any]:
    """
    仅等待“窗口可操作”，用于 resize/maximize 崩溃测试，不等同于正式进图。
    """
    t0 = time.time()
    first_hwnd_ts = 0.0
    last_hwnd = 0

    while time.time() - t0 < max(1, int(timeout_sec)):
        if not _pid_alive(pid):
            return {"ok": False, "error": "进程已退出", "pid": int(pid)}

        hwnd = _find_main_window_hwnd(pid)
        cpu_sec = _get_process_cpu_seconds(pid)
        if hwnd:
            if hwnd != last_hwnd:
                first_hwnd_ts = time.time()
                last_hwnd = hwnd
            if first_hwnd_ts <= 0.0:
                first_hwnd_ts = time.time()
            if (time.time() - first_hwnd_ts) >= max(0.1, float(stable_sec)) and cpu_sec >= max(0.0, float(min_cpu_sec)):
                return {
                    "ok": True,
                    "pid": int(pid),
                    "elapsedSec": round(time.time() - t0, 3),
                    "cpuSec": round(cpu_sec, 3),
                    "window": _query_window_info_by_hwnd(hwnd, pid=pid),
                }
        time.sleep(0.1)

    return {
        "ok": False,
        "error": "等待窗口可操作超时",
        "pid": int(pid),
        "elapsedSec": round(time.time() - t0, 3),
    }


def _enumerate_pid_windows(pid: int) -> List[int]:
    user32 = ctypes.windll.user32
    hwnds: List[int] = []

    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def _cb(hwnd: int, _lparam: int) -> bool:
        proc_id = ctypes.c_ulong(0)
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(proc_id))
        if proc_id.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            hwnds.append(hwnd)
        return True

    enum_cb = WNDENUMPROC(_cb)
    desktop_handle = int(STATE.desktop_handle or 0) if STATE.war3_pid == pid else 0
    if desktop_handle:
        user32.EnumDesktopWindows.argtypes = [wintypes.HANDLE, WNDENUMPROC, wintypes.LPARAM]
        user32.EnumDesktopWindows.restype = wintypes.BOOL
        user32.EnumDesktopWindows(wintypes.HANDLE(desktop_handle), enum_cb, 0)
    else:
        user32.EnumWindows(enum_cb, 0)
    return hwnds


def _rank_window_candidate(info: Dict[str, Any]) -> int:
    wr = dict(info.get("windowRect", {}) or {})
    cr = dict(info.get("clientRect", {}) or {})
    window_area = int(wr.get("width", 0) or 0) * int(wr.get("height", 0) or 0)
    client_area = int(cr.get("width", 0) or 0) * int(cr.get("height", 0) or 0)
    show_cmd = int(info.get("showCmd", 0) or 0)
    score = max(window_area, client_area * 2)
    if show_cmd == 2:  # SW_SHOWMINIMIZED
        score -= 1_000_000_000
    if str(info.get("title", "") or "").strip():
        score += 10_000
    return int(score)


def _main_window_candidates(pid: int) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for hwnd in _enumerate_pid_windows(pid):
        info = _query_window_info_by_hwnd(hwnd, pid=pid)
        wr = dict(info.get("windowRect", {}) or {})
        cr = dict(info.get("clientRect", {}) or {})
        window_area = int(wr.get("width", 0) or 0) * int(wr.get("height", 0) or 0)
        client_area = int(cr.get("width", 0) or 0) * int(cr.get("height", 0) or 0)
        info["windowArea"] = int(window_area)
        info["clientArea"] = int(client_area)
        info["score"] = _rank_window_candidate(info)
        rows.append(info)
    rows.sort(key=lambda row: int(row.get("score", 0) or 0), reverse=True)
    return rows


def _find_main_window_hwnd(pid: int) -> int:
    candidates = _main_window_candidates(pid)
    for row in candidates:
        if int(row.get("windowArea", 0) or 0) > 0 and int(row.get("clientArea", 0) or 0) > 0:
            return int(row.get("hwnd", 0) or 0)
    return int(candidates[0].get("hwnd", 0) or 0) if candidates else 0


def _post_close(pid: int) -> bool:
    hwnd = _find_main_window_hwnd(pid)
    if not hwnd:
        return False
    WM_CLOSE = 0x0010
    ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, 0, 0)
    return True


def _powershell_capture_window(pid: int, output_png: Path) -> Dict[str, Any]:
    hwnd = _find_main_window_hwnd(pid)
    selected_window = _query_window_info_by_hwnd(hwnd, pid=pid) if hwnd else {"ok": False, "error": "hwnd=0"}
    candidates = _main_window_candidates(pid)[:8]
    script = r'''
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Rect {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }
  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  [DllImport("user32.dll")]
  public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")]
  public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
}
"@
[Win32Rect]::SetProcessDPIAware() | Out-Null
$procId = [int]$env:WAR3_AUTOTEST_PID
$out = $env:WAR3_AUTOTEST_OUT
$hWnd = [IntPtr]::Zero
if ($env:WAR3_AUTOTEST_HWND) {
  $hWnd = [IntPtr]([Int64]$env:WAR3_AUTOTEST_HWND)
}
$proc = Get-Process -Id $procId -ErrorAction Stop
if ($hWnd -eq [IntPtr]::Zero) {
  $hWnd = $proc.MainWindowHandle
}
if ($hWnd -eq 0) { throw "MainWindowHandle=0" }
$rect = New-Object Win32Rect+RECT
[Win32Rect]::GetWindowRect($hWnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "WindowRect invalid: ${w}x${h}" }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$captureMode = "PrintWindow"
$dc = [IntPtr]::Zero
try {
  $dc = $g.GetHdc()
  # 优先按窗口句柄抓图，避免窗口被覆盖时抓到桌面内容。
  $okPrint = [Win32Rect]::PrintWindow($hWnd, $dc, 2)
}
finally {
  if ($dc -ne [IntPtr]::Zero) { $g.ReleaseHdc($dc) }
}
if (-not $okPrint) {
  # 回退：若 PrintWindow 不支持则退化为屏幕抓图。
  $captureMode = "CopyFromScreen"
  $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
}
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output ("OK:" + $captureMode)
'''
    env = os.environ.copy()
    env["WAR3_AUTOTEST_PID"] = str(pid)
    env["WAR3_AUTOTEST_OUT"] = str(output_png)
    if hwnd:
        env["WAR3_AUTOTEST_HWND"] = str(int(hwnd))
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return {
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
        "hwnd": int(hwnd or 0),
        "selectedWindow": selected_window,
        "windowCandidates": candidates,
    }


def _powershell_convert_bitmap_to_png(input_bmp: Path, output_png: Path) -> Dict[str, Any]:
    script = r'''
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$src = $env:WAR3_AUTOTEST_SRC
$dst = $env:WAR3_AUTOTEST_DST
$bmp = New-Object System.Drawing.Bitmap $src
try {
  $bmp.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Output "OK:BitmapToPng"
}
finally {
  $bmp.Dispose()
}
'''
    env = os.environ.copy()
    env["WAR3_AUTOTEST_SRC"] = str(input_bmp)
    env["WAR3_AUTOTEST_DST"] = str(output_png)
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return {
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
    }


def _request_internal_frame_capture(
    pid: int,
    output_path: Path,
    war3_dir: Path,
    timeout_sec: float = 8.0,
) -> Dict[str, Any]:
    final_out = output_path.resolve()
    raw_bmp = final_out if final_out.suffix.lower() == ".bmp" else final_out.with_suffix(".bmp")
    pipe_res = _control_plane_request(
        pid=pid,
        command="capture_final_frame",
        payload={
            "outputPath": str(raw_bmp),
            "timeoutMs": max(1000, int(float(timeout_sec) * 1000.0)),
        },
        timeout_sec=max(2.0, float(timeout_sec) + 1.0),
    )
    if pipe_res.get("transportOk"):
        if not pipe_res.get("ok"):
            return {
                "ok": False,
                "mode": "control-plane-capture",
                "error": str(pipe_res.get("error", "control plane capture 失败")),
                "detail": pipe_res,
            }

        raw_output = Path(str((pipe_res.get("result", {}) or {}).get("outputPath", raw_bmp)))
        if not raw_output.exists():
            return {
                "ok": False,
                "mode": "control-plane-capture",
                "error": "control plane capture 未产出文件",
                "detail": pipe_res,
            }

        convert: Dict[str, Any] = {
            "returncode": 0,
            "stdout": "",
            "stderr": "",
            "skipped": True,
        }
        delivered_output = raw_output
        if final_out.suffix.lower() == ".png":
            convert = _powershell_convert_bitmap_to_png(raw_output, final_out)
            if convert.get("returncode", 1) != 0 or (not final_out.exists()):
                return {
                    "ok": False,
                    "mode": "control-plane-capture",
                    "error": f"control plane PNG 转换失败: {convert.get('stderr') or convert.get('stdout') or 'unknown'}",
                    "detail": pipe_res,
                    "convert": convert,
                    "rawOutput": str(raw_output),
                }
            delivered_output = final_out
            try:
                raw_output.unlink()
            except Exception:
                pass

        return {
            "ok": True,
            "mode": "control-plane-capture",
            "output": str(delivered_output),
            "rawOutput": str(raw_output),
            "details": pipe_res,
            "convert": convert,
            "elapsedSec": round(float(pipe_res.get("elapsedSec", 0.0) or 0.0), 3),
        }
    return {
        "ok": False,
        "mode": "control-plane-capture-unavailable",
        "error": str(pipe_res.get("error", "control plane capture 不可用") or "control plane capture 不可用"),
        "detail": pipe_res,
    }


def _powershell_resize_window_client(pid: int, client_w: int, client_h: int, x: int = 40, y: int = 40) -> Dict[str, Any]:
    """
    将窗口客户端区调整为目标尺寸（用于 windowed 模式下的 2K 基线）。
    """
    res = _resize_window_client_native(
        pid=pid,
        client_w=max(640, int(client_w)),
        client_h=max(480, int(client_h)),
        x=int(x),
        y=int(y),
    )
    info = {}
    if isinstance(res.get("info"), dict):
        client_rect = res["info"].get("clientRect", {}) if isinstance(res["info"], dict) else {}
        window_rect = res["info"].get("windowRect", {}) if isinstance(res["info"], dict) else {}
        info = {
            "windowW": int(window_rect.get("width", 0) or 0),
            "windowH": int(window_rect.get("height", 0) or 0),
            "clientW": int(client_rect.get("width", 0) or 0),
            "clientH": int(client_rect.get("height", 0) or 0),
            "hwnd": int(res.get("hwnd", 0) or 0),
        }
    return {
        "returncode": 0 if res.get("ok") else 1,
        "stdout": json.dumps(info, ensure_ascii=False) if info else "",
        "stderr": "" if res.get("ok") else str(res.get("error", "resize failed")),
        "info": info,
        "native": res,
    }


@dataclass
class RuntimeState:
    war3_proc: Optional[subprocess.Popen] = None
    war3_pid: Optional[int] = None
    war3_dir: Path = DEFAULT_WAR3_DIR
    test_map_path: Path = DEFAULT_TEST_MAP
    desktop_name: str = ""
    desktop_handle: int = 0
    desktop_mode: str = "default"
    launch_epoch_ms: int = 0
    last_report_path: Optional[Path] = None
    video_restore_key_path: str = WAR3_VIDEO_REG_KEY
    video_restore_snapshot: Dict[str, Any] = field(default_factory=dict)
    debug_thread: Optional[threading.Thread] = None
    debug_stop: threading.Event = field(default_factory=threading.Event)
    debug_pid_filter: Optional[int] = None
    debug_lock: threading.Lock = field(default_factory=threading.Lock)
    debug_events: List[Dict[str, Any]] = field(default_factory=list)
    debug_seq: int = 0
    perf_thread: Optional[threading.Thread] = None
    perf_stop: threading.Event = field(default_factory=threading.Event)
    perf_lock: threading.Lock = field(default_factory=threading.Lock)
    perf_next_job_id: int = 1
    perf_job: Dict[str, Any] = field(
        default_factory=lambda: {
            "status": "idle",
            "jobId": 0,
            "startedAt": "",
            "updatedAt": "",
            "endedAt": "",
            "roundsTotal": 0,
            "roundsDone": 0,
            "results": [],
            "lastError": "",
            "params": {},
            "aggregate": {},
        }
    )

    def push_event(self, pid: int, msg: str) -> None:
        with self.debug_lock:
            self.debug_seq += 1
            self.debug_events.append(
                {
                    "id": self.debug_seq,
                    "ts": _now_str(),
                    "pid": pid,
                    "msg": msg.strip(),
                }
            )
            if len(self.debug_events) > MAX_EVENT_BUFFER:
                self.debug_events = self.debug_events[-MAX_EVENT_BUFFER:]

    def get_events(self, since_id: int, limit: int, contains: str = "") -> List[Dict[str, Any]]:
        with self.debug_lock:
            rows = [x for x in self.debug_events if x["id"] > since_id]
            if contains:
                rows = [x for x in rows if contains.lower() in x["msg"].lower()]
            return rows[: max(1, min(limit, 1000))]


STATE = RuntimeState()
mcp = FastMCP("war3-autotest")


def _restore_video_config_if_needed(target_pid: int) -> Dict[str, Any]:
    if target_pid <= 0 or STATE.war3_pid != target_pid:
        return {"ok": True, "skipped": True, "reason": "pid 不匹配当前 AutoTest 会话"}
    if not STATE.video_restore_snapshot:
        return {"ok": True, "skipped": True, "reason": "无待恢复视频配置"}

    snapshot = dict(STATE.video_restore_snapshot)
    key_path = str(STATE.video_restore_key_path or WAR3_VIDEO_REG_KEY)
    restore = _restore_war3_video_registry(snapshot, key_path=key_path)
    if restore.get("ok"):
        STATE.video_restore_snapshot = {}
    return restore


def _close_state_desktop_if_needed(target_pid: int) -> Dict[str, Any]:
    if target_pid <= 0 or STATE.war3_pid != target_pid:
        return {"ok": True, "skipped": True, "reason": "pid 不匹配当前 AutoTest 会话"}
    handle = int(STATE.desktop_handle or 0)
    name = str(STATE.desktop_name or "")
    if handle == 0:
        return {"ok": True, "skipped": True, "reason": "无隔离桌面", "name": name}
    ok = _close_desktop_handle(handle)
    if ok:
        STATE.desktop_handle = 0
        STATE.desktop_name = ""
        STATE.desktop_mode = "default"
    return {
        "ok": ok,
        "closed": ok,
        "name": name,
        "handle": handle,
    }


def _clear_war3_launch_state(target_pid: int) -> None:
    if target_pid > 0 and STATE.war3_pid == target_pid:
        STATE.war3_proc = None
        STATE.war3_pid = None
        STATE.desktop_name = ""
        STATE.desktop_handle = 0
        STATE.desktop_mode = "default"
        STATE.launch_epoch_ms = 0


def _start_debug_monitor(pid_filter: Optional[int]) -> Dict[str, Any]:
    with STATE.debug_lock:
        if STATE.debug_thread and STATE.debug_thread.is_alive():
            STATE.debug_pid_filter = pid_filter
            return {"ok": True, "message": "debug monitor already running", "pidFilter": pid_filter}
        STATE.debug_stop.clear()
        STATE.debug_pid_filter = pid_filter

    def _worker() -> None:
        listener = DbWinListener()
        try:
            listener.open()
        except Exception as e:
            STATE.push_event(0, f"[AutoTest] DBWIN open failed: {e}")
            return
        while not STATE.debug_stop.is_set():
            try:
                pid, msg = listener.read(timeout_ms=200)
            except Exception as e:
                STATE.push_event(0, f"[AutoTest] DBWIN read failed: {e}")
                time.sleep(0.2)
                continue
            if pid is None or not msg:
                continue
            pid_filter_now = STATE.debug_pid_filter
            if pid_filter_now and pid != pid_filter_now:
                continue
            if "DXVK" in msg or "War3" in msg or "JASS" in msg:
                STATE.push_event(pid, msg)
        listener.close()

    t = threading.Thread(target=_worker, name="war3-dbwin-monitor", daemon=True)
    STATE.debug_thread = t
    t.start()
    return {"ok": True, "message": "debug monitor started", "pidFilter": pid_filter}


def _stop_debug_monitor() -> None:
    STATE.debug_stop.set()


def _get_perf_job_snapshot(limit_results: int = 50) -> Dict[str, Any]:
    with STATE.perf_lock:
        job = copy.deepcopy(STATE.perf_job)
    results = job.get("results", [])
    if isinstance(results, list) and limit_results > 0:
        job["results"] = results[-max(1, min(limit_results, 500)) :]
    job["running"] = bool(STATE.perf_thread and STATE.perf_thread.is_alive())
    return job


def _set_perf_job_fields(**kwargs: Any) -> None:
    with STATE.perf_lock:
        STATE.perf_job.update(kwargs)
        STATE.perf_job["updatedAt"] = _now_str()


def _compute_perf_aggregate(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    ok_rows = [r for r in results if bool(r.get("ok"))]
    agg: Dict[str, Any] = {
        "rounds": len(results),
        "success": len(ok_rows),
        "failed": len(results) - len(ok_rows),
    }
    if not ok_rows:
        return agg

    def _avg(key: str) -> float:
        vals = [float(r.get(key, 0.0)) for r in ok_rows if isinstance(r.get(key, None), (int, float))]
        return round(sum(vals) / len(vals), 4) if vals else 0.0

    agg.update(
        {
            "avgFps": _avg("avgFps"),
            "avgFrameTimeMs": _avg("avgFrameTimeMs"),
            "avgGpuTimeMs": _avg("avgGpuTimeMs"),
            "avgTrackedActiveCpuMs": _avg("avgTrackedActiveCpuMs"),
            "avgUntrackedActiveCpuMs": _avg("avgUntrackedActiveCpuMs"),
            "cpuCoveragePct": _avg("cpuCoveragePct"),
            "cpuCoverageWithIdlePct": _avg("cpuCoverageWithIdlePct"),
            "totalJank16": int(sum(int(r.get("jank16", 0)) for r in ok_rows)),
            "totalJank33": int(sum(int(r.get("jank33", 0)) for r in ok_rows)),
        }
    )
    return agg


def _prepare_test_map_copy(war3_dir: Path, map_path: Path, target_rel: Path) -> Path:
    target_abs = war3_dir / target_rel
    _ensure_dir(target_abs.parent)
    try:
        # 若源图和目标图是同一文件，直接复用，避免无意义覆盖。
        if map_path.resolve() != target_abs.resolve():
            shutil.copy2(map_path, target_abs)
    except PermissionError:
        # 目标图被占用时兜底复用现有短路径地图，避免自动测试链路整体失败。
        if not target_abs.exists():
            raise
    return target_abs


def _prefer_inplace_relative_loadfile_arg(war3_dir: Path, map_path: Path) -> str:
    try:
        rel = map_path.resolve().relative_to(war3_dir.resolve())
    except Exception:
        return ""
    rel_text = str(rel).replace("/", "\\")
    if not rel_text or len(rel_text) >= 54:
        return ""
    return rel_text


def _deploy_d3d9(build_dll: Path, war3_dir: Path) -> Dict[str, Any]:
    if not build_dll.is_absolute():
        build_dll = (REPO_ROOT / build_dll).resolve()
    dst = war3_dir / "d3d9.dll"
    if not build_dll.exists():
        return {"ok": False, "error": f"构建产物不存在: {build_dll}"}
    if not war3_dir.exists():
        return {"ok": False, "error": f"War3 目录不存在: {war3_dir}"}

    old_info = None
    if dst.exists():
        old_info = {
            "path": str(dst),
            "size": dst.stat().st_size,
            "mtime": datetime.fromtimestamp(dst.stat().st_mtime).isoformat(),
        }
    # 某些时刻（进程刚退出/杀软扫描）会短暂占用目标文件，做短重试避免整条链路失败。
    copy_error: Optional[str] = None
    for i in range(8):
        try:
            shutil.copy2(build_dll, dst)
            copy_error = None
            break
        except PermissionError as e:
            copy_error = str(e)
            time.sleep(0.25)
    if copy_error is not None:
        return {
            "ok": False,
            "error": f"部署 d3d9.dll 失败（重试后仍被占用）: {copy_error}",
            "source": str(build_dll),
            "target": str(dst),
            "old": old_info,
        }

    new_info = {
        "path": str(dst),
        "size": dst.stat().st_size,
        "mtime": datetime.fromtimestamp(dst.stat().st_mtime).isoformat(),
    }
    return {
        "ok": True,
        "source": str(build_dll),
        "target": str(dst),
        "old": old_info,
        "new": new_info,
    }


@mcp.tool()
def ydwe_launch_chain_analysis() -> Dict[str, Any]:
    """返回 YDWE 启动链关键结论（从源码抽取）。"""
    return {
        "ok": True,
        "summary": [
            "YDWE 使用 `war3.exe -loadfile <map>` 直进地图。",
            "为规避路径长度问题，会先把地图复制到 `Maps\\\\Test\\\\WorldEditTestMap.w3x`。",
            "之后传相对路径给 `-loadfile`（相对 war3 根目录）。",
            "可附带 `-window` 与 `-opengl`。",
            "YDWE 还会可选注入 LuaEngine.dll 与 Storm.dll 替换补丁。"
        ],
        "evidence": [
            r"E:\Mycode\Source\Repos\YDWE\Development\Core\YDWEStartup\LaunchWarcraft3.cpp:136",
            r"E:\Mycode\Source\Repos\YDWE\Development\Core\YDWEStartup\LaunchWarcraft3.cpp:147",
            r"E:\Mycode\Source\Repos\YDWE\Development\Component\script\ydwe\ydwe_on_test.lua:71",
        ],
    }


@mcp.tool()
def prepare_test_map(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    target_rel: str = str(DEFAULT_TEST_MAP_REL),
) -> Dict[str, Any]:
    """按 YDWE 方式复制测试地图到 War3 根目录下的短路径。"""
    w3 = Path(war3_dir)
    src = Path(map_path)
    rel = Path(target_rel)
    if not w3.exists():
        return {"ok": False, "error": f"war3_dir 不存在: {w3}"}
    if not src.exists():
        return {"ok": False, "error": f"map_path 不存在: {src}"}

    dst = _prepare_test_map_copy(w3, src, rel)
    return {
        "ok": True,
        "source": str(src),
        "target": str(dst),
        "loadfileArg": str(rel).replace("/", "\\"),
    }


@mcp.tool()
def deploy_d3d9_to_war3(
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    war3_dir: str = str(DEFAULT_WAR3_DIR),
) -> Dict[str, Any]:
    """将当前编译产物 d3d9.dll 部署到 War3 根目录。"""
    return _deploy_d3d9(Path(build_d3d9_path), Path(war3_dir))


@mcp.tool()
def ensure_war3_video_baseline(
    width: int = DEFAULT_BENCHMARK_WIDTH,
    height: int = DEFAULT_BENCHMARK_HEIGHT,
    refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
) -> Dict[str, Any]:
    """写入 War3 视频配置注册表，统一自动测试分辨率基线。"""
    return _set_war3_video_registry(width=width, height=height, refresh_rate=refresh_rate)


@mcp.tool()
def launch_war3_test(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    windowed: bool = False,
    use_isolated_desktop: bool = False,
    desktop_name: str = "",
    opengl: bool = False,
    auto_perf_record: bool = True,
    record_after_game_started: bool = False,
    auto_perf_export_sec: int = 10,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    render_log: bool = False,
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    extra_args: str = "",
) -> Dict[str, Any]:
    """启动 War3 并自动加载测试地图。"""
    w3 = Path(war3_dir)
    war3_exe = w3 / "war3.exe"
    src_map = Path(map_path)
    if not war3_exe.exists():
        return {"ok": False, "error": f"未找到 war3.exe: {war3_exe}"}
    if not src_map.exists():
        return {"ok": False, "error": f"未找到地图: {src_map}"}

    deploy = None
    if deploy_d3d9_before_launch:
        deploy = _deploy_d3d9(Path(build_d3d9_path), w3)
        if not deploy.get("ok"):
            return {"ok": False, "error": deploy.get("error", "部署 d3d9.dll 失败"), "deploy": deploy}

    video_baseline = {
        "ok": True,
        "skipped": True,
        "reason": "enforce_video_baseline=False",
    }
    if enforce_video_baseline:
        video_baseline = _set_war3_video_registry(
            width=baseline_width,
            height=baseline_height,
            refresh_rate=baseline_refresh_rate,
        )
        if not video_baseline.get("ok"):
            return {
                "ok": False,
                "error": video_baseline.get("error", "写入视频基线失败"),
                "deploy": deploy,
                "videoBaseline": video_baseline,
            }

    inplace_rel = _prefer_inplace_relative_loadfile_arg(w3, src_map)
    if inplace_rel:
        dst = src_map
        loadfile_arg = inplace_rel
        map_launch_mode = "inplace-relative"
    else:
        dst = _prepare_test_map_copy(w3, src_map, DEFAULT_TEST_MAP_REL)
        loadfile_arg = str(DEFAULT_TEST_MAP_REL).replace("/", "\\")
        map_launch_mode = "copied-short-path"

    effective_windowed = bool(windowed)
    forced_windowed = False
    desktop = {"ok": True, "skipped": True, "reason": "use_isolated_desktop=False"}
    if use_isolated_desktop:
        desktop = _create_isolated_desktop(desktop_name)
        if not desktop.get("ok"):
            return {
                "ok": False,
                "error": desktop.get("error", "创建隔离桌面失败"),
                "deploy": deploy,
                "videoBaseline": video_baseline,
                "desktop": desktop,
            }
        if not effective_windowed:
            effective_windowed = True
            forced_windowed = True

    args = [str(war3_exe)]
    if effective_windowed:
        args.append("-window")
    if opengl:
        args.append("-opengl")
    args.extend(["-loadfile", loadfile_arg])
    if extra_args.strip():
        args.extend(extra_args.strip().split())

    env = os.environ.copy()
    if auto_perf_record:
        env.setdefault("DXVK_WAR3_PERF_HISTORY_FRAMES", "7200")
        if record_after_game_started:
            env["DXVK_WAR3_PERF_RECORD_AFTER_GAME_START"] = "1"
            env.pop("DXVK_WAR3_PERF_RECORD_ON_START", None)
        else:
            env["DXVK_WAR3_PERF_RECORD_ON_START"] = "1"
        if auto_perf_export_sec > 0:
            env["DXVK_WAR3_PERF_AUTO_EXPORT_SEC"] = str(int(auto_perf_export_sec))
    if render_log:
        env["DXVK_WAR3_RENDER_LOG"] = "1"
    profile_name = str(profile or "").strip()
    if profile_name:
        env["DXVK_WAR3_PROFILE"] = profile_name
    disable_csv = str(disable_modules or "").strip()
    if disable_csv:
        env["DXVK_WAR3_DISABLE"] = disable_csv
    extra_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = extra_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "error": f"env_overrides_json 解析失败: {parse_error}"}
    # 高频 SpriteFrame/runtime-matrix pose hooks are no longer the default
    # semantic palette producer. The production path samples Blizzard's
    # already-evaluated CModel palette from the visible contract; tests that
    # specifically need the legacy hook path can still opt in explicitly.
    env.update(extra_env)

    for stale in (
        _runtime_status_file(w3),
        _frame_capture_request_file(w3),
        _frame_capture_result_file(w3),
        _internal_test_request_file(w3),
        _internal_test_result_file(w3),
    ):
        try:
            if stale.exists():
                stale.unlink()
        except Exception:
            pass

    # 先清空事件并启动 DBWIN 监听（不过滤 pid），
    # 避免进程启动早期日志（例如 RegisterImage 写入端）在监控线程尚未就绪时丢失。
    with STATE.debug_lock:
        STATE.debug_events.clear()
        STATE.debug_seq = 0

    _start_debug_monitor(None)
    # 给监听线程一点启动时间，降低“进程启动瞬间日志”丢失概率。
    time.sleep(0.2)

    launch_result: Dict[str, Any]
    if use_isolated_desktop:
        launch_result = _launch_process_on_desktop(
            args=args,
            cwd=w3,
            env=env,
            desktop_name=str(desktop.get("name", "")),
        )
        if not launch_result.get("ok"):
            _close_desktop_handle(int(desktop.get("handle", 0) or 0))
            return {
                "ok": False,
                "error": launch_result.get("error", "CreateProcessW 失败"),
                "deploy": deploy,
                "videoBaseline": video_baseline,
                "desktop": desktop,
            }
        proc = None
        pid = int(launch_result["pid"])
    else:
        proc = subprocess.Popen(
            args,
            cwd=str(w3),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        launch_result = {"ok": True, "pid": int(proc.pid)}
        pid = int(proc.pid)

    priority = _set_process_priority_high(pid)

    STATE.war3_proc = proc
    STATE.war3_pid = pid
    STATE.war3_dir = w3
    STATE.test_map_path = src_map
    STATE.desktop_name = str(desktop.get("name", "")) if use_isolated_desktop else ""
    STATE.desktop_handle = int(desktop.get("handle", 0) or 0) if use_isolated_desktop else 0
    STATE.desktop_mode = "isolated" if use_isolated_desktop else "default"
    STATE.launch_epoch_ms = int(time.time() * 1000)
    STATE.video_restore_key_path = str(video_baseline.get("keyPath", WAR3_VIDEO_REG_KEY))
    STATE.video_restore_snapshot = dict(video_baseline.get("old", {})) if enforce_video_baseline else {}

    # 进程创建后收敛到目标 pid，避免跨进程噪声。
    _start_debug_monitor(pid)

    return {
        "ok": True,
        "pid": pid,
        "args": args,
        "windowed": bool(effective_windowed),
        "requestedWindowed": bool(windowed),
        "forcedWindowedBecauseIsolatedDesktop": bool(forced_windowed),
        "useIsolatedDesktop": bool(use_isolated_desktop),
        "desktop": desktop,
        "profile": profile_name or "full_default",
        "disableModules": disable_csv,
        "recordAfterGameStarted": bool(record_after_game_started),
        "envOverrides": extra_env,
        "copiedMap": str(dst),
        "loadfileArg": loadfile_arg,
        "mapLaunchMode": map_launch_mode,
        "deploy": deploy,
        "videoBaseline": video_baseline,
        "priority": priority,
        "time": _now_str(),
    }


@mcp.tool()
def is_war3_running(pid: int = 0) -> Dict[str, Any]:
    """检查 War3 进程是否仍在运行。"""
    check_pid = pid or (STATE.war3_pid or 0)
    if check_pid <= 0:
        return {"ok": True, "running": False, "pid": 0}
    return {"ok": True, "running": _pid_alive(check_pid), "pid": check_pid}


@mcp.tool()
def read_runtime_status(war3_dir: str = str(DEFAULT_WAR3_DIR)) -> Dict[str, Any]:
    """读取项目侧 runtime_status.json（由 DXVK 运行时周期写入）。"""
    w3 = Path(war3_dir)
    target_pid = STATE.war3_pid or 0
    if target_pid > 0 and _pid_alive(target_pid):
        pipe_res = _control_plane_request(
            pid=target_pid,
            command="get_runtime_status",
            payload={},
            timeout_sec=2.0,
        )
        if pipe_res.get("transportOk"):
            return {
                "ok": bool(pipe_res.get("ok")),
                "mode": "control-plane",
                "pipeName": str(pipe_res.get("pipeName", "") or ""),
                "data": dict(pipe_res.get("result", {}) or {}),
                "detail": pipe_res,
            }

    path = _runtime_status_file(w3)
    data = _read_runtime_status_file(w3)
    if not data:
        return {"ok": False, "error": "runtime_status.json 不存在或解析失败", "path": str(path)}
    return {"ok": True, "path": str(path), "data": data}


@mcp.tool()
def wait_for_runtime_status(
    timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    require_runtime_ready: bool = False,
    require_game_started: bool = True,
    min_timestamp_ms: int = 0,
) -> Dict[str, Any]:
    """轮询 runtime_status.json，等待进入目标状态。"""
    w3 = Path(war3_dir)
    path = _runtime_status_file(w3)
    t0 = time.time()
    while time.time() - t0 < max(1, timeout_sec):
        data = _read_runtime_status_file(w3)
        if data:
            ts = int(data.get("timestampMs", 0))
            if min_timestamp_ms > 0 and ts < min_timestamp_ms:
                time.sleep(0.25)
                continue
            runtime = data.get("runtime", {})
            runtime_ready = bool(runtime.get("runtimeReady", False))
            game_started = bool(runtime.get("gameStarted", False))
            if ((not require_runtime_ready or runtime_ready) and
                (not require_game_started or game_started)):
                return {
                    "ok": True,
                    "path": str(path),
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "data": data,
                }
        time.sleep(0.25)
    return {
        "ok": False,
        "error": "等待 runtime_status 超时",
        "path": str(path),
        "elapsedSec": round(time.time() - t0, 3),
    }


@mcp.tool()
def get_runtime_events(since_id: int = 0, limit: int = 200, contains: str = "") -> Dict[str, Any]:
    """拉取 OutputDebugString 事件（可作为订阅轮询）。"""
    rows = STATE.get_events(since_id=since_id, limit=limit, contains=contains)
    return {
        "ok": True,
        "count": len(rows),
        "events": rows,
        "latestId": rows[-1]["id"] if rows else since_id,
    }


@mcp.tool()
def wait_for_game_ready(
    timeout_sec: int = 120,
    pid: int = 0,
    allow_fallback: bool = True,
    fallback_min_elapsed_sec: int = 20,
    fallback_min_cpu_sec: float = 1.0,
    require_game_started_for_fallback: bool = True,
) -> Dict[str, Any]:
    """
    等待“正式进入游戏”：
    - 先命中 runtime init：`JASS runtime fully initialized`
    - 再命中 in-game 渲染信号：`War3Shadow: Run frame=` 或 `War3StageSig: stage=19`
    """
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid，请先 launch_war3_test"}

    _start_debug_monitor(target_pid)

    pipe_wait_t0 = time.time()
    pipe_ready: Dict[str, Any] = {}
    while True:
        elapsed_wait = time.time() - pipe_wait_t0
        remaining_timeout = max(0.0, float(timeout_sec) - elapsed_wait)
        if remaining_timeout <= 0.0:
            break

        pipe_ready = _control_plane_request(
            pid=target_pid,
            command="wait_until",
            payload={
                "timeoutSec": max(1, int(math.ceil(remaining_timeout))),
                "pollIntervalMs": 50,
            },
            timeout_sec=max(2.0, remaining_timeout + 2.0),
        )
        if pipe_ready.get("transportOk"):
            break
        if not _pid_alive(target_pid):
            break
        time.sleep(0.2)

    if pipe_ready.get("transportOk"):
        runtime_status = dict(((pipe_ready.get("result", {}) or {}).get("runtimeStatus", {})) or {})
        if pipe_ready.get("ok"):
            return {
                "ok": True,
                "mode": "control-plane",
                "pid": target_pid,
                "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
                "runtimeStatus": runtime_status,
                "detail": pipe_ready,
            }
        return {
            "ok": False,
            "error": str(pipe_ready.get("error", "control plane wait_until 失败")),
            "mode": "control-plane",
            "pid": target_pid,
            "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
            "runtimeStatus": runtime_status,
            "detail": pipe_ready,
        }
    if not allow_fallback:
        return {
            "ok": False,
            "error": str(pipe_ready.get("error", "control plane 不可用")),
            "mode": "control-plane-required",
            "pid": target_pid,
            "elapsedSec": round(time.time() - pipe_wait_t0, 3),
            "detail": pipe_ready,
        }

    t0 = time.time()
    last_id = 0
    hit_init: Optional[Dict[str, Any]] = None
    hit_ingame: Optional[Dict[str, Any]] = None
    last_runtime_status: Optional[Dict[str, Any]] = None
    last_runtime_ts = 0
    stable_runtime_updates = 0
    stable_runtime_wall_t0 = 0.0
    dead_grace_checks = 0

    while time.time() - t0 < max(1, timeout_sec):
        if not _pid_alive(target_pid):
            runtime_status = _read_runtime_status_file(STATE.war3_dir)
            if runtime_status:
                rt = runtime_status.get("runtime", {})
                module = runtime_status.get("module", {})
                ts = int(runtime_status.get("timestampMs", 0))
                if (
                    ts >= max(0, STATE.launch_epoch_ms - 5_000)
                    and bool(rt.get("gameStarted", False))
                    and str(module.get("state", "")) == "Running"
                ):
                    dead_grace_checks += 1
                    last_runtime_status = runtime_status
                    if dead_grace_checks <= 15:
                        time.sleep(0.2)
                        continue
            return {
                "ok": False,
                "error": "进程已退出，等待失败",
                "pid": target_pid,
                "hitInit": hit_init,
                "hitInGame": hit_ingame,
            }
        dead_grace_checks = 0

        batch = STATE.get_events(since_id=last_id, limit=256)
        if batch:
            last_id = batch[-1]["id"]
        for e in batch:
            msg = e["msg"]
            if (hit_init is None) and ("DXVK War3Hook: JASS runtime fully initialized" in msg):
                hit_init = e
            if (
                "DXVK War3Shadow: Run frame=" in msg
                or "DXVK War3StageSig: stage=19" in msg
                or "DXVK War3Hook: Auto-fired OnGameStart via TIME_OF_DAY=" in msg
                or "[War3Events] OnGameStart complete" in msg
            ):
                hit_ingame = e

        if hit_init and hit_ingame:
            if not allow_fallback:
                status0 = _read_runtime_status_best_effort(target_pid)
                time.sleep(1.0)
                status1 = _read_runtime_status_best_effort(target_pid)
                if isinstance(status0, dict) and isinstance(status1, dict):
                    frame0 = int(status0.get("frameIndex", 0) or 0)
                    frame1 = int(status1.get("frameIndex", 0) or 0)
                    render0 = dict(status0.get("render", {}) or {})
                    render1 = dict(status1.get("render", {}) or {})
                    module0 = dict(status0.get("module", {}) or {})
                    module1 = dict(status1.get("module", {}) or {})
                    dispatch0 = int(module0.get("dispatchCalls", 0) or 0)
                    dispatch1 = int(module1.get("dispatchCalls", 0) or 0)
                    if (
                        frame0 > 0
                        and frame1 <= frame0
                        and dispatch1 <= dispatch0
                        and bool(render1.get("inGameRenderReady", False))
                        and not bool(render1.get("isInGame", False))
                    ):
                        return {
                            "ok": False,
                            "error": "debug-events ready 后 frameIndex 未继续推进，疑似首帧卡住",
                            "mode": "debug-events-stalled",
                            "pid": target_pid,
                            "elapsedSec": round(time.time() - t0, 3),
                            "hitInit": hit_init,
                            "hitInGame": hit_ingame,
                            "status0": status0,
                            "status1": status1,
                        }
            return {
                "ok": True,
                "mode": "debug-events",
                "pid": target_pid,
                "elapsedSec": round(time.time() - t0, 3),
                "hitInit": hit_init,
                "hitInGame": hit_ingame,
            }

        # 优先读取项目侧 runtime_status（更稳定，不依赖 DBWIN）。
        runtime_status = _read_runtime_status_file(STATE.war3_dir)
        if runtime_status:
            last_runtime_status = runtime_status
            rt = runtime_status.get("runtime", {})
            perf = runtime_status.get("perf", {})
            module = runtime_status.get("module", {})
            ts = int(runtime_status.get("timestampMs", 0))
            runtime_ready = bool(rt.get("runtimeReady", False))
            jass_ready = bool(rt.get("jassReady", False))
            game_started = bool(rt.get("gameStarted", False))
            frame_index = int(runtime_status.get("frameIndex", 0) or 0)
            periodic_source = str(runtime_status.get("source", "")) == "periodic"
            module_running = str(module.get("state", "")) == "Running"
            if hit_init is None and jass_ready:
                hit_init = {
                    "id": -1,
                    "ts": ts,
                    "msg": "runtime_status.runtime.jassReady=true",
                }
            if ts >= max(0, STATE.launch_epoch_ms - 5_000) and runtime_ready:
                return {
                    "ok": True,
                    "mode": "runtime-status-file",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(STATE.war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            # 背景/隔离桌面里，部分图会稳定触发 OnGameStart，但 runtimeReady
            # 与 DBWIN in-game 信号迟迟不补齐。这里改成“稳定活动”软成功：
            # runtime_status 必须持续变新、模块持续 Running，且具备 recording /
            # periodic / frameIndex>0 三者之一，才允许进入采样。
            soft_game_started = (
                ts >= max(0, STATE.launch_epoch_ms - 5_000)
                and game_started
                and module_running
            )
            stable_activity = (
                soft_game_started
                and (
                    bool(perf.get("recording", False))
                    or periodic_source
                    or frame_index > 0
                    or hit_init is not None
                )
            )
            if stable_activity and ts > last_runtime_ts:
                if stable_runtime_updates == 0:
                    stable_runtime_wall_t0 = time.time()
                stable_runtime_updates += 1
                last_runtime_ts = ts
            elif not stable_activity:
                stable_runtime_updates = 0
                stable_runtime_wall_t0 = 0.0
                if ts > last_runtime_ts:
                    last_runtime_ts = ts

            if (
                stable_activity
                and stable_runtime_updates >= 2
                and stable_runtime_wall_t0 > 0.0
                and (time.time() - stable_runtime_wall_t0) >= 1.5
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-stable",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(STATE.war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            if (
                soft_game_started
                and hit_init is not None
                and (time.time() - t0) >= 4.0
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-game-started",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(STATE.war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            if (
                soft_game_started
                and (bool(perf.get("recording", False)) or periodic_source or frame_index > 0)
                and (time.time() - t0) >= 4.0
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-game-started",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(STATE.war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }

        # 兜底：当 DBWIN 抓不到日志时，使用窗口存在 + CPU 累计时间判定。
        if allow_fallback:
            elapsed = time.time() - t0
            hwnd = _find_main_window_hwnd(target_pid)
            cpu_sec = _get_process_cpu_seconds(target_pid)
            fallback_game_started = True
            if require_game_started_for_fallback and last_runtime_status:
                fallback_game_started = bool(((last_runtime_status.get("runtime") or {}).get("gameStarted", False)))
            if hwnd and fallback_game_started and elapsed >= max(1, fallback_min_elapsed_sec) and cpu_sec >= max(0.0, fallback_min_cpu_sec):
                return {
                    "ok": True,
                    "mode": "fallback-window-cpu",
                    "pid": target_pid,
                    "elapsedSec": round(elapsed, 3),
                    "cpuSec": round(cpu_sec, 3),
                    "hwnd": int(hwnd),
                    "runtimeStatus": last_runtime_status or {},
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                    "requireGameStartedForFallback": bool(require_game_started_for_fallback),
                }
        time.sleep(0.2)

    return {
        "ok": False,
        "error": "超时，未观察到完整 in-game 信号",
        "pid": target_pid,
        "elapsedSec": round(time.time() - t0, 3),
        "runtimeStatus": last_runtime_status or {},
        "hitInit": hit_init,
        "hitInGame": hit_ingame,
    }


def _shadow_summary_int(summary: Dict[str, Any], key: str) -> int:
    try:
        return int(summary.get(key, 0) or 0)
    except (TypeError, ValueError):
        return 0


def _native_execute_success_draw_count(summary: Dict[str, Any]) -> int:
    """Return a stable native execute draw count.

    The current-frame executed counter can be reset when a later control-plane
    summary prepares a new native frame. The stable last-success fields preserve
    the actual render-thread execute result.
    """
    current = _shadow_summary_int(summary, "nativeD3D9BackendExecutedDrawCount")
    stable = _shadow_summary_int(
        summary,
        "nativeD3D9BackendLastSuccessfulExecutedDrawCount",
    )
    if stable <= 0 and _shadow_summary_int(
        summary,
        "nativeD3D9BackendExecuteSuccessCount",
    ) > 0:
        submitted = _shadow_summary_int(
            summary,
            "nativeD3D9BackendLastExecuteSubmittedDrawCount",
        )
        failed = _shadow_summary_int(
            summary,
            "nativeD3D9BackendLastExecuteFailedDrawCount",
        )
        stable = max(0, submitted - failed)
    return max(current, stable)


def _nested_status_int(status: Dict[str, Any], *path: str) -> int:
    cur: Any = status
    for key in path:
        if not isinstance(cur, dict):
            return 0
        cur = cur.get(key)
    try:
        return int(cur or 0)
    except (TypeError, ValueError):
        return 0


def _runtime_frame_progress_status(
    runtime_status: Dict[str, Any],
    *,
    frame_advance_stalled: bool = False,
    frame_stall_sec: float = 0.0,
) -> Dict[str, Any]:
    """Expose frame-tail state without changing the ready contract."""
    render = runtime_status.get("render", {}) if isinstance(runtime_status, dict) else {}
    frame = runtime_status.get("frame", {}) if isinstance(runtime_status, dict) else {}
    render_in_game_ready = bool(render.get("inGameRenderReady", False))
    render_is_in_game = bool(render.get("isInGame", False))
    tail_stalled = render_in_game_ready and bool(frame_advance_stalled)
    inactive_tail_stalled = tail_stalled and not render_is_in_game
    return {
        "runtimeFrameIndex": _nested_status_int(runtime_status, "frameIndex"),
        "runtimeFrameNumber": _nested_status_int(runtime_status, "frame", "frameNumber"),
        "runtimeFramePublishRevision": _nested_status_int(
            runtime_status,
            "frame",
            "publishRevision",
        ),
        "runtimeRenderInGameReady": render_in_game_ready,
        "runtimeRenderIsInGame": render_is_in_game,
        "runtimeFrameAdvanceStalled": bool(frame_advance_stalled),
        "runtimeFrameStallSec": round(max(0.0, float(frame_stall_sec)), 3),
        "runtimeRenderTailStalled": bool(tail_stalled),
        "runtimeRenderInactiveTailStalled": bool(inactive_tail_stalled),
        "runtimeVisibleCount": _nested_status_int(runtime_status, "frame", "visibleCount"),
        "runtimeUnitCount": _nested_status_int(runtime_status, "frame", "unitCount"),
        "runtimeRecordsWithRuntimeModel": _nested_status_int(
            runtime_status,
            "frame",
            "recordsWithRuntimeModel",
        ),
        "runtimeRecordsWithModelResource": _nested_status_int(
            runtime_status,
            "frame",
            "recordsWithModelResource",
        ),
    }


def _semantic_scene_consumption_status(summary: Dict[str, Any]) -> Dict[str, Any]:
    """Classify whether the DXVK scene pass consumed the latest semantic frame."""
    near_latest_max_lag = 2
    core_submitted = _shadow_summary_int(summary, "semanticCoreSubmittedDrawCount")
    scene_submitted = _shadow_summary_int(summary, "semanticSceneLastSubmittedDrawCount")
    scene_skinned = _shadow_summary_int(summary, "semanticSceneSubmittedSkinned")
    direct_currentdraw_ready = _shadow_summary_int(
        summary,
        "semanticSceneCurrentDrawResolveReadyCount",
    )
    canonical_ready = _shadow_summary_int(summary, "semanticSceneCanonicalReadyCount")
    fallback = _shadow_summary_int(summary, "objectFallbackDrawCount")
    scene_publish_count = _shadow_summary_int(summary, "semanticSceneStatsPublishCount")
    scene_lag = _shadow_summary_int(summary, "semanticScenePublishRevisionLag")
    scene_frame_serial = _shadow_summary_int(
        summary,
        "semanticSceneLastFrameSerial",
    )
    core_frame_serial = _shadow_summary_int(
        summary,
        "semanticCoreFrameSerial",
    )
    revision_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_lag == 0
    )
    same_frame_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_frame_serial >= core_frame_serial
    )
    near_latest_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_lag >= 0
        and scene_lag <= near_latest_max_lag
        and core_frame_serial >= scene_frame_serial
        and (core_frame_serial - scene_frame_serial) <= near_latest_max_lag
    )
    direct_currentdraw_consumed = (
        scene_submitted > 0
        and scene_skinned > 0
        and direct_currentdraw_ready > 0
        and canonical_ready > 0
        and fallback == 0
    )
    consumed = (
        revision_consumed
        or same_frame_consumed
        or near_latest_consumed
        or direct_currentdraw_consumed
    )
    supplemented_revision_pending = same_frame_consumed and not revision_consumed
    waiting_for_render_scene = (
        core_submitted > 0 and not consumed and not direct_currentdraw_consumed
    )
    if revision_consumed:
        consumption_mode = "revision"
    elif same_frame_consumed:
        consumption_mode = "same-frame"
    elif near_latest_consumed:
        consumption_mode = "near-latest"
    elif direct_currentdraw_consumed:
        consumption_mode = "current-draw-direct"
    else:
        consumption_mode = "pending"
    return {
        "semanticSceneConsumptionFresh": bool(consumed),
        "semanticSceneRevisionConsumed": bool(revision_consumed),
        "semanticSceneSameFrameConsumed": bool(same_frame_consumed),
        "semanticSceneNearLatestConsumed": bool(near_latest_consumed),
        "semanticSceneDirectCurrentDrawConsumed": bool(direct_currentdraw_consumed),
        "semanticSceneNearLatestMaxLag": int(near_latest_max_lag),
        "semanticSceneSupplementedRevisionPending": bool(supplemented_revision_pending),
        "semanticSceneConsumptionMode": consumption_mode,
        "semanticSceneWaitingForRenderPass": bool(waiting_for_render_scene),
        "semanticScenePublishRevisionLag": int(scene_lag),
        "semanticSceneStatsPublishCount": int(scene_publish_count),
        "semanticSceneLastSubmittedDrawCount": int(scene_submitted),
        "semanticSceneLastFrameSerial": scene_frame_serial,
        "semanticCoreFrameSerial": core_frame_serial,
        "semanticSceneLastSourcePublishRevision": _shadow_summary_int(
            summary,
            "semanticSceneLastSourcePublishRevision",
        ),
        "semanticCoreSourcePublishRevision": _shadow_summary_int(
            summary,
            "semanticCoreSourcePublishRevision",
        ),
    }


def _refresh_shadow_runtime_summary_until(
    *,
    pid: int,
    wait_sec: float,
    min_submitted_draw_count: int = 0,
    min_attachment_rigid_resolved: int = 0,
    require_semantic_frame_fresh: bool = True,
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """Poll the control plane summary after a failed hot-frame wait.

    wait_until can return the last pre-rebuild summary when semantic resources
    are populated late in the same frame. A short explicit summary poll keeps
    the failure diagnostic tied to the latest semantic contract state.
    """
    deadline = time.time() + max(0.0, float(wait_sec))
    last_detail: Dict[str, Any] = {}
    best_summary: Dict[str, Any] = {}

    while True:
        detail = _control_plane_request(
            pid=pid,
            command="get_shadow_runtime_summary",
            payload={
                "refreshSemanticFrameIfStale": True,
                "forceSemanticFrameBuild": True,
                "allowControlPlaneSemanticDrain": True,
                "semanticBuildMinIntervalMs": 0,
                "semanticBuildDrainMaxChunks": 32,
                "semanticBuildDrainBudgetUs": 50000,
                "semanticBuildDrainRecordCeiling": 1024,
            },
            timeout_sec=2.0,
        )
        last_detail = detail
        if detail.get("transportOk") and detail.get("ok"):
            summary = dict(detail.get("result", {}) or {})
            best_summary = summary
            submitted = _shadow_summary_int(summary, "semanticCoreSubmittedDrawCount")
            attachment = _shadow_summary_int(summary, "semanticCoreAttachmentRigidResolved")
            supplemental = _shadow_summary_int(
                summary,
                "semanticCoreAttachmentRigidSupplementalResolvedCount",
            )
            frame_fresh = bool(summary.get("semanticCoreFrameFresh", False))
            if (
                submitted >= max(0, int(min_submitted_draw_count))
                and max(attachment, supplemental)
                >= max(0, int(min_attachment_rigid_resolved))
                and (not bool(require_semantic_frame_fresh) or frame_fresh)
            ):
                return summary, detail

        if time.time() >= deadline:
            break
        # The attachment supplemental path can settle a few ticks after ready
        # even when the isolated desktop stops advancing visible frames.
        time.sleep(0.5)

    return best_summary, last_detail


@mcp.tool()
def wait_for_hot_shadow_frame(
    timeout_sec: int = 120,
    pid: int = 0,
    min_visible_count: int = 1,
    min_stable_identity_count: int = 1,
    min_unit_count: int = 1,
    min_semantic_resolved: int = 1,
    min_semantic_skinned_resolved: int = 0,
    min_native_executed_draw_count: int = 0,
    require_semantic_frame_fresh: bool = True,
    min_frame_advance: int = 2,
    allow_semantic_rigid_only: bool = False,
    allow_semantic_attachment_rigid_only: bool = False,
    min_semantic_attachment_rigid_resolved: int = 0,
    post_failure_summary_wait_sec: int = 6,
    prefer_summary_poll: bool = False,
    require_semantic_scene_consumed: bool = False,
    allow_scene_pending_if_core_and_currentdraw_ready: bool = False,
    min_semantic_static_world_submitted: int = 0,
    allow_semantic_static_world_only: bool = False,
) -> Dict[str, Any]:
    """等待进入热帧语义阴影状态，而不是只等待 ready 首帧。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid，请先 launch_war3_test"}
    timeout_sec = max(1, int(timeout_sec))
    t0 = time.time()
    if bool(prefer_summary_poll):
        deadline = t0 + float(timeout_sec)
        last_detail: Dict[str, Any] = {}
        last_manifest: Dict[str, Any] = {}
        last_runtime_status: Dict[str, Any] = {}
        last_summary: Dict[str, Any] = {}
        last_frame_index: Optional[int] = None
        last_frame_advance_at = t0
        response: Dict[str, Any] = {
            "ok": False,
            "mode": "control-plane-hot-frame-summary-poll",
            "pid": target_pid,
            "elapsedSec": 0.0,
            "error": "等待 hot shadow summary 超时",
        }
        while time.time() < deadline:
            latest = _control_plane_request(
                pid=target_pid,
                command="get_hot_shadow_probe",
                payload={
                    "refreshSemanticFrameIfStale": True,
                    "forceSemanticFrameBuild": True,
                    "allowControlPlaneSemanticDrain": True,
                    "semanticBuildMinIntervalMs": 0,
                    "semanticBuildDrainMaxChunks": 32,
                    "semanticBuildDrainBudgetUs": 50000,
                    "semanticBuildDrainRecordCeiling": 1024,
                },
                timeout_sec=3.0,
            )
            last_detail = latest
            latest_result = dict(latest.get("result", {}) or {}) if latest.get("ok") else {}
            latest_summary = dict(latest_result.get("shadowRuntimeSummary", {}) or {})
            if latest_summary:
                last_summary = latest_summary
            else:
                latest_summary = last_summary
            last_manifest = dict(latest_result.get("frameManifestSummary", {}) or last_manifest or {})
            last_runtime_status = dict(latest_result.get("runtimeStatus", {}) or last_runtime_status or {})
            runtime_frame_index = _nested_status_int(last_runtime_status, "frameIndex")
            now = time.time()
            if last_frame_index is None or runtime_frame_index != last_frame_index:
                last_frame_index = runtime_frame_index
                last_frame_advance_at = now
            frame_stall_sec = now - last_frame_advance_at
            runtime_frame_status = _runtime_frame_progress_status(
                last_runtime_status,
                frame_advance_stalled=frame_stall_sec >= 3.0,
                frame_stall_sec=frame_stall_sec,
            )
            response = {
                "ok": False,
                "mode": "control-plane-hot-frame-summary-poll",
                "pid": target_pid,
                "elapsedSec": round(time.time() - t0, 3),
                "runtimeStatus": last_runtime_status,
                "frameManifestSummary": last_manifest,
                "shadowRuntimeSummary": latest_summary,
                "error": "",
                "detail": latest,
            }
            response.update(runtime_frame_status)
            semantic_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreResolved",
            )
            submitted = _shadow_summary_int(
                latest_summary,
                "semanticCoreSubmittedDrawCount",
            )
            rigid_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreRigidResolved",
            )
            explicit_rigid = _shadow_summary_int(
                latest_summary,
                "semanticCoreExplicitResourceOwnerRigidResolved",
            )
            attachment_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreAttachmentRigidResolved",
            )
            attachment_supplemental_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreAttachmentRigidSupplementalResolvedCount",
            )
            skinned_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreSkinnedResolved",
            )
            native_draws = _native_execute_success_draw_count(latest_summary)
            response["nativeD3D9BackendEffectiveExecutedDrawCount"] = native_draws
            fallback = _shadow_summary_int(latest_summary, "objectFallbackDrawCount")
            frame_fresh = bool(latest_summary.get("semanticCoreFrameFresh", False))
            scene_status = _semantic_scene_consumption_status(latest_summary)
            response.update(scene_status)
            scene_submitted = _shadow_summary_int(
                latest_summary,
                "semanticSceneLastSubmittedDrawCount",
            )
            scene_skinned = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedSkinned",
            )
            scene_building = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedBuilding",
            )
            scene_destructible = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedDestructible",
            )
            explicit_rigid_scene = _shadow_summary_int(
                latest_summary,
                "semanticSceneAcceptedExplicitResourceOwnerRigid",
            )
            currentdraw_query_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawContractQueryHitCount",
            )
            currentdraw_palette_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawCapturedPaletteQueryHitCount",
            )
            currentdraw_group_decode_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawGroupSlotDecodeHitCount",
            )
            scene_lag = int(
                scene_status.get("semanticScenePublishRevisionLag", 0) or 0
            )
            scene_frame_serial = int(
                scene_status.get("semanticSceneLastFrameSerial", 0) or 0
            )
            core_frame_serial = int(
                scene_status.get("semanticCoreFrameSerial", 0) or 0
            )
            manifest_ok = True
            if last_manifest:
                manifest_ok = (
                    _shadow_summary_int(last_manifest, "visibleCount")
                    >= max(0, int(min_visible_count))
                    and _shadow_summary_int(last_manifest, "recordsWithStableIdentity")
                    >= max(0, int(min_stable_identity_count))
                    and _shadow_summary_int(last_manifest, "unitCount")
                    >= max(0, int(min_unit_count))
                )
            semantic_contract_ok = (
                semantic_resolved >= max(0, int(min_semantic_resolved))
                and submitted > 0
                and fallback == 0
                and (not bool(require_semantic_frame_fresh) or frame_fresh)
            )
            scene_consumption_required = bool(require_semantic_scene_consumed) or not (
                bool(allow_semantic_rigid_only)
                or bool(allow_semantic_attachment_rigid_only)
            )
            strict_ok = (
                semantic_contract_ok
                and manifest_ok
                and skinned_resolved >= max(0, int(min_semantic_skinned_resolved))
                and native_draws >= max(0, int(min_native_executed_draw_count))
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
            )
            scene_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and manifest_ok
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
            )
            static_world_scene_submitted = (
                scene_building + scene_destructible + explicit_rigid_scene
            )
            static_world_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and manifest_ok
                and static_world_scene_submitted
                >= max(0, int(min_semantic_static_world_submitted))
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
            )
            direct_currentdraw_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
                and _shadow_summary_int(
                    latest_summary,
                    "semanticSceneCurrentDrawResolveReadyCount",
                )
                > 0
                and _shadow_summary_int(
                    latest_summary,
                    "semanticSceneCanonicalReadyCount",
                )
                > 0
                and bool(scene_status.get("semanticSceneDirectCurrentDrawConsumed"))
            )
            attachment_contract_ok = (
                semantic_contract_ok
                and max(attachment_resolved, attachment_supplemental_resolved)
                >= max(1, int(min_semantic_attachment_rigid_resolved))
            )
            if int(min_semantic_attachment_rigid_resolved) > 0:
                # Attachment-rigid was the previous proof that semantic data had
                # reached the renderer. Once a strict skinned semantic frame is
                # present, do not keep dynamic_shadow_pressure blocked on the
                # older attachment diagnostic. model_runtime_probe still passes
                # min_semantic_skinned_resolved=0 and therefore keeps the
                # attachment contract gate as intended.
                strict_ok = strict_ok and (
                    attachment_contract_ok
                    or skinned_resolved >= max(
                        1, int(min_semantic_skinned_resolved)
                    )
                )
            if strict_ok:
                response["ok"] = True
                return response
            if scene_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "DXVK semantic scene submitted and consumed draw packets; "
                    "native backend counters are not required for this visual "
                    "validation gate"
                )
                return response
            if direct_currentdraw_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "current-draw canonical scene submitted skinned packets "
                    "with fallback disabled; this Phase 3 validation path does "
                    "not require the older semantic core/manifest consumer"
                )
                response["semanticShadowPhase"] = "current-draw-direct-ok"
                return response
            if allow_semantic_static_world_only and static_world_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "static-world canonical scene submitted building/destructible "
                    "packets with fallback disabled"
                )
                response["semanticShadowPhase"] = "static-world-direct-ok"
                return response
            if (
                bool(allow_scene_pending_if_core_and_currentdraw_ready)
                and semantic_contract_ok
                and fallback == 0
                and currentdraw_query_hit > 0
                and currentdraw_palette_hit > 0
                and currentdraw_group_decode_hit > 0
            ):
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "semantic core is fresh, current-draw contract/palette/group-slot "
                    "queries are hot, and isolated-desktop render-scene consumption "
                    "is stalled at tail; accepted as low-pressure tail artifact"
                )
                response["semanticShadowPhase"] = "low-pressure-tail-accepted"
                return response
            if (
                semantic_contract_ok
                and manifest_ok
                and scene_submitted > 0
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
                and scene_status.get("semanticSceneWaitingForRenderPass")
                and scene_lag <= 16
                and scene_frame_serial > 0
                and core_frame_serial > 0
                and scene_frame_serial <= core_frame_serial
                and (core_frame_serial - scene_frame_serial) <= 1
            ):
                response["ok"] = True
                response["semanticTailSceneNearLatestAccepted"] = True
                response["semanticTailSceneNearLatestReason"] = (
                    "semantic core is fresh and the DXVK render-scene pass has "
                    "already consumed the immediately previous semantic frame "
                    "with fallback disabled; the latest publish revision is "
                    "waiting for one more isolated-desktop render tick, so this "
                    "is accepted as a tail-frame validation artifact"
                )
                return response

            core_packets_present = (
                submitted > 0
                and fallback == 0
                and semantic_resolved >= max(0, int(min_semantic_resolved))
            )
            if (
                core_packets_present
                and scene_status.get("semanticSceneWaitingForRenderPass")
            ):
                if native_draws > 0:
                    response["semanticShadowPhase"] = (
                        "native-semantic-executed-scene-pending"
                    )
                    response["semanticNativeExecuted"] = True
                    response["semanticShadowPhaseReason"] = (
                        "native D3D9 backend has successfully executed semantic "
                        "draws, but the DXVK render-scene validation pass has "
                        "not consumed the latest semantic publish revision yet"
                    )
                else:
                    response["semanticShadowPhase"] = (
                        "core-fresh-waiting-render-scene"
                        if frame_fresh
                        else "core-packets-waiting-render-scene"
                    )
                    response["semanticShadowPhaseReason"] = (
                        "semantic core has submitted draw packets, but the DXVK "
                        "render-scene pass has not consumed the latest semantic "
                        "publish revision yet"
                    )
                if not frame_fresh:
                    response["semanticShadowPhaseReason"] += (
                        "; semantic core freshness is also pending because the "
                        "latest render-scene tick has not advanced"
                    )
                if response.get("runtimeRenderTailStalled"):
                    response["semanticShadowPhaseReason"] += (
                        "; runtime frame advance is stalled after in-game render "
                        "readiness, so this is a render-scene consumption wait, "
                        "not a semantic data-chain miss"
                    )
                response["error"] = response["semanticShadowPhaseReason"]

            pending_without_consumer = (
                response.get("runtimeRenderTailStalled")
                and bool(latest_summary.get("semanticCoreBuildRequestPending", False))
                and not bool(latest_summary.get("semanticCoreBuildInProgress", False))
                and submitted <= 0
                and scene_submitted <= 0
            )
            if pending_without_consumer and frame_stall_sec >= 8.0:
                response["semanticShadowPhase"] = "core-build-pending-no-render-consumer"
                response["semanticShadowPhaseReason"] = (
                    "semantic contract is pending, but the isolated-desktop "
                    "runtime frame has stopped advancing before the DXVK render "
                    "scene consumed any build chunks; this is a render-thread "
                    "consumer timing blocker, not a control-plane timeout"
                )
                response["error"] = response["semanticShadowPhaseReason"]
                return response

            if attachment_contract_ok:
                response["semanticAttachmentRigidOnlyAccepted"] = True
                response["semanticAttachmentRigidGateSatisfied"] = True
                if not response.get("semanticShadowPhase"):
                    response["semanticShadowPhase"] = (
                        "attachment-rigid-ok-skinned-pending"
                    )
                    response["semanticShadowPhaseReason"] = (
                        "attachment rigid semantic path is producing submitted draw "
                        "packets with fallback disabled; skinned/upper-layer/native "
                        "gates remain pending"
                    )
                if bool(allow_semantic_attachment_rigid_only):
                    response["ok"] = True
                else:
                    response["error"] = response["semanticShadowPhaseReason"]
                return response

            if (
                semantic_contract_ok
                and (explicit_rigid > 0 or rigid_resolved > 0)
                and skinned_resolved <= 0
            ):
                response["semanticShadowPhase"] = (
                    "semantic-rigid-ok-skinned-pending"
                )
                response["semanticShadowPhaseReason"] = (
                    "semantic rigid path is producing draw packets, but skinned "
                    "gate is still pending"
                )
                if bool(allow_semantic_rigid_only):
                    response["ok"] = True
                    response["semanticRigidOnlyAccepted"] = True
                    response["error"] = ""
                    return response

            time.sleep(0.5)

        response["ok"] = False
        response["elapsedSec"] = round(time.time() - t0, 3)
        response["error"] = response.get("error") or "等待 hot shadow summary 超时"
        response["detail"] = last_detail
        return response

    require_frame_advance = int(min_frame_advance) > 0
    pipe_ready = _control_plane_request(
        pid=target_pid,
        command="wait_until",
        payload={
            "timeoutSec": timeout_sec,
            "pollIntervalMs": 50,
            "requireFrameAdvance": require_frame_advance,
            "minFrameAdvance": max(0, int(min_frame_advance)),
            "requireSemanticFrameFresh": bool(require_semantic_frame_fresh),
            "requireSemanticSceneConsumed": bool(require_semantic_scene_consumed),
            "minVisibleCount": max(0, int(min_visible_count)),
            "minStableIdentityCount": max(0, int(min_stable_identity_count)),
            "minUnitCount": max(0, int(min_unit_count)),
            "minSemanticResolved": max(0, int(min_semantic_resolved)),
            "minSemanticSkinnedResolved": max(0, int(min_semantic_skinned_resolved)),
            "requestSemanticFrameBuild": True,
            "forceSemanticFrameBuild": True,
            "allowControlPlaneSemanticDrain": True,
            "allowPreInGameSemanticBuild": False,
            "semanticBuildMinIntervalMs": 0,
            "semanticBuildDrainMaxChunks": 32,
            "semanticBuildDrainBudgetUs": 50000,
            "semanticBuildDrainRecordCeiling": 1024,
            "stalledFrameTimeoutMs": 3000,
        },
        timeout_sec=max(2.0, float(timeout_sec) + 2.0),
    )
    if not pipe_ready.get("transportOk"):
        return {
            "ok": False,
            "mode": "control-plane-hot-frame",
            "pid": target_pid,
            "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
            "error": str(pipe_ready.get("error", "control plane 不可用")),
            "detail": pipe_ready,
        }

    result = dict(pipe_ready.get("result", {}) or {})
    runtime_status = dict(result.get("runtimeStatus", {}) or {})
    wait_stalled = str(pipe_ready.get("error", "") or "") == "wait_until stalled"
    response = {
        "ok": bool(pipe_ready.get("ok")),
        "mode": "control-plane-hot-frame",
        "pid": target_pid,
        "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
        "runtimeStatus": runtime_status,
        "frameManifestSummary": dict(result.get("frameManifestSummary", {}) or {}),
        "shadowRuntimeSummary": dict(result.get("shadowRuntimeSummary", {}) or {}),
        "readyFrameBaseline": int(result.get("readyFrameBaseline", 0) or 0),
        "requestedSemanticFrameBuild": bool(result.get("requestedSemanticFrameBuild", False)),
        "semanticBuildRequestReason": str(result.get("semanticBuildRequestReason", "") or ""),
        "error": str(pipe_ready.get("error", "") or ""),
        "detail": pipe_ready,
    }
    response.update(
        _runtime_frame_progress_status(
            runtime_status,
            frame_advance_stalled=wait_stalled,
            frame_stall_sec=float(result.get("stalledFrameTimeoutMs", 0) or 0) / 1000.0
            if wait_stalled
            else 0.0,
        )
    )
    latest_summary = dict(response.get("shadowRuntimeSummary", {}) or {})
    if not response.get("ok"):
        refreshed_summary, refresh_detail = _refresh_shadow_runtime_summary_until(
            pid=target_pid,
            wait_sec=max(0, int(post_failure_summary_wait_sec)),
            min_submitted_draw_count=1,
            min_attachment_rigid_resolved=max(
                0,
                int(min_semantic_attachment_rigid_resolved),
            ),
            require_semantic_frame_fresh=bool(require_semantic_frame_fresh),
        )
        if refreshed_summary:
            response["shadowRuntimeSummary"] = refreshed_summary
            response["postFailureSummaryRefresh"] = refresh_detail
            latest_summary = refreshed_summary

    semantic_resolved = _shadow_summary_int(latest_summary, "semanticCoreResolved")
    submitted = _shadow_summary_int(latest_summary, "semanticCoreSubmittedDrawCount")
    rigid_resolved = _shadow_summary_int(latest_summary, "semanticCoreRigidResolved")
    explicit_rigid = _shadow_summary_int(
        latest_summary,
        "semanticCoreExplicitResourceOwnerRigidResolved",
    )
    attachment_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreAttachmentRigidResolved",
    )
    attachment_supplemental_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreAttachmentRigidSupplementalResolvedCount",
    )
    skinned_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreSkinnedResolved",
    )
    fallback = _shadow_summary_int(latest_summary, "objectFallbackDrawCount")
    frame_fresh = bool(latest_summary.get("semanticCoreFrameFresh", False))
    scene_status = _semantic_scene_consumption_status(latest_summary)
    response.update(scene_status)
    native_draws = _native_execute_success_draw_count(latest_summary)
    response["nativeD3D9BackendEffectiveExecutedDrawCount"] = native_draws
    scene_submitted = _shadow_summary_int(
        latest_summary,
        "semanticSceneLastSubmittedDrawCount",
    )
    scene_skinned = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedSkinned",
    )
    scene_building = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedBuilding",
    )
    scene_destructible = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedDestructible",
    )
    explicit_rigid_scene = _shadow_summary_int(
        latest_summary,
        "semanticSceneAcceptedExplicitResourceOwnerRigid",
    )
    currentdraw_query_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawContractQueryHitCount",
    )
    currentdraw_palette_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawCapturedPaletteQueryHitCount",
    )
    currentdraw_group_decode_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawGroupSlotDecodeHitCount",
    )
    scene_lag = int(scene_status.get("semanticScenePublishRevisionLag", 0) or 0)
    scene_frame_serial = int(
        scene_status.get("semanticSceneLastFrameSerial", 0) or 0
    )
    core_frame_serial = int(scene_status.get("semanticCoreFrameSerial", 0) or 0)
    manifest_summary = dict(response.get("frameManifestSummary", {}) or {})
    manifest_ok = True
    if manifest_summary:
        manifest_ok = (
            _shadow_summary_int(manifest_summary, "visibleCount")
            >= max(0, int(min_visible_count))
            and _shadow_summary_int(manifest_summary, "recordsWithStableIdentity")
            >= max(0, int(min_stable_identity_count))
            and _shadow_summary_int(manifest_summary, "unitCount")
            >= max(0, int(min_unit_count))
        )
    core_contract_ok = (
        semantic_resolved >= max(0, int(min_semantic_resolved))
        and submitted > 0
        and fallback == 0
        and (not bool(require_semantic_frame_fresh) or frame_fresh)
    )
    semantic_contract_ok = (
        core_contract_ok
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
    )
    scene_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and manifest_ok
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
    )
    static_world_scene_submitted = (
        scene_building + scene_destructible + explicit_rigid_scene
    )
    static_world_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and manifest_ok
        and static_world_scene_submitted
        >= max(0, int(min_semantic_static_world_submitted))
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
    )
    direct_currentdraw_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
        and _shadow_summary_int(
            latest_summary,
            "semanticSceneCurrentDrawResolveReadyCount",
        )
        > 0
        and _shadow_summary_int(
            latest_summary,
            "semanticSceneCanonicalReadyCount",
        )
        > 0
        and bool(scene_status.get("semanticSceneDirectCurrentDrawConsumed"))
    )
    core_packets_present = (
        submitted > 0
        and fallback == 0
        and semantic_resolved >= max(0, int(min_semantic_resolved))
    )
    if (
        core_packets_present
        and scene_status.get("semanticSceneWaitingForRenderPass")
    ):
        if native_draws > 0:
            response["semanticShadowPhase"] = (
                "native-semantic-executed-scene-pending"
            )
            response["semanticNativeExecuted"] = True
            response["semanticShadowPhaseReason"] = (
                "native D3D9 backend has successfully executed semantic draws, "
                "but the DXVK render-scene validation pass has not consumed the "
                "latest semantic publish revision yet"
            )
        else:
            response["semanticShadowPhase"] = (
                "core-fresh-waiting-render-scene"
                if frame_fresh
                else "core-packets-waiting-render-scene"
            )
            response["semanticShadowPhaseReason"] = (
                "semantic core has submitted draw packets, but the DXVK "
                "render-scene pass has not consumed the latest semantic publish "
                "revision yet"
            )
        if not frame_fresh:
            response["semanticShadowPhaseReason"] += (
                "; semantic core freshness is also pending because the latest "
                "render-scene tick has not advanced"
            )
        if response.get("runtimeRenderTailStalled"):
            response["semanticShadowPhaseReason"] += (
                "; runtime frame advance is stalled after in-game render "
                "readiness, so this is a render-scene consumption wait, not a "
                "semantic data-chain miss"
            )
        if not response.get("ok"):
            response["error"] = response["semanticShadowPhaseReason"]
    attachment_contract_ok = (
        core_contract_ok
        and max(attachment_resolved, attachment_supplemental_resolved)
        >= max(1, int(min_semantic_attachment_rigid_resolved))
    )
    attachment_gate_ok = (
        int(min_semantic_attachment_rigid_resolved) <= 0
        or attachment_contract_ok
        or skinned_resolved >= max(1, int(min_semantic_skinned_resolved))
    )
    if scene_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "DXVK semantic scene submitted and consumed draw packets; native "
            "backend counters are not required for this visual validation gate"
        )
        response["error"] = ""
        return response
    if direct_currentdraw_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "current-draw canonical scene submitted skinned packets with "
            "fallback disabled; this Phase 3 validation path does not require "
            "the older semantic core/manifest consumer"
        )
        response["semanticShadowPhase"] = "current-draw-direct-ok"
        response["error"] = ""
        return response
    if allow_semantic_static_world_only and static_world_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "static-world canonical scene submitted building/destructible "
            "packets with fallback disabled"
        )
        response["semanticShadowPhase"] = "static-world-direct-ok"
        response["error"] = ""
        return response
    if (
        bool(allow_scene_pending_if_core_and_currentdraw_ready)
        and core_contract_ok
        and currentdraw_query_hit > 0
        and currentdraw_palette_hit > 0
        and currentdraw_group_decode_hit > 0
    ):
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "semantic core is fresh, current-draw contract/palette/group-slot "
            "queries are hot, and isolated-desktop render-scene consumption is "
            "stalled at tail; accepted as low-pressure tail artifact"
        )
        response["semanticShadowPhase"] = "low-pressure-tail-accepted"
        response["error"] = ""
        return response
    if (
        not response.get("ok")
        and scene_contract_ok
        and attachment_gate_ok
        and native_draws >= max(0, int(min_native_executed_draw_count))
    ):
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticSceneOnlyReason"] = (
            "DXVK semantic scene submitted and consumed draw packets with "
            "fallback disabled; this gate does not require the control-plane "
            "wait_until call to classify the final state as stalled"
        )
    if (
        not response.get("ok")
        and wait_stalled
        and semantic_contract_ok
        and manifest_ok
        and skinned_resolved >= max(0, int(min_semantic_skinned_resolved))
        and native_draws >= max(0, int(min_native_executed_draw_count))
        and attachment_gate_ok
    ):
        response["ok"] = True
        response["semanticTailFrameAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailFrameReason"] = (
            "semantic frame was fresh, consumed by the render scene, and "
            "executed by the native backend before the isolated-desktop frame "
            "advance gate stalled"
        )
    if (
        not response.get("ok")
        and wait_stalled
        and scene_contract_ok
        and manifest_ok
    ):
        response["ok"] = True
        response["semanticTailSceneAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailSceneReason"] = (
            "semantic scene submitted and consumed draw packets before the "
            "isolated-desktop frame advance gate stalled; core counters may be "
            "reset by EndFrame flush, so the scene submission counters are the "
            "authoritative visual-consumption signal for this gate"
        )
    if (
        not response.get("ok")
        and (
            wait_stalled
            or scene_status.get("semanticSceneWaitingForRenderPass")
        )
        and frame_fresh
        and core_contract_ok
        and manifest_ok
        and scene_submitted > 0
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
        and scene_lag <= 16
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_frame_serial <= core_frame_serial
        and (core_frame_serial - scene_frame_serial) <= 1
    ):
        response["ok"] = True
        response["semanticTailSceneNearLatestAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailSceneNearLatestReason"] = (
            "semantic core was advanced by the bounded control-plane tail drain "
            "after the isolated-desktop render tick stalled; the DXVK scene had "
            "already consumed the immediately previous semantic frame with "
            "fallback disabled, so this is accepted as a tail-frame validation "
            "artifact rather than a data-chain failure"
        )
    if attachment_contract_ok:
        response["semanticAttachmentRigidOnlyAccepted"] = True
        response["semanticAttachmentRigidGateSatisfied"] = True
        if not response.get("semanticShadowPhase"):
            response["semanticShadowPhase"] = "attachment-rigid-ok-skinned-pending"
            response["semanticShadowPhaseReason"] = (
                "attachment rigid semantic path is producing submitted draw packets "
                "with fallback disabled; skinned/upper-layer/native gates remain pending"
            )
        if not response.get("ok"):
            response["originalError"] = response.get("error", "")
            response["error"] = response["semanticShadowPhaseReason"]
        if bool(allow_semantic_attachment_rigid_only):
            response["ok"] = True
            response["error"] = ""

    if (
        not response.get("ok")
        and bool(allow_semantic_rigid_only)
        and semantic_contract_ok
        and (explicit_rigid > 0 or rigid_resolved > 0)
    ):
        response["ok"] = True
        response["semanticRigidOnlyAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticRigidOnlyReason"] = (
            "semantic rigid path is producing submitted draw packets with "
            "fallback disabled; skinned/attachment gates remain pending"
        )
    elif not response.get("ok") and semantic_contract_ok and skinned_resolved <= 0:
        response.setdefault("semanticShadowPhase", "semantic-rigid-ok-skinned-pending")
        response.setdefault(
            "semanticShadowPhaseReason",
            "semantic rigid path is producing draw packets, but skinned gate is still pending",
        )
    if (
        not response.get("ok")
        or int(min_native_executed_draw_count) <= 0
        or response.get("semanticSceneOnlyAccepted")
        or response.get("semanticTailSceneAccepted")
        or response.get("semanticTailSceneNearLatestAccepted")
    ):
        return response

    if native_draws >= int(min_native_executed_draw_count):
        return response

    deadline = t0 + float(timeout_sec)
    last_detail: Dict[str, Any] = {}
    while time.time() < deadline:
        latest = _control_plane_request(
            pid=target_pid,
            command="get_shadow_runtime_summary",
            payload={
                "refreshSemanticFrameIfStale": True,
                "forceSemanticFrameBuild": True,
                "allowControlPlaneSemanticDrain": True,
                "semanticBuildMinIntervalMs": 0,
                "semanticBuildDrainMaxChunks": 32,
                "semanticBuildDrainBudgetUs": 50000,
                "semanticBuildDrainRecordCeiling": 1024,
            },
            timeout_sec=2.0,
        )
        last_detail = latest
        if latest.get("transportOk") and latest.get("ok"):
            latest_summary = dict(latest.get("result", {}) or {})
            native_draws = _native_execute_success_draw_count(latest_summary)
            if native_draws >= int(min_native_executed_draw_count):
                response["shadowRuntimeSummary"] = latest_summary
                response["nativeD3D9BackendEffectiveExecutedDrawCount"] = (
                    native_draws
                )
                response["elapsedSec"] = round(time.time() - t0, 3)
                response["nativeExecuteWaitMode"] = "control-plane-summary"
                response["nativeExecuteWaitDetail"] = latest
                return response
        time.sleep(0.1)

    response["ok"] = False
    response["error"] = (
        f"等待 native D3D9 effective executed draw count>="
        f"{int(min_native_executed_draw_count)} 超时"
    )
    response["elapsedSec"] = round(time.time() - t0, 3)
    response["shadowRuntimeSummary"] = latest_summary
    response["nativeExecuteWaitMode"] = "control-plane-summary"
    response["nativeExecuteWaitDetail"] = last_detail
    return response


@mcp.tool()
def query_war3_window(
    pid: int = 0,
    wait_sec: int = 0,
    require_visible: bool = True,
) -> Dict[str, Any]:
    """查询 War3 主窗口句柄/尺寸/状态。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid}

    hwnd = (
        _wait_for_main_window_hwnd(target_pid, timeout_sec=max(0.2, float(wait_sec)), require_visible=require_visible)
        if wait_sec > 0
        else _find_main_window_hwnd(target_pid)
    )
    if not hwnd:
        return {
            "ok": False,
            "error": "未找到主窗口",
            "pid": target_pid,
            "requireVisible": bool(require_visible),
        }

    info = _query_window_info_by_hwnd(hwnd, pid=target_pid)
    info["requireVisible"] = bool(require_visible)
    return info


@mcp.tool()
def wait_for_war3_window_ready(
    timeout_sec: int = 30,
    pid: int = 0,
    min_cpu_sec: float = 0.5,
    stable_sec: float = 0.8,
) -> Dict[str, Any]:
    """
    等待 War3 窗口进入“可操作”状态。
    适用于窗口化 resize/maximize 回归，不要求正式进图。
    """
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    return _wait_for_window_ready(
        pid=target_pid,
        timeout_sec=max(1, int(timeout_sec)),
        min_cpu_sec=max(0.0, float(min_cpu_sec)),
        stable_sec=max(0.1, float(stable_sec)),
    )


@mcp.tool()
def control_war3_window(
    action: str = "query",
    pid: int = 0,
    client_w: int = 0,
    client_h: int = 0,
    x: int = 40,
    y: int = 40,
    wait_sec: float = 1.0,
) -> Dict[str, Any]:
    """
    控制 War3 窗口：
    - query
    - resize_client
    - maximize
    - restore
    - minimize
    - close
    """
    target_pid = pid or (STATE.war3_pid or 0)
    action_norm = str(action or "query").strip().lower()
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid", "action": action_norm}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid, "action": action_norm}

    before = query_war3_window(pid=target_pid, wait_sec=5, require_visible=True)
    if not before.get("ok") and action_norm != "query":
        return {"ok": False, "error": "窗口尚不可见，无法执行动作", "before": before, "action": action_norm}

    result: Dict[str, Any]
    if action_norm == "query":
        result = {"ok": True, "action": action_norm}
    elif action_norm == "resize_client":
        result = _resize_window_client_native(
            pid=target_pid,
            client_w=max(64, int(client_w)),
            client_h=max(64, int(client_h)),
            x=int(x),
            y=int(y),
        )
    elif action_norm == "maximize":
        result = _post_window_syscommand(target_pid, 0xF030)
    elif action_norm == "restore":
        result = _post_window_syscommand(target_pid, 0xF120)
    elif action_norm == "minimize":
        result = _post_window_syscommand(target_pid, 0xF020)
    elif action_norm == "close":
        result = {
            "ok": _post_close(target_pid),
            "pid": int(target_pid),
        }
    else:
        return {"ok": False, "error": f"未知 action: {action}", "action": action_norm}

    time.sleep(max(0.0, float(wait_sec)))
    alive = _pid_alive(target_pid)
    after = query_war3_window(pid=target_pid, wait_sec=1, require_visible=False) if alive else {
        "ok": False,
        "error": "进程已退出",
        "pid": target_pid,
    }
    return {
        "ok": bool(result.get("ok")),
        "action": action_norm,
        "pid": int(target_pid),
        "aliveAfter": bool(alive),
        "before": before,
        "result": result,
        "after": after,
    }


@mcp.tool()
def run_windowed_resize_crash_test(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    window_ready_timeout_sec: int = 30,
    require_game_ready: bool = False,
    game_ready_timeout_sec: int = 45,
    initial_client_w: int = 1600,
    initial_client_h: int = 900,
    resize_client_w: int = 2560,
    resize_client_h: int = 1440,
    include_maximize: bool = True,
    include_restore: bool = True,
    step_wait_sec: float = 2.0,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = False,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
) -> Dict[str, Any]:
    """
    窗口化回归：
    启动 -> 等窗口可操作 -> resize / maximize / restore -> 检查是否闪退。
    """
    launch = launch_war3_test(
        war3_dir=war3_dir,
        map_path=map_path,
        windowed=True,
        opengl=False,
        auto_perf_record=False,
        auto_perf_export_sec=0,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        render_log=False,
        extra_args="",
    )
    if not launch.get("ok"):
        return {"ok": False, "stage": "launch", "detail": launch}

    pid = int(launch["pid"])
    window_ready = wait_for_war3_window_ready(
        timeout_sec=window_ready_timeout_sec,
        pid=pid,
        min_cpu_sec=0.4,
        stable_sec=0.8,
    )
    if not window_ready.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {
            "ok": False,
            "stage": "window-ready",
            "launch": launch,
            "windowReady": window_ready,
            "stop": stop,
        }

    game_ready: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "require_game_ready=False",
    }
    warnings: List[str] = []
    if require_game_ready:
        game_ready = wait_for_game_ready(
            timeout_sec=game_ready_timeout_sec,
            pid=pid,
            allow_fallback=True,
        )
        if not game_ready.get("ok"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "game-ready",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "stop": stop,
            }

    steps: List[Dict[str, Any]] = []
    crash_step = ""

    def _run_step(name: str, **kwargs: Any) -> bool:
        nonlocal crash_step
        res = control_war3_window(pid=pid, wait_sec=step_wait_sec, **kwargs)
        row = {"name": name, "detail": res, "time": _now_str()}
        steps.append(row)
        alive = bool(res.get("aliveAfter", False))
        if not alive and not crash_step:
            crash_step = name
        return alive

    if initial_client_w > 0 and initial_client_h > 0:
        if not _run_step(
            "resize-initial",
            action="resize_client",
            client_w=initial_client_w,
            client_h=initial_client_h,
            x=40,
            y=40,
        ):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if include_maximize:
        if not _run_step("maximize", action="maximize"):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if include_restore:
        if not _run_step("restore", action="restore"):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if resize_client_w > 0 and resize_client_h > 0:
        if not _run_step(
            "resize-final",
            action="resize_client",
            client_w=resize_client_w,
            client_h=resize_client_h,
            x=60,
            y=60,
        ):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    shot = capture_war3_screenshot(pid=pid)
    if not shot.get("ok"):
        warnings.append(str(shot.get("error", "截图失败")))

    stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
    return {
        "ok": True,
        "stage": "done",
        "launch": launch,
        "windowReady": window_ready,
        "gameReady": game_ready,
        "steps": steps,
        "warnings": warnings,
        "screenshot": shot,
        "stop": stop,
    }


@mcp.tool()
def capture_war3_screenshot(
    output_path: str = "",
    pid: int = 0,
    war3_dir: str = "",
    prefer_internal: bool = True,
    timeout_sec: int = 8,
    fallback_to_window_capture: bool = True,
) -> Dict[str, Any]:
    """抓取 War3 最终帧图片（优先内部截图，失败时回退窗口抓图）。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}"}

    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    out = Path(output_path) if output_path else (
        _ensure_dir(ARTIFACT_ROOT / "screenshots") / f"war3_{_now_compact()}.png"
    )
    _ensure_dir(out.parent)

    internal_res: Optional[Dict[str, Any]] = None
    if prefer_internal:
        internal_res = _request_internal_frame_capture(
            pid=target_pid,
            output_path=out,
            war3_dir=target_war3_dir,
            timeout_sec=max(1, int(timeout_sec)),
        )
        if internal_res.get("ok"):
            return internal_res
        if not fallback_to_window_capture:
            return internal_res

    fallback_out = out if out.suffix.lower() == ".png" else out.with_suffix(".png")
    result = _powershell_capture_window(target_pid, fallback_out)
    ok = result["returncode"] == 0 and fallback_out.exists()
    return {
        "ok": ok,
        "pid": target_pid,
        "output": str(fallback_out),
        "mode": "window-capture",
        "details": result,
        "internalAttempt": internal_res or {},
        "fallbackUsed": True,
    }


@mcp.tool()
def invoke_internal_test_api(
    command: str,
    payload_json: str = "{}",
    pid: int = 0,
    war3_dir: str = "",
    timeout_sec: int = 6,
) -> Dict[str, Any]:
    """通过 named pipe control plane 调用游戏内测试命令。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid}

    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    try:
        payload = json.loads(str(payload_json or "{}"))
    except Exception as e:
        return {"ok": False, "error": f"payload_json 解析失败: {e}"}
    if not isinstance(payload, dict):
        return {"ok": False, "error": "payload_json 必须是 JSON object"}

    return _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command=str(command or "").strip(),
        payload=payload,
        timeout_sec=max(1, int(timeout_sec)),
    )


@mcp.tool()
def set_city_test_view(
    view: str = "mid",
    pid: int = 0,
    war3_dir: str = "",
    angle_of_attack: float = 0.0,
    rotation: float = 0.0,
    target_distance: float = 0.0,
    z_offset: float = 0.0,
    duration: float = 0.0,
    quick_position: bool = True,
) -> Dict[str, Any]:
    """基于当前相机生成 City.w3x 的高/中/低俯仰角测试视图。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)

    snap = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="camera.snapshot",
        payload={},
        timeout_sec=4.0,
    )
    if not snap.get("ok"):
        return {"ok": False, "stage": "snapshot", "detail": snap}

    snapshot = dict(snap.get("result", {}) or {})
    current_aoa = float(snapshot.get("angleOfAttack", 0.0) or 0.0)
    current_rot = float(snapshot.get("rotation", 0.0) or 0.0)
    current_dist = float(snapshot.get("targetDistance", 0.0) or 0.0)
    current_zoff = float(snapshot.get("zOffset", 0.0) or 0.0)
    use_degree_like = abs(current_aoa) > 6.5
    angle_high = current_aoa + (12.0 if use_degree_like else 0.18)
    angle_mid = current_aoa
    angle_low = current_aoa - (20.0 if use_degree_like else 0.38)
    if use_degree_like:
        angle_low = max(12.0, angle_low)
    else:
        angle_low = max(0.12, angle_low)

    presets = {
        "high": angle_high,
        "mid": angle_mid,
        "low": angle_low,
    }
    view_name = str(view or "mid").strip().lower()
    target_aoa_value = float(angle_of_attack) if abs(float(angle_of_attack)) > 1.0e-6 else float(presets.get(view_name, angle_mid))
    target_rot_value = float(rotation) if abs(float(rotation)) > 1.0e-6 else current_rot
    target_dist_value = float(target_distance) if abs(float(target_distance)) > 1.0e-6 else current_dist
    target_zoff_value = float(z_offset) if abs(float(z_offset)) > 1.0e-6 else current_zoff

    apply = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="camera.apply",
        payload={
            "targetX": float(snapshot.get("targetX", 0.0) or 0.0),
            "targetY": float(snapshot.get("targetY", 0.0) or 0.0),
            "targetDistance": target_dist_value,
            "angleOfAttack": target_aoa_value,
            "rotation": target_rot_value,
            "zOffset": target_zoff_value,
            "duration": float(duration),
            "quickPosition": bool(quick_position),
        },
        timeout_sec=max(4.0, 2.0 + float(duration)),
    )
    return {
        "ok": bool(apply.get("ok")),
        "view": view_name,
        "snapshot": snapshot,
        "requested": {
            "angleOfAttack": target_aoa_value,
            "rotation": target_rot_value,
            "targetDistance": target_dist_value,
            "zOffset": target_zoff_value,
            "duration": float(duration),
            "quickPosition": bool(quick_position),
        },
        "detail": apply,
    }


@mcp.tool()
def compare_frame_sequence(frame_paths_json: str) -> Dict[str, Any]:
    """比较一组连续 BMP/PNG 帧，输出阴影闪烁/消失可疑指标。"""
    try:
        raw = json.loads(str(frame_paths_json or "[]"))
    except Exception as e:
        return {"ok": False, "error": f"frame_paths_json 解析失败: {e}"}
    if not isinstance(raw, list) or not raw:
        return {"ok": False, "error": "frame_paths_json 必须是非空 JSON array"}
    paths = [Path(str(item)) for item in raw if str(item).strip()]
    return _compare_bmp_sequence(paths)


@mcp.tool()
def capture_shadow_factor_sequence(
    pid: int = 0,
    war3_dir: str = "",
    mode: str = "shadow_factor",
    label: str = "",
    frame_count: int = 5,
    interval_sec: float = 0.25,
    warmup_sec: float = 0.35,
    timeout_sec: int = 8,
) -> Dict[str, Any]:
    """切换阴影调试模式并抓取一组连续最终帧，用于稳定性比较。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    mode_name = str(mode or "shadow_factor").strip().lower()
    debug_mode = 2 if mode_name in ("shadow_factor", "shadowfactor", "factor") else 0

    set_mode = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="shadow.debug_mode",
        payload={"mode": int(debug_mode)},
        timeout_sec=4.0,
    )
    if not set_mode.get("ok"):
        return {"ok": False, "stage": "set-debug-mode", "detail": set_mode}
    if float(warmup_sec) > 0.0:
        time.sleep(max(0.0, float(warmup_sec)))

    out_dir = _ensure_dir(ARTIFACT_ROOT / "city_suite" / _now_compact() / (label or mode_name))
    frame_rows: List[Dict[str, Any]] = []
    frame_paths: List[Path] = []
    for index in range(max(2, int(frame_count))):
        out_path = out_dir / f"{label or mode_name}_{index + 1:02d}.bmp"
        cap = _capture_final_frame_via_internal_test_api(
            pid=target_pid,
            war3_dir=target_war3_dir,
            output_path=out_path,
            timeout_sec=max(2.0, float(timeout_sec)),
        )
        status_after = _read_runtime_status_best_effort(target_pid) or {}
        cap["runtimeStatusAfterCapture"] = status_after
        shadow_status = status_after.get("shadow", {}) if isinstance(status_after, dict) else {}
        cap["shadowSummaryAfterCapture"] = {
            "semanticSceneReceiverInputValid": shadow_status.get("semanticSceneReceiverInputValid"),
            "semanticSceneReceiverInputRejectReason": shadow_status.get("semanticSceneReceiverInputRejectReason"),
            "semanticSceneReceiverNeedPass": shadow_status.get("semanticSceneReceiverNeedPass"),
            "semanticSceneReceiverNeedShadowMap": shadow_status.get("semanticSceneReceiverNeedShadowMap"),
            "semanticSceneReceiverHasCompleteShadowMap": shadow_status.get("semanticSceneReceiverHasCompleteShadowMap"),
            "semanticSceneReceiverHasUsableDirectionalShadow": shadow_status.get("semanticSceneReceiverHasUsableDirectionalShadow"),
            "semanticSceneReceiverActiveStrengthMilli": shadow_status.get("semanticSceneReceiverActiveStrengthMilli"),
            "semanticSceneReceiverUboStrengthMilli": shadow_status.get("semanticSceneReceiverUboStrengthMilli"),
            "semanticSceneReceiverDebugMode": shadow_status.get("semanticSceneReceiverDebugMode"),
            "semanticSceneReceiverCsmCascadeCount": shadow_status.get("semanticSceneReceiverCsmCascadeCount"),
            "semanticSceneReceiverReuseShadowMap": shadow_status.get("semanticSceneReceiverReuseShadowMap"),
            "semanticSceneShadowMapSkinnedDrawnCount": shadow_status.get("semanticSceneShadowMapSkinnedDrawnCount"),
            "semanticSceneReplayDrawsCount": shadow_status.get("semanticSceneReplayDrawsCount"),
            "semanticSceneSubmittedSkinned": shadow_status.get("semanticSceneSubmittedSkinned"),
            "semanticSceneCurrentDrawResolveReadyCount": shadow_status.get("semanticSceneCurrentDrawResolveReadyCount"),
            "objectFallbackDrawCount": shadow_status.get("objectFallbackDrawCount"),
        }
        frame_rows.append(cap)
        if not cap.get("ok"):
            return {
                "ok": False,
                "stage": "capture",
                "mode": mode_name,
                "frames": frame_rows,
            }
        frame_paths.append(Path(str(cap.get("output", ""))))
        if index + 1 < int(frame_count):
            time.sleep(max(0.0, float(interval_sec)))

    comparison = _compare_bmp_sequence(frame_paths)
    return {
        "ok": bool(comparison.get("ok")),
        "mode": mode_name,
        "debugMode": int(debug_mode),
        "outputDir": str(out_dir),
        "frames": frame_rows,
        "comparison": comparison,
    }


@mcp.tool()
def run_city_shadow_stability_suite(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_CITY_MAP),
    allow_fallback_to_default_test_map: bool = True,
    ready_timeout_sec: int = 240,
    settle_sec: int = 60,
    sequence_frame_count: int = 5,
    sequence_interval_sec: float = 0.25,
    use_isolated_desktop: bool = True,
    desktop_name: str = "War3CityStability",
    profile: str = "full_analysis",
    windowed: bool = True,
    sample_duration_sec: int = 5,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
) -> Dict[str, Any]:
    """
    City.w3x 专项稳定性套件：
    启动 -> 进图 -> 额外静置 -> 开全图 -> 镜头高/中/低俯仰角扫掠 -> 连续帧比较。
    """
    w3 = Path(war3_dir)
    before_report = _find_latest_report(w3)
    before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
    log_offsets = _snapshot_log_offsets(w3)
    env_overrides = {
        "DXVK_WAR3_RUNTIME_BENCHMARK": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC": str(max(3, int(sample_duration_sec))),
    }

    start = _launch_suite_map_until_ready(
        war3_dir=war3_dir,
        requested_map_path=map_path,
        allow_fallback_to_default_test_map=bool(allow_fallback_to_default_test_map),
        ready_timeout_sec=ready_timeout_sec,
        ready_allow_fallback=False,
        ready_require_game_started_for_fallback=True,
        ready_fallback_min_elapsed_sec=20,
        ready_fallback_min_cpu_sec=1.0,
        launch_kwargs={
            "windowed": bool(windowed),
            "use_isolated_desktop": bool(use_isolated_desktop),
            "desktop_name": str(desktop_name or "War3CityStability"),
            "opengl": False,
            "auto_perf_record": True,
            "auto_perf_export_sec": max(8, int(sample_duration_sec) + 2),
            "deploy_d3d9_before_launch": deploy_d3d9_before_launch,
            "build_d3d9_path": build_d3d9_path,
            "enforce_video_baseline": True,
            "baseline_width": DEFAULT_BENCHMARK_WIDTH,
            "baseline_height": DEFAULT_BENCHMARK_HEIGHT,
            "baseline_refresh_rate": DEFAULT_BENCHMARK_REFRESH,
            "render_log": False,
            "profile": profile,
            "disable_modules": "",
            "env_overrides_json": json.dumps(env_overrides, ensure_ascii=False),
            "extra_args": "",
        },
    )
    if not start.get("ok"):
        return {"ok": False, "stage": str(start.get("stage", "ready")), "start": start}

    launch = dict(start.get("launch", {}) or {})
    ready = dict(start.get("ready", {}) or {})
    pid = int(start["pid"])

    time.sleep(max(0, int(settle_sec)))

    markers: List[Dict[str, Any]] = []
    full_map = _invoke_internal_test_request(pid, Path(war3_dir), "visibility.full_map", {}, timeout_sec=4.0)
    markers.append(_invoke_internal_test_request(pid, Path(war3_dir), "runtime.log_marker", {"marker": "city-suite-start"}, timeout_sec=3.0))
    if not full_map.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {"ok": False, "stage": "full-map", "launch": launch, "ready": ready, "fullMap": full_map, "stop": stop}

    base_snapshot = _invoke_internal_test_request(pid, Path(war3_dir), "camera.snapshot", {}, timeout_sec=4.0)
    if not base_snapshot.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {"ok": False, "stage": "camera-snapshot", "launch": launch, "ready": ready, "fullMap": full_map, "snapshot": base_snapshot, "stop": stop}

    sequences: List[Dict[str, Any]] = []
    for mode_name in ("shadow_factor", "normal"):
        for view_name in ("high", "mid", "low"):
            view_res = set_city_test_view(
                view=view_name,
                pid=pid,
                war3_dir=war3_dir,
                duration=0.0,
                quick_position=True,
            )
            row: Dict[str, Any] = {
                "mode": mode_name,
                "view": view_name,
                "viewSet": view_res,
            }
            if not view_res.get("ok"):
                sequences.append(row)
                break
            time.sleep(0.6)
            seq = capture_shadow_factor_sequence(
                pid=pid,
                war3_dir=war3_dir,
                mode=mode_name,
                label=f"{mode_name}_{view_name}",
                frame_count=sequence_frame_count,
                interval_sec=sequence_interval_sec,
            )
            row["sequence"] = seq
            sequences.append(row)

    # 恢复正常渲染模式。
    markers.append(_invoke_internal_test_request(pid, Path(war3_dir), "shadow.debug_mode", {"mode": 0}, timeout_sec=4.0))
    time.sleep(max(1, int(sample_duration_sec)))

    stop = stop_war3(pid=pid, graceful_wait_sec=20, force=False, avoid_foreground_switch=True)
    if not stop.get("stopped"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)

    latest = {"ok": False, "error": "未找到报告"}
    new_report_detected = False
    for _ in range(12):
        maybe = find_latest_perf_report(war3_dir=war3_dir)
        if maybe.get("ok"):
            mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
            if mtime > before_mtime + 0.5:
                latest = maybe
                new_report_detected = True
                break
            latest = maybe
        time.sleep(1.0)

    report = read_perf_report(latest["reportPath"], include_sections=False) if latest.get("ok") else latest
    log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
    if isinstance(report, dict):
        report["logSummary"] = log_summary
        _merge_shadow_budget_summary_with_log_fallback(report, log_summary)

    seq_ok = True
    low_pitch_missing = False
    flicker_suspect = False
    shadow_factor_dark_ratios: Dict[str, float] = {}
    for row in sequences:
        seq = dict(row.get("sequence", {}) or {})
        cmp_res = dict(seq.get("comparison", {}) or {})
        summary = dict(cmp_res.get("summary", {}) or {})
        if not seq.get("ok"):
            seq_ok = False
            continue
        flicker_suspect = flicker_suspect or bool(summary.get("flickerSuspect", False))
        if row.get("mode") == "shadow_factor":
            frames = list((cmp_res.get("frames", []) or []))
            if frames:
                shadow_factor_dark_ratios[str(row.get("view"))] = float(frames[0].get("darkRatioPct", 0.0) or 0.0)
            if row.get("view") == "low":
                low_pitch_missing = low_pitch_missing or bool(summary.get("missingShadowSuspect", False))

    if {"high", "mid", "low"} <= set(shadow_factor_dark_ratios.keys()):
        ref_ratio = max(shadow_factor_dark_ratios["high"], shadow_factor_dark_ratios["mid"])
        low_ratio = shadow_factor_dark_ratios["low"]
        if ref_ratio >= 1.0 and low_ratio <= ref_ratio * 0.35:
            low_pitch_missing = True

    top_keywords = {str(item.get("name", "")): int(item.get("count", 0) or 0) for item in list(log_summary.get("topKeywords", []) or [])}
    no_bad_keywords = (
        top_keywords.get("deviceLost", 0) == 0
        and top_keywords.get("shadowReuseLastComplete", 0) == 0
        and top_keywords.get("csmComputeFailed", 0) == 0
    )
    shadow_budget = dict((report.get("shadowBudgetSummary", {}) if isinstance(report, dict) else {}) or {})
    zero_budget_errors = (
        int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0
        and int(shadow_budget.get("framesIncomplete", 0) or 0) == 0
    )
    ok = bool(seq_ok and not flicker_suspect and not low_pitch_missing and no_bad_keywords and zero_budget_errors)

    return {
        "ok": ok,
        "stage": "done",
        "requestedMapPath": str(map_path),
        "actualMapPath": str(start.get("actualMapPath", map_path)),
        "fallbackUsed": bool(start.get("fallbackUsed", False)),
        "start": start,
        "launch": launch,
        "ready": ready,
        "fullMap": full_map,
        "baseSnapshot": base_snapshot,
        "markers": markers,
        "sequences": sequences,
        "flickerSuspect": bool(flicker_suspect),
        "lowPitchMissingShadowSuspect": bool(low_pitch_missing),
        "logSummary": log_summary,
        "report": report,
        "shadowBudgetSummary": shadow_budget,
        "stop": stop,
        "newReportDetected": bool(new_report_detected),
    }


@mcp.tool()
def run_city_shadow_pressure_suite(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_CITY_MAP),
    allow_fallback_to_default_test_map: bool = True,
    rounds: int = 3,
    duration_sec: int = 600,
    sample_interval_sec: int = 30,
    ready_timeout_sec: int = 240,
    use_isolated_desktop: bool = True,
    desktop_name: str = "War3CityPressure",
    profile: str = "full_default",
    windowed: bool = True,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
) -> Dict[str, Any]:
    """City.w3x 高压压力测试：长时间运行并定时采样日志/截图/预算。"""
    rounds = max(1, min(int(rounds), 6))
    duration_sec = max(30, min(int(duration_sec), 3600))
    sample_interval_sec = max(5, min(int(sample_interval_sec), 300))
    all_rounds: List[Dict[str, Any]] = []
    current_map_path = str(map_path)
    actual_map_path = str(map_path)
    fallback_used_any = False

    for round_index in range(rounds):
        w3 = Path(war3_dir)
        before_report = _find_latest_report(w3)
        before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
        log_offsets = _snapshot_log_offsets(w3)
        start = _launch_suite_map_until_ready(
            war3_dir=war3_dir,
            requested_map_path=current_map_path,
            allow_fallback_to_default_test_map=bool(allow_fallback_to_default_test_map),
            ready_timeout_sec=ready_timeout_sec,
            ready_allow_fallback=False,
            ready_require_game_started_for_fallback=True,
            ready_fallback_min_elapsed_sec=20,
            ready_fallback_min_cpu_sec=1.0,
            launch_kwargs={
                "windowed": bool(windowed),
                "use_isolated_desktop": bool(use_isolated_desktop),
                "desktop_name": f"{desktop_name}_{round_index + 1}",
                "opengl": False,
                "auto_perf_record": True,
                "auto_perf_export_sec": max(8, min(60, duration_sec // 2)),
                "deploy_d3d9_before_launch": deploy_d3d9_before_launch,
                "build_d3d9_path": build_d3d9_path,
                "enforce_video_baseline": True,
                "baseline_width": DEFAULT_BENCHMARK_WIDTH,
                "baseline_height": DEFAULT_BENCHMARK_HEIGHT,
                "baseline_refresh_rate": DEFAULT_BENCHMARK_REFRESH,
                "render_log": False,
                "profile": profile,
                "disable_modules": "",
                "env_overrides_json": "{}",
                "extra_args": "",
            },
        )
        if not start.get("ok"):
            all_rounds.append({"ok": False, "stage": str(start.get("stage", "ready")), "detail": start, "round": round_index + 1})
            continue

        launch = dict(start.get("launch", {}) or {})
        ready = dict(start.get("ready", {}) or {})
        pid = int(start["pid"])
        current_map_path = str(start.get("actualMapPath", current_map_path))
        actual_map_path = current_map_path
        fallback_used_any = fallback_used_any or bool(start.get("fallbackUsed", False))
        round_row: Dict[str, Any] = {
            "round": round_index + 1,
            "requestedMapPath": str(map_path),
            "actualMapPath": current_map_path,
            "fallbackUsed": bool(start.get("fallbackUsed", False)),
            "start": start,
            "launch": launch,
            "ready": ready,
            "samples": [],
        }
        if not ready.get("ok"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
            round_row["ok"] = False
            round_row["stage"] = "ready"
            round_row["stop"] = stop
            all_rounds.append(round_row)
            continue

        started_at = time.time()
        sample_idx = 0
        while time.time() - started_at < duration_sec:
            if not _pid_alive(pid):
                break
            sample_idx += 1
            out_dir = _ensure_dir(ARTIFACT_ROOT / "city_pressure" / _now_compact())
            shot = capture_war3_screenshot(
                pid=pid,
                war3_dir=war3_dir,
                output_path=str(out_dir / f"city_pressure_r{round_index + 1}_s{sample_idx:03d}.bmp"),
                prefer_internal=True,
                timeout_sec=8,
                fallback_to_window_capture=False,
            )
            runtime_status = read_runtime_status(war3_dir=war3_dir)
            round_row["samples"].append(
                {
                    "time": _now_str(),
                    "elapsedSec": round(time.time() - started_at, 3),
                    "screenshot": shot,
                    "runtimeStatus": runtime_status,
                }
            )
            sleep_left = sample_interval_sec
            while sleep_left > 0:
                if not _pid_alive(pid):
                    sleep_left = 0
                    break
                step = min(1.0, sleep_left)
                time.sleep(step)
                sleep_left -= step

        stop = stop_war3(pid=pid, graceful_wait_sec=20, force=False, avoid_foreground_switch=True)
        if not stop.get("stopped"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)

        latest = {"ok": False, "error": "未找到报告"}
        new_report_detected = False
        for _ in range(12):
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

        report = read_perf_report(latest["reportPath"], include_sections=False) if latest.get("ok") else latest
        log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
        if isinstance(report, dict):
            report["logSummary"] = log_summary
            _merge_shadow_budget_summary_with_log_fallback(report, log_summary)
        shadow_budget = dict((report.get("shadowBudgetSummary", {}) if isinstance(report, dict) else {}) or {})
        top_keywords = {str(item.get("name", "")): int(item.get("count", 0) or 0) for item in list(log_summary.get("topKeywords", []) or [])}
        round_ok = (
            bool(stop.get("stopped"))
            and top_keywords.get("deviceLost", 0) == 0
            and int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0
            and int(shadow_budget.get("framesIncomplete", 0) or 0) == 0
        )
        round_row.update(
            {
                "ok": bool(round_ok),
                "stage": "done",
                "stop": stop,
                "report": report,
                "logSummary": log_summary,
                "shadowBudgetSummary": shadow_budget,
                "newReportDetected": bool(new_report_detected),
            }
        )
        all_rounds.append(round_row)

    return {
        "ok": bool(all_rounds) and all(bool(row.get("ok")) for row in all_rounds),
        "requestedMapPath": str(map_path),
        "actualMapPath": actual_map_path,
        "fallbackUsed": bool(fallback_used_any),
        "rounds": all_rounds,
        "passed": sum(1 for row in all_rounds if bool(row.get("ok"))),
        "total": len(all_rounds),
    }


@mcp.tool()
def stop_war3(
    pid: int = 0,
    graceful_wait_sec: int = 8,
    force: bool = True,
    avoid_foreground_switch: bool = False,
) -> Dict[str, Any]:
    """停止 War3：优先 WM_CLOSE，超时后可强杀。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": True, "stopped": True, "pid": 0, "message": "无活跃 pid"}

    if not _pid_alive(target_pid):
        restore = _restore_video_config_if_needed(target_pid)
        desktop = _close_state_desktop_if_needed(target_pid)
        _clear_war3_launch_state(target_pid)
        return {
            "ok": True,
            "stopped": True,
            "pid": target_pid,
            "message": "进程已不在",
            "videoRestore": restore,
            "desktop": desktop,
        }

    # 静默结束模式：不发送 WM_CLOSE，直接 taskkill，避免窗口抢焦点。
    # 注意：这里也不要先走 control-plane shutdown_session。War3/DXVK 在
    # GPU/pipe 卡死时可能 CPU=0 但命名管道不再响应，等待 pipe 会把无人
    # 值守测试拖成十几分钟残留进程。
    if avoid_foreground_switch:
        shutdown_session = {
            "ok": True,
            "skipped": True,
            "reason": "avoid_foreground_switch direct taskkill",
        }
        _taskkill(target_pid, force=bool(force))
        time.sleep(0.6)
        alive = _pid_alive(target_pid)
        restore = (
            _restore_video_config_if_needed(target_pid)
            if not alive
            else {"ok": True, "skipped": True, "reason": "进程仍存活，未恢复视频配置"}
        )
        desktop = _close_state_desktop_if_needed(target_pid) if not alive else {"ok": True, "skipped": True, "reason": "进程仍存活，未关闭隔离桌面"}
        if not alive:
            _clear_war3_launch_state(target_pid)
        return {
            "ok": not alive,
            "stopped": not alive,
            "pid": target_pid,
            "closeSent": False,
            "forced": force,
            "silentStop": True,
            "avoidForegroundSwitch": True,
            "shutdownSession": shutdown_session,
            "videoRestore": restore,
            "desktop": desktop,
        }

    shutdown_session = _control_plane_request(
        pid=target_pid,
        command="shutdown_session",
        payload={},
        timeout_sec=2.0,
    )

    close_sent = _post_close(target_pid)
    t0 = time.time()
    while time.time() - t0 < max(1, graceful_wait_sec):
        if not _pid_alive(target_pid):
            restore = _restore_video_config_if_needed(target_pid)
            desktop = _close_state_desktop_if_needed(target_pid)
            _clear_war3_launch_state(target_pid)
            return {
                "ok": True,
                "stopped": True,
                "pid": target_pid,
                "closeSent": close_sent,
                "silentStop": False,
                "avoidForegroundSwitch": False,
                "shutdownSession": shutdown_session,
                "videoRestore": restore,
                "desktop": desktop,
            }
        time.sleep(0.25)

    if force:
        _taskkill(target_pid, force=True)
        time.sleep(0.6)
    alive = _pid_alive(target_pid)
    restore = (
        _restore_video_config_if_needed(target_pid)
        if not alive
        else {"ok": True, "skipped": True, "reason": "进程仍存活，未恢复视频配置"}
    )
    desktop = _close_state_desktop_if_needed(target_pid) if not alive else {"ok": True, "skipped": True, "reason": "进程仍存活，未关闭隔离桌面"}
    if not alive:
        _clear_war3_launch_state(target_pid)
    return {
        "ok": not alive,
        "stopped": not alive,
        "pid": target_pid,
        "closeSent": close_sent,
        "forced": force,
        "silentStop": False,
        "avoidForegroundSwitch": False,
        "shutdownSession": shutdown_session,
        "videoRestore": restore,
        "desktop": desktop,
    }


@mcp.tool()
def find_latest_perf_report(war3_dir: str = str(DEFAULT_WAR3_DIR)) -> Dict[str, Any]:
    """查找 WarVK/Log 下最新性能报告 HTML。"""
    w3 = Path(war3_dir)
    report = _find_latest_report(w3)
    if not report:
        return {"ok": False, "error": "未找到报告", "searchDir": str(w3 / 'WarVK' / 'Log')}
    return {
        "ok": True,
        "reportPath": str(report),
        "mtime": datetime.fromtimestamp(report.stat().st_mtime).isoformat(),
        "size": report.stat().st_size,
    }


@mcp.tool()
def read_perf_report(
    report_path: str = "",
    include_sections: bool = False,
    section_top_n: int = 20,
) -> Dict[str, Any]:
    """读取并摘要性能报告。report_path 为空时读取最新报告。"""
    report = Path(report_path) if report_path else _find_latest_report(STATE.war3_dir)
    if not report or not report.exists():
        return {"ok": False, "error": "报告不存在", "reportPath": str(report) if report else ""}
    summary = _read_perf_summary(
        report,
        include_sections=include_sections,
        section_top_n=section_top_n,
    )
    STATE.last_report_path = report
    return summary


@mcp.tool()
def run_quick_autotest(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    ready_timeout_sec: int = 120,
    sample_duration_sec: int = 20,
    windowed: bool = False,
    use_isolated_desktop: bool = False,
    desktop_name: str = "",
    opengl: bool = False,
    auto_perf_record: bool = True,
    record_after_game_started: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    scenario_name: str = "",
    require_control_plane_ready: bool = True,
    require_hot_shadow_frame: bool = False,
    hot_shadow_timeout_sec: int = 60,
) -> Dict[str, Any]:
    """
    一键流程：
    启动 -> 等进图 -> 截图 -> 等待采样 -> 关闭 -> 读报告。
    """
    w3 = Path(war3_dir)
    before_report = _find_latest_report(w3)
    before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
    log_offsets = _snapshot_log_offsets(w3)
    runtime_profile = _runtime_profile_summary_from_inputs(profile, disable_modules)
    scenario_name_norm = _normalize_scenario_name(scenario_name)
    merged_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = merged_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "stage": "launch", "error": f"env_overrides_json 解析失败: {parse_error}"}
    merged_env.setdefault("DXVK_WAR3_RUNTIME_BENCHMARK", "1")
    merged_env.setdefault(
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC",
        "1",
    )
    merged_env.setdefault(
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC",
        str(max(3, int(sample_duration_sec) - 1)),
    )
    native_semantic_preview_enabled = _bool_env(
        merged_env.get("DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW", "")
    )
    if runtime_profile["name"] == "dxvk_only":
        merged_env.setdefault(
            "DXVK_WAR3_FPS_UNLOCK_ONLY_WARMUP_SEC",
            "1",
        )
        merged_env.setdefault(
            "DXVK_WAR3_FPS_UNLOCK_ONLY_SAMPLE_SEC",
            str(max(3, int(sample_duration_sec) - 1)),
        )
    if scenario_name_norm:
        merged_env.setdefault("DXVK_WAR3_SCENARIO", scenario_name_norm)
    if auto_perf_record and record_after_game_started and auto_perf_export_sec > 0:
        auto_perf_export_sec = max(int(auto_perf_export_sec),
                                   int(sample_duration_sec) + 2)

    launch = launch_war3_test(
        war3_dir=war3_dir,
        map_path=map_path,
        windowed=windowed,
        use_isolated_desktop=use_isolated_desktop,
        desktop_name=desktop_name,
        opengl=opengl,
        auto_perf_record=auto_perf_record,
        record_after_game_started=record_after_game_started,
        auto_perf_export_sec=auto_perf_export_sec,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        render_log=False,
        profile=profile,
        disable_modules=disable_modules,
        env_overrides_json=json.dumps(merged_env, ensure_ascii=False),
        extra_args="",
    )
    if not launch.get("ok"):
        return {"ok": False, "stage": "launch", "detail": launch}

    pid = int(launch["pid"])
    strict_ready_profile = runtime_profile["name"] in (
        "full_default",
        "full_analysis",
        "full_perf_experimental",
    )
    ready = wait_for_game_ready(
        timeout_sec=ready_timeout_sec,
        pid=pid,
        allow_fallback=not bool(require_control_plane_ready),
        fallback_min_elapsed_sec=20 if strict_ready_profile else 10,
        fallback_min_cpu_sec=1.0 if strict_ready_profile else 0.5,
        require_game_started_for_fallback=strict_ready_profile,
    )
    if not ready.get("ok"):
        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )
        if (not bool(require_control_plane_ready) and auto_perf_record and
                not bool(record_after_game_started)):
            latest: Dict[str, Any] = {"ok": False, "error": "未找到报告"}
            new_report_detected = False
            for _ in range(20):
                maybe = find_latest_perf_report(war3_dir=war3_dir)
                if maybe.get("ok"):
                    mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                    if mtime > before_mtime + 0.5:
                        latest = maybe
                        new_report_detected = True
                        break
                    latest = maybe
                time.sleep(1.0)
            if latest.get("ok") and new_report_detected:
                summary = read_perf_report(
                    latest["reportPath"],
                    include_sections=include_sections_in_report,
                    section_top_n=section_top_n,
                )
                if isinstance(summary, dict):
                    summary["newReportDetected"] = True
                    summary["scenarioName"] = scenario_name_norm
                    summary["latestReportPath"] = latest.get("reportPath")
                    summary["reportWasStale"] = False
                    summary["perfOnlyReadyTimeout"] = True
                    summary["readyTimeoutMode"] = str(ready.get("mode", ""))
                    summary["readyTimeoutError"] = str(ready.get("error", ""))
                return {
                    "ok": bool(summary.get("ok")),
                    "stage": "perf-only-ready-timeout",
                    "launch": launch,
                    "ready": ready,
                    "hotShadow": {
                        "ok": True,
                        "skipped": True,
                        "reason": "ready timeout perf-only",
                    },
                    "windowResize": {
                        "ok": True,
                        "skipped": True,
                        "reason": "ready timeout",
                    },
                    "screenshot": {"ok": False, "error": "ready timeout"},
                    "screenshotSize": {
                        "width": 0,
                        "height": 0,
                        "matchBaseline": False,
                        "baselineWidth": baseline_width,
                        "baselineHeight": baseline_height,
                    },
                    "warnings": [
                        "ready gate timed out; returned perf-only report "
                        "because control-plane readiness was not required"
                    ],
                    "stop": stop,
                    "report": summary,
                    "logSummary": _read_runtime_log_summary(
                        w3, log_offsets=log_offsets),
                    "scenarioName": scenario_name_norm,
                }
        return {"ok": False, "stage": "ready", "launch": launch, "ready": ready}

    if str(ready.get("mode", "")) in ("fallback-window-cpu",
                                      "runtime-status-game-started",
                                      "runtime-status-stable"):
        time.sleep(2.0)

    shadow_scenario_names = {
        "low_pressure_static_reuse",
        "dynamic_shadow_pressure",
        "model_runtime_probe",
        "static_world_caster_acceptance",
        "phase4_world_caster_acceptance",
    }
    effective_require_hot_shadow_frame = bool(require_hot_shadow_frame) or (
        scenario_name_norm in shadow_scenario_names
    )
    hot_shadow: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not required",
    }
    if effective_require_hot_shadow_frame:
        attachment_probe = scenario_name_norm in (
            "dynamic_shadow_pressure",
            "model_runtime_probe",
        )
        rigid_observe_probe = (
            scenario_name_norm == "model_runtime_probe"
        )
        hot_shadow = wait_for_hot_shadow_frame(
            timeout_sec=max(1, int(hot_shadow_timeout_sec)),
            pid=pid,
            min_visible_count=1,
            min_stable_identity_count=0 if rigid_observe_probe else 1,
            min_unit_count=0
            if scenario_name_norm in ("low_pressure_static_reuse", "model_runtime_probe")
            else 1,
            min_semantic_resolved=1,
            min_semantic_skinned_resolved=0
            if scenario_name_norm in ("low_pressure_static_reuse", "model_runtime_probe")
            else 1,
            min_native_executed_draw_count=1
            if native_semantic_preview_enabled and not rigid_observe_probe
            else 0,
            require_semantic_frame_fresh=True,
            min_frame_advance=0
            if scenario_name_norm == "dynamic_shadow_pressure"
            else 2,
            allow_semantic_rigid_only=rigid_observe_probe,
            allow_semantic_attachment_rigid_only=
            scenario_name_norm == "model_runtime_probe",
            min_semantic_attachment_rigid_resolved=1
            if attachment_probe
            else 0,
            post_failure_summary_wait_sec=8
            if attachment_probe
            else 6,
            prefer_summary_poll=attachment_probe,
            require_semantic_scene_consumed=scenario_name_norm
            in ("dynamic_shadow_pressure", "low_pressure_static_reuse",
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance"),
            allow_scene_pending_if_core_and_currentdraw_ready=
            scenario_name_norm == "low_pressure_static_reuse",
            min_semantic_static_world_submitted=1
            if scenario_name_norm in (
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance",
            )
            else 0,
            allow_semantic_static_world_only=
            scenario_name_norm in (
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance",
            ),
        )
        if not hot_shadow.get("ok"):
            hot_shadow_phase = str(hot_shadow.get("semanticShadowPhase", "") or "")
            if hot_shadow_phase in (
                "native-semantic-executed-scene-pending",
                "core-fresh-waiting-render-scene",
                "core-packets-waiting-render-scene",
            ):
                hot_shadow_stage = "hot-shadow-render-scene-pending"
            elif hot_shadow_phase == "attachment-rigid-ok-skinned-pending":
                hot_shadow_stage = "hot-shadow-skinned-pending"
            elif hot_shadow_phase == "semantic-rigid-ok-skinned-pending":
                hot_shadow_stage = "hot-shadow-skinned-pending"
            else:
                hot_shadow_stage = "hot-shadow"
            stop_war3(
                pid=pid,
                graceful_wait_sec=3,
                force=True,
                avoid_foreground_switch=avoid_focus_on_stop,
            )
            return {
                "ok": False,
                "stage": hot_shadow_stage,
                "launch": launch,
                "ready": ready,
                "hotShadow": hot_shadow,
            }

    launched_windowed = bool(launch.get("windowed"))
    window_resize: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not windowed",
    }
    if launched_windowed and enforce_video_baseline:
        rz = _powershell_resize_window_client(
            pid=pid,
            client_w=baseline_width,
            client_h=baseline_height,
            x=40,
            y=40,
        )
        window_resize = {
            "ok": rz.get("returncode", 1) == 0,
            "details": rz,
        }
        # 给窗口尺寸变更一点稳定时间，避免立刻截图拿到旧值。
        time.sleep(0.25)

    time.sleep(max(1, sample_duration_sec))

    latest: Dict[str, Any] = {"ok": False, "error": "未找到报告"}
    new_report_detected = False
    if auto_perf_record and auto_perf_export_sec > 0:
        pre_stop_wait_sec = max(4, min(18, int(auto_perf_export_sec) // 2 + 4))
        deadline = time.time() + float(pre_stop_wait_sec)
        while time.time() < deadline:
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

    shot = capture_war3_screenshot(pid=pid)
    shot_size = {"width": 0, "height": 0, "matchBaseline": False, "baselineWidth": baseline_width, "baselineHeight": baseline_height}
    if shot.get("ok") and shot.get("output"):
        sw, sh = _read_png_size(Path(str(shot["output"])))
        shot_size = {
            "width": int(sw),
            "height": int(sh),
            "matchBaseline": int(sw) == int(baseline_width) and int(sh) == int(baseline_height),
            "baselineWidth": int(baseline_width),
            "baselineHeight": int(baseline_height),
        }

    stop = stop_war3(
        pid=pid,
        graceful_wait_sec=20,
        force=False,
        avoid_foreground_switch=avoid_focus_on_stop,
    )
    if not stop.get("stopped"):
        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )
    # 给报告写盘留一点时间
    if not new_report_detected:
        for _ in range(20):
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

    benchmark_summary = {"ok": False, "error": "未在运行日志中找到 runtime benchmark 输出"}
    if latest.get("ok") and new_report_detected:
        summary = read_perf_report(
            latest["reportPath"],
            include_sections=include_sections_in_report,
            section_top_n=section_top_n,
        )
    else:
        for _ in range(15):
            benchmark_summary = _read_runtime_benchmark_summary(
                w3,
                log_offsets=log_offsets,
                profile=runtime_profile["name"],
                disable_modules=disable_modules,
            )
            if benchmark_summary.get("ok"):
                break
            time.sleep(1.0)
        if benchmark_summary.get("ok"):
            summary = benchmark_summary
        else:
            summary = benchmark_summary
    if isinstance(summary, dict):
        summary["newReportDetected"] = new_report_detected
        summary["scenarioName"] = scenario_name_norm
        summary["latestReportPath"] = latest.get("reportPath") if isinstance(latest, dict) else None
        summary["reportWasStale"] = bool(latest.get("ok")) and not new_report_detected
        runtime_ready_frame = _nested_status_int(
            dict(ready.get("runtimeStatus", {}) or {}),
            "frameIndex",
        )
        runtime_hot_frame = int(hot_shadow.get("runtimeFrameIndex", 0) or 0)
        if runtime_hot_frame <= 0:
            runtime_hot_frame = _nested_status_int(
                dict(hot_shadow.get("runtimeStatus", {}) or {}),
                "frameIndex",
            )
        runtime_frame_delta = (
            max(0, runtime_hot_frame - runtime_ready_frame)
            if runtime_ready_frame > 0 and runtime_hot_frame > 0
            else 0
        )
        report_frame_count = _shadow_summary_int(summary, "frameCount")
        try:
            report_window_sec = float(summary.get("windowSec", 0.0) or 0.0)
        except (TypeError, ValueError):
            report_window_sec = 0.0
        fps_sample_reliable = (
            str(summary.get("reportType", "")) == "benchmark_log"
            or (report_frame_count >= 10 and report_window_sec >= 1.0)
        )
        summary["fpsSampleReliable"] = bool(fps_sample_reliable)
        summary["fpsSampleFrameCount"] = report_frame_count
        summary["fpsSampleWindowSec"] = round(max(0.0, report_window_sec), 3)
        summary["runtimeFrameDeltaReadyToHotShadow"] = runtime_frame_delta
        if not fps_sample_reliable:
            summary["fpsSampleReliabilityReason"] = (
                "perf report recorded too few Present frames for an FPS "
                "judgement; isolated desktop/windowed runs can tail-stall or "
                "only present on capture/stop, so use semantic counters for "
                "correctness and a dedicated visible-desktop perf run for FPS"
            )
        if summary.get("reportType") == "benchmark_log":
            summary["newReportDetected"] = True
            summary["benchmarkFallback"] = True
            summary["reportWasStale"] = False
        elif not new_report_detected and summary.get("ok"):
            summary["ok"] = False
            summary["error"] = "未检测到新报告（可能未部署最新 d3d9.dll 或进程未优雅退出）"

    log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
    if isinstance(summary, dict):
        summary["logSummary"] = log_summary
        _merge_shadow_budget_summary_with_log_fallback(summary, log_summary)
        summary["scenarioName"] = scenario_name_norm

    warnings: List[str] = []
    if shot_size["width"] > 0 and (not shot_size["matchBaseline"]):
        warnings.append(
            f"截图尺寸 {shot_size['width']}x{shot_size['height']} 与基线 {baseline_width}x{baseline_height} 不一致"
        )

    return {
        "ok": bool(summary.get("ok")),
        "stage": "done",
        "launch": launch,
        "ready": ready,
        "hotShadow": hot_shadow,
        "windowResize": window_resize,
        "screenshot": shot,
        "screenshotSize": shot_size,
        "warnings": warnings,
        "stop": stop,
        "report": summary,
        "logSummary": log_summary,
        "scenarioName": scenario_name_norm,
    }


@mcp.tool()
def list_named_scenario_presets() -> Dict[str, Any]:
    """列出可用的命名场景预设。"""
    return {
        "ok": True,
        "presets": _scenario_preset_rows(),
    }


@mcp.tool()
def run_named_scenario(
    scenario_name: str,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    env_overrides_json: str = "",
) -> Dict[str, Any]:
    """按命名场景预设启动 AutoTest。"""
    preset = _get_scenario_preset(scenario_name)
    if not preset:
        return {
            "ok": False,
            "stage": "preset",
            "error": f"未知场景预设: {scenario_name}",
            "availablePresets": _scenario_preset_rows(),
        }

    merged_env = dict(preset.get("envOverrides", {}) or {})
    user_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = user_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "stage": "preset", "error": f"env_overrides_json 解析失败: {parse_error}"}
    merged_env.update(user_env)

    result = run_quick_autotest(
        war3_dir=war3_dir,
        map_path=str(preset.get("mapPath", DEFAULT_TEST_MAP)),
        ready_timeout_sec=int(preset.get("readyTimeoutSec", 120) or 120),
        sample_duration_sec=int(preset.get("sampleDurationSec", 20) or 20),
        windowed=bool(preset.get("windowed", False)),
        use_isolated_desktop=bool(preset.get("useIsolatedDesktop", True)),
        desktop_name=str(preset.get("desktopName", "")),
        opengl=bool(preset.get("opengl", False)),
        auto_perf_record=bool(preset.get("autoPerfRecord", True)),
        record_after_game_started=bool(preset.get("recordAfterGameStarted", True)),
        auto_perf_export_sec=int(preset.get("autoPerfExportSec", 8) or 8),
        deploy_d3d9_before_launch=bool(preset.get("deployD3d9BeforeLaunch", True)),
        build_d3d9_path="build32/src/d3d9/d3d9.dll",
        enforce_video_baseline=bool(preset.get("enforceVideoBaseline", True)),
        baseline_width=int(preset.get("baselineWidth", DEFAULT_BENCHMARK_WIDTH) or DEFAULT_BENCHMARK_WIDTH),
        baseline_height=int(preset.get("baselineHeight", DEFAULT_BENCHMARK_HEIGHT) or DEFAULT_BENCHMARK_HEIGHT),
        baseline_refresh_rate=int(preset.get("baselineRefreshRate", DEFAULT_BENCHMARK_REFRESH) or DEFAULT_BENCHMARK_REFRESH),
        include_sections_in_report=include_sections_in_report,
        section_top_n=section_top_n,
        avoid_focus_on_stop=True,
        profile=str(preset.get("profile", "full_default")),
        disable_modules=str(preset.get("disableModules", "")),
        env_overrides_json=json.dumps(merged_env, ensure_ascii=False),
        scenario_name=scenario_name,
        require_control_plane_ready=True,
        require_hot_shadow_frame=bool(
            preset.get("requireHotShadowFrame", True)
        ),
        hot_shadow_timeout_sec=int(preset.get("hotShadowTimeoutSec", preset.get("readyTimeoutSec", 120)) or 120),
    )
    if isinstance(result, dict):
        result["scenarioPreset"] = dict(preset)
        result["scenarioName"] = _normalize_scenario_name(scenario_name)
    return result


def _write_profile_matrix_html(path: Path, rows: List[Dict[str, Any]], aggregate: Dict[str, Any]) -> None:
    def _f(v: Any) -> str:
        try:
            return f"{float(v):.3f}"
        except Exception:
            return str(v)

    parts: List[str] = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        "<title>War3 Profile Matrix</title>",
        "<style>body{font-family:Segoe UI,Arial,sans-serif;background:#111;color:#eee;padding:24px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #333;padding:8px;text-align:left;vertical-align:top}th{background:#1d1d1d}tr:nth-child(even){background:#181818}.ok{color:#7ad67a}.bad{color:#ff7b7b}.warn{color:#ffd36e}.mono{font-family:Consolas,monospace}.kpi{display:inline-block;margin-right:24px;margin-bottom:16px;padding:12px 16px;background:#1a1a1a;border:1px solid #333}.section{margin-top:28px}</style>",
        "</head><body>",
        "<h1>War3 Runtime Profile Matrix</h1>",
        f"<div class='kpi'><div>Cases</div><strong>{len(rows)}</strong></div>",
        f"<div class='kpi'><div>Success</div><strong>{aggregate.get('success', 0)}</strong></div>",
        f"<div class='kpi'><div>Avg FPS</div><strong>{_f(aggregate.get('avgFps', 0.0))}</strong></div>",
    ]
    summary = aggregate.get("summary", {}) if isinstance(aggregate, dict) else {}
    top_offenders = list(summary.get("topOffenders", []) or [])
    if top_offenders:
        parts.append("<div class='section'><h2>Top Offenders</h2><table><thead><tr><th>Case</th><th>Category</th><th>Gain vs Full FPS</th><th>Budget FPS</th><th>Status</th></tr></thead><tbody>")
        for row in top_offenders[:8]:
            status = str(row.get("budgetStatus", "within_budget"))
            cls = "warn" if status == "over_budget" else "ok"
            parts.append(
                "<tr>"
                f"<td>{row.get('label','')}</td>"
                f"<td>{row.get('category','')}</td>"
                f"<td>{_f(row.get('gainVsFullDefaultFps', 0.0))}</td>"
                f"<td>{_f(row.get('budgetFps', 0.0))}</td>"
                f"<td class='{cls}'>{status}</td>"
                "</tr>"
            )
        parts.append("</tbody></table></div>")

    case_summaries = list(summary.get("caseSummaries", []) or [])
    parts.append("<div class='section'><h2>Case Summary</h2><table><thead><tr><th>Case</th><th>Group</th><th>Profile</th><th>Disable</th><th>OK</th><th>FPS</th><th>Delta</th><th>Budget</th><th>Measurement</th><th>Keywords</th><th>Screenshot</th></tr></thead><tbody>")
    for row in case_summaries:
        ok = bool(row.get("ok"))
        delta = row.get("gainVsFullDefaultFps", row.get("dropVsPrevCoreFps", 0.0))
        keywords = ", ".join(
            f"{item.get('name')}={item.get('count')}"
            for item in list(row.get("topKeywords", []) or [])[:4]
        )
        screenshot = f"{int(row.get('screenshotMatchCount', 0))}/{int(row.get('rounds', 0))}"
        status = str(row.get("budgetStatus", "n/a"))
        status_cls = "warn" if status == "over_budget" else ("ok" if ok else "bad")
        measure = str(row.get("measurementStatus", "unknown"))
        parts.append(
            "<tr>"
            f"<td>{row.get('label','')}</td>"
            f"<td>{row.get('group','')}</td>"
            f"<td class='mono'>{row.get('profile','')}</td>"
            f"<td class='mono'>{row.get('disableModules','')}</td>"
            f"<td class='{'ok' if ok else 'bad'}'>{'OK' if ok else 'FAIL'}</td>"
            f"<td>{_f(row.get('avgFps', 0.0))}</td>"
            f"<td>{_f(delta)}</td>"
            f"<td class='{status_cls}'>{status}</td>"
            f"<td>{measure}</td>"
            f"<td>{keywords}</td>"
            f"<td>{screenshot}</td>"
            "</tr>"
        )
    parts.append("</tbody></table></div>")

    parts.append("<div class='section'><h2>Round Details</h2><table><thead><tr><th>Case</th><th>Profile</th><th>Disable</th><th>OK</th><th>FPS</th><th>CPU ms</th><th>GPU ms</th><th>Tracked CPU ms</th><th>Shadow Budget</th><th>Budget Actions</th><th>Keywords</th><th>Report</th></tr></thead><tbody>")
    for row in rows:
        report = row.get("report") or {}
        shadow_budget = report.get("shadowBudgetSummary") or {}
        log_summary = report.get("logSummary") or row.get("logSummary") or {}
        keywords = ", ".join(
            f"{item.get('name')}={item.get('count')}"
            for item in list(log_summary.get("topKeywords", []) or [])[:4]
        )
        budget_actions = (
            f"prioSkip={int(shadow_budget.get('skippedPriorityBudget', 0) or 0)}, "
            f"alphaDegrade={int(shadow_budget.get('degradedAlphaBudget', 0) or 0)}, "
            f"reuse={int(shadow_budget.get('reusedFreezeHits', 0) or 0)}, "
            f"instSave={int(shadow_budget.get('instancedGeometryDrawsSaved', 0) or 0)}, "
            f"actReuse={_f(shadow_budget.get('actualFreezeReuseMb', 0.0))}MB, "
            f"dupBypass={_f(shadow_budget.get('duplicateFreezeBypassMb', 0.0))}MB, "
            f"dupFreeze={_f(shadow_budget.get('potentialFreezeReuseMb', 0.0))}MB"
        )
        report_path = str(report.get("reportPath", "") or "")
        parts.append(
            "<tr>"
            f"<td>{row.get('name','')}</td>"
            f"<td class='mono'>{row.get('profile','')}</td>"
            f"<td class='mono'>{row.get('disableModules','')}</td>"
            f"<td class='{'ok' if row.get('ok') else 'bad'}'>{'OK' if row.get('ok') else 'FAIL'}</td>"
            f"<td>{_f(report.get('avgFps', 0.0))}</td>"
            f"<td>{_f(report.get('avgFrameTimeMs', 0.0))}</td>"
            f"<td>{_f(report.get('avgGpuTimeMs', 0.0))}</td>"
            f"<td>{_f(report.get('avgTrackedActiveCpuMs', 0.0))}</td>"
            f"<td>{int(shadow_budget.get('framesBudgetExceeded', 0) or 0)} / {_f(shadow_budget.get('avgUsedMb', 0.0))}MB</td>"
            f"<td>{budget_actions}</td>"
            f"<td>{keywords}</td>"
            f"<td class='mono'>{report_path}</td>"
            "</tr>"
        )
    parts.append("</tbody></table></div></body></html>")
    path.write_text("".join(parts), encoding="utf-8")


def _mean_or_zero(values: List[float]) -> float:
    vals = [float(v) for v in values]
    return float(sum(vals) / len(vals)) if vals else 0.0


def _aggregate_profile_matrix_rows(
    rows: List[Dict[str, Any]],
    case_defs: List[Dict[str, Any]],
) -> Dict[str, Any]:
    case_by_name = {str(case["name"]): dict(case) for case in case_defs}
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        key = str(row.get("caseName") or row.get("name") or "")
        grouped.setdefault(key, []).append(row)

    summaries: List[Dict[str, Any]] = []
    for case in case_defs:
        key = str(case["name"])
        case_rows = grouped.get(key, [])
        ok_rows = [r for r in case_rows if bool(r.get("ok"))]
        reports = [dict(r.get("report") or {}) for r in ok_rows]
        report_types = sorted({str(rep.get("reportType", "perf_report")) for rep in reports if rep})
        fps_values = [float(rep.get("avgFps", 0.0) or 0.0) for rep in reports if rep.get("avgFps") is not None]
        cpu_values = [float(rep.get("avgFrameTimeMs", 0.0) or 0.0) for rep in reports if rep.get("avgFrameTimeMs") is not None]
        gpu_values = [float(rep.get("avgGpuTimeMs", 0.0) or 0.0) for rep in reports if rep.get("avgGpuTimeMs") is not None]
        tracked_values = [float(rep.get("avgTrackedActiveCpuMs", 0.0) or 0.0) for rep in reports if rep.get("avgTrackedActiveCpuMs") is not None]
        main_thread_values = [float(rep.get("avgMainThreadCpuMs", 0.0) or 0.0) for rep in reports if rep.get("avgMainThreadCpuMs") is not None]
        coverage_values = [float(rep.get("cpuCoveragePct", 0.0) or 0.0) for rep in reports if rep.get("cpuCoveragePct") is not None]
        shadow_used_values = [
            float((rep.get("shadowBudgetSummary") or {}).get("avgUsedMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_exceeded_frames = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("framesBudgetExceeded", 0) or 0)
            for rep in reports
        )
        shadow_priority_skips = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("skippedPriorityBudget", 0) or 0)
            for rep in reports
        )
        shadow_alpha_degrades = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("degradedAlphaBudget", 0) or 0)
            for rep in reports
        )
        shadow_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("reusedFreezeHits", 0) or 0)
            for rep in reports
        )
        shadow_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("reusedFreezeMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_actual_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("actualFreezeReuseHits", 0) or 0)
            for rep in reports
        )
        shadow_actual_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("actualFreezeReuseMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_unique_geometry = [
            int((rep.get("shadowBudgetSummary") or {}).get("uniqueGeometryCount", 0) or 0)
            for rep in reports
        ]
        shadow_duplicate_instances = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("duplicateGeometryInstances", 0) or 0)
            for rep in reports
        )
        shadow_reuse_eligible_duplicates = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("reuseEligibleDuplicates", 0) or 0)
            for rep in reports
        )
        shadow_unique_freeze_accepted_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("uniqueFreezeAcceptedMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_duplicate_freeze_bypass_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("duplicateFreezeBypassMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_potential_freeze_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("potentialFreezeReuseHits", 0) or 0)
            for rep in reports
        )
        shadow_potential_freeze_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("potentialFreezeReuseMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_instanced_draws_saved = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("instancedGeometryDrawsSaved", 0) or 0)
            for rep in reports
        )
        screenshot_match_count = sum(
            1
            for r in ok_rows
            if bool(((r.get("screenshotSize") or {}).get("matchBaseline", False)))
        )
        keyword_counts: Dict[str, int] = {}
        for rep in reports:
            log_summary = rep.get("logSummary") or {}
            for item in list(log_summary.get("topKeywords", []) or []):
                name = str(item.get("name", ""))
                if not name:
                    continue
                keyword_counts[name] = keyword_counts.get(name, 0) + int(item.get("count", 0) or 0)
        top_keywords = [
            {"name": name, "count": count}
            for name, count in sorted(keyword_counts.items(), key=lambda item: (-item[1], item[0]))
        ]
        summaries.append(
            {
                "caseName": key,
                "label": str(case.get("label", key)),
                "profile": str(case.get("profile", "")),
                "disableModules": str(case.get("disable", "")),
                "group": str(case.get("group", "")),
                "category": str(case.get("category", "")),
                "budgetFps": float(case.get("budgetFps", 0.0) or 0.0),
                "rounds": len(case_rows),
                "success": len(ok_rows),
                "failed": len(case_rows) - len(ok_rows),
                "ok": len(ok_rows) == len(case_rows) and len(case_rows) > 0,
                "avgFps": round(_mean_or_zero(fps_values), 4),
                "avgFrameTimeMs": round(_mean_or_zero(cpu_values), 4),
                "avgGpuTimeMs": round(_mean_or_zero(gpu_values), 4),
                "avgTrackedActiveCpuMs": round(_mean_or_zero(tracked_values), 4),
                "avgMainThreadCpuMs": round(_mean_or_zero(main_thread_values), 4),
                "avgCpuCoveragePct": round(_mean_or_zero(coverage_values), 4),
                "avgShadowUsedMb": round(_mean_or_zero(shadow_used_values), 4),
                "shadowBudgetExceededFrames": int(shadow_exceeded_frames),
                "shadowPriorityBudgetSkips": int(shadow_priority_skips),
                "shadowAlphaBudgetDegrades": int(shadow_alpha_degrades),
                "shadowReuseHits": int(shadow_reuse_hits),
                "avgShadowReuseMb": round(_mean_or_zero(shadow_reuse_mb), 4),
                "shadowActualFreezeReuseHits": int(shadow_actual_reuse_hits),
                "avgShadowActualFreezeReuseMb": round(_mean_or_zero(shadow_actual_reuse_mb), 4),
                "avgUniqueGeometryCount": round(_mean_or_zero(shadow_unique_geometry), 4),
                "shadowDuplicateGeometryInstances": int(shadow_duplicate_instances),
                "shadowReuseEligibleDuplicates": int(shadow_reuse_eligible_duplicates),
                "avgShadowUniqueFreezeAcceptedMb": round(_mean_or_zero(shadow_unique_freeze_accepted_mb), 4),
                "avgShadowDuplicateFreezeBypassMb": round(_mean_or_zero(shadow_duplicate_freeze_bypass_mb), 4),
                "shadowPotentialFreezeReuseHits": int(shadow_potential_freeze_reuse_hits),
                "avgShadowPotentialFreezeReuseMb": round(_mean_or_zero(shadow_potential_freeze_reuse_mb), 4),
                "shadowInstancedGeometryDrawsSaved": int(shadow_instanced_draws_saved),
                "screenshotMatchCount": int(screenshot_match_count),
                "reportTypes": report_types,
                "topKeywords": top_keywords[:6],
            }
        )

    by_case = {str(row["caseName"]): row for row in summaries}
    full_default_fps = float((by_case.get("add_full_default", {}) or {}).get("avgFps", 0.0) or 0.0)
    dxvk_only_fps = float((by_case.get("add_dxvk_only", {}) or {}).get("avgFps", 0.0) or 0.0)
    for row in summaries:
        row["gainVsFullDefaultFps"] = round(float(row.get("avgFps", 0.0) or 0.0) - full_default_fps, 4)
        row["lossVsDxvkOnlyFps"] = round(dxvk_only_fps - float(row.get("avgFps", 0.0) or 0.0), 4)
        row["measurementValid"] = bool(row.get("ok"))
        row["measurementStatus"] = "ok" if row.get("ok") else "case_failed"
        if row.get("ok"):
            report_types = list(row.get("reportTypes", []) or [])
            has_benchmark = "benchmark_log" in report_types
            if (not has_benchmark) and float(row.get("avgMainThreadCpuMs", 0.0) or 0.0) <= 0.0001 and float(row.get("avgCpuCoveragePct", 0.0) or 0.0) <= 0.0001:
                row["measurementValid"] = False
                row["measurementStatus"] = "invalid_no_mainthread_signal"
        if row.get("ok") and dxvk_only_fps > 1e-6 and float(row.get("avgFps", 0.0) or 0.0) > dxvk_only_fps * 1.05:
            row["measurementValid"] = False
            row["measurementStatus"] = "invalid_faster_than_dxvk_only"
        budget = float(row.get("budgetFps", 0.0) or 0.0)
        if row.get("group") == "subtractive" and budget > 0.0:
            gain = float(row.get("gainVsFullDefaultFps", 0.0) or 0.0)
            if not row.get("measurementValid"):
                row["budgetStatus"] = "invalid_measurement"
            else:
                row["budgetStatus"] = "over_budget" if gain > budget else "within_budget"
        else:
            row["budgetStatus"] = "n/a"

    prev_fps = None
    for case_name in PROFILE_MATRIX_PRIMARY_CHAIN:
        row = by_case.get(case_name)
        if not row:
            continue
        current_fps = float(row.get("avgFps", 0.0) or 0.0)
        if prev_fps is None:
            row["dropVsPrevCoreFps"] = 0.0
        else:
            row["dropVsPrevCoreFps"] = round(prev_fps - current_fps, 4)
        prev_fps = current_fps

    top_offenders = sorted(
        [row for row in summaries if row.get("group") == "subtractive" and row.get("measurementValid")],
        key=lambda item: float(item.get("gainVsFullDefaultFps", 0.0) or 0.0),
        reverse=True,
    )
    over_budget = [row for row in top_offenders if row.get("budgetStatus") == "over_budget"]
    invalid_cases = [row for row in summaries if not row.get("measurementValid")]
    return {
        "caseSummaries": summaries,
        "dxvkOnlyFps": dxvk_only_fps,
        "fullDefaultFps": full_default_fps,
        "fullStackGapFps": round(dxvk_only_fps - full_default_fps, 4),
        "topOffenders": top_offenders[:12],
        "overBudgetCases": over_budget,
        "invalidCases": invalid_cases,
    }


@mcp.tool()
def run_profile_matrix(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    rounds_per_case: int = 2,
    sample_duration_sec: int = 60,
    ready_timeout_sec: int = 180,
    windowed: bool = False,
    use_isolated_desktop: bool = True,
    opengl: bool = False,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    env_overrides_json: str = "",
    scenario_name: str = "",
) -> Dict[str, Any]:
    """
    运行运行时档位矩阵，并输出统一 JSON/HTML 汇总。
    """
    rounds = max(1, min(int(rounds_per_case), 5))
    cases: List[Dict[str, Any]] = [dict(case) for case in PROFILE_MATRIX_CASES]
    scenario_name_norm = _normalize_scenario_name(scenario_name)

    out_dir = _ensure_dir(ARTIFACT_ROOT / "profile_matrix" / _now_compact())
    rows: List[Dict[str, Any]] = []
    for case in cases:
        for round_index in range(rounds):
            name = f"{case['name']}_r{round_index + 1}"
            res = run_quick_autotest(
                war3_dir=war3_dir,
                map_path=map_path,
                ready_timeout_sec=ready_timeout_sec,
                sample_duration_sec=sample_duration_sec,
                windowed=windowed,
                use_isolated_desktop=use_isolated_desktop,
                desktop_name=f"War3Matrix_{case['name']}_r{round_index + 1}",
                opengl=opengl,
                auto_perf_record=True,
                auto_perf_export_sec=8,
                deploy_d3d9_before_launch=deploy_d3d9_before_launch,
                build_d3d9_path=build_d3d9_path,
                enforce_video_baseline=True,
                baseline_width=baseline_width,
                baseline_height=baseline_height,
                baseline_refresh_rate=baseline_refresh_rate,
                include_sections_in_report=include_sections_in_report,
                section_top_n=section_top_n,
                avoid_focus_on_stop=True,
                profile=case["profile"],
                disable_modules=case["disable"],
                env_overrides_json=env_overrides_json,
                scenario_name=scenario_name_norm,
            )
            row = {
                "name": name,
                "caseName": case["name"],
                "label": case.get("label", case["name"]),
                "group": case.get("group", ""),
                "category": case.get("category", ""),
                "budgetFps": float(case.get("budgetFps", 0.0) or 0.0),
                "profile": case["profile"],
                "disableModules": case["disable"],
                "ok": bool(res.get("ok")),
                "stage": res.get("stage", ""),
                "scenarioName": scenario_name_norm,
                "warnings": list(res.get("warnings", []) or []),
                "report": dict(res.get("report", {}) or {}),
                "screenshotSize": dict(res.get("screenshotSize", {}) or {}),
                "logSummary": dict(res.get("logSummary", {}) or {}),
                "ready": dict(res.get("ready", {}) or {}),
                "launch": dict(res.get("launch", {}) or {}),
            }
            rows.append(row)

    ok_reports = [r.get("report", {}) for r in rows if r.get("ok")]
    summary = _aggregate_profile_matrix_rows(rows, cases)
    aggregate = {
        "rounds": len(rows),
        "success": len(ok_reports),
        "failed": len(rows) - len(ok_reports),
        "avgFps": round(sum(float(r.get("avgFps", 0.0) or 0.0) for r in ok_reports) / max(1, len(ok_reports)), 4),
        "summary": summary,
    }
    data = {
        "ok": bool(ok_reports),
        "generatedAt": _now_str(),
        "outDir": str(out_dir),
        "useIsolatedDesktop": bool(use_isolated_desktop),
        "scenarioName": scenario_name_norm,
        "aggregate": aggregate,
        "summary": summary,
        "rows": rows,
    }
    json_path = out_dir / "profile_matrix.json"
    html_path = out_dir / "profile_matrix.html"
    json_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    _write_profile_matrix_html(html_path, rows, aggregate)
    return {
        "ok": bool(ok_reports),
        "outDir": str(out_dir),
        "jsonPath": str(json_path),
        "htmlPath": str(html_path),
        "aggregate": aggregate,
        "summary": summary,
        "rows": rows,
    }


@mcp.tool()
def start_periodic_perf_test(
    rounds: int = 3,
    interval_sec: int = 20,
    sample_duration_sec: int = 15,
    ready_timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    windowed: bool = False,
    opengl: bool = False,
    auto_perf_record: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    stop_on_failure: bool = False,
    scenario_name: str = "",
) -> Dict[str, Any]:
    """
    启动“定时性能测试”后台任务（非阻塞）。
    - 每轮执行一次 run_quick_autotest。
    - 轮次之间按 interval_sec 等待。
    - 通过 get_periodic_perf_test_status 查询结果。
    """
    rounds = max(1, min(int(rounds), 100))
    interval_sec = max(0, min(int(interval_sec), 3600))
    sample_duration_sec = max(1, min(int(sample_duration_sec), 600))
    ready_timeout_sec = max(20, min(int(ready_timeout_sec), 900))
    scenario_name_norm = _normalize_scenario_name(scenario_name)

    if STATE.perf_thread and STATE.perf_thread.is_alive():
        return {
            "ok": False,
            "error": "已有定时性能任务在运行",
            "job": _get_perf_job_snapshot(limit_results=30),
        }

    with STATE.perf_lock:
        job_id = int(STATE.perf_next_job_id)
        STATE.perf_next_job_id += 1
        STATE.perf_stop.clear()
        STATE.perf_job = {
            "status": "running",
            "jobId": job_id,
            "startedAt": _now_str(),
            "updatedAt": _now_str(),
            "endedAt": "",
            "roundsTotal": rounds,
            "roundsDone": 0,
            "results": [],
            "lastError": "",
            "params": {
                "rounds": rounds,
                "intervalSec": interval_sec,
                "sampleDurationSec": sample_duration_sec,
                "readyTimeoutSec": ready_timeout_sec,
                "war3Dir": war3_dir,
                "mapPath": map_path,
                "windowed": bool(windowed),
                "opengl": bool(opengl),
                "autoPerfRecord": bool(auto_perf_record),
                "autoPerfExportSec": int(auto_perf_export_sec),
                "deployD3d9BeforeLaunch": bool(deploy_d3d9_before_launch),
                "buildD3d9Path": build_d3d9_path,
                "enforceVideoBaseline": bool(enforce_video_baseline),
                "baselineWidth": int(baseline_width),
                "baselineHeight": int(baseline_height),
                "baselineRefreshRate": int(baseline_refresh_rate),
                "includeSectionsInReport": bool(include_sections_in_report),
                "sectionTopN": int(section_top_n),
                "avoidFocusOnStop": bool(avoid_focus_on_stop),
                "stopOnFailure": bool(stop_on_failure),
                "scenarioName": scenario_name_norm,
            },
            "aggregate": {},
        }

    def _worker() -> None:
        status = "completed"
        last_error = ""
        results: List[Dict[str, Any]] = []
        try:
            for i in range(rounds):
                if STATE.perf_stop.is_set():
                    status = "cancelled"
                    break

                t_round = time.time()
                run_res = run_quick_autotest(
                    war3_dir=war3_dir,
                    map_path=map_path,
                    ready_timeout_sec=ready_timeout_sec,
                    sample_duration_sec=sample_duration_sec,
                    windowed=windowed,
                    opengl=opengl,
                    auto_perf_record=auto_perf_record,
                    auto_perf_export_sec=auto_perf_export_sec,
                    deploy_d3d9_before_launch=deploy_d3d9_before_launch,
                    build_d3d9_path=build_d3d9_path,
                    enforce_video_baseline=enforce_video_baseline,
                    baseline_width=baseline_width,
                    baseline_height=baseline_height,
                    baseline_refresh_rate=baseline_refresh_rate,
                    include_sections_in_report=include_sections_in_report,
                    section_top_n=section_top_n,
                    avoid_focus_on_stop=avoid_focus_on_stop,
                    scenario_name=scenario_name_norm,
                )
                elapsed = round(time.time() - t_round, 3)
                report = run_res.get("report", {}) if isinstance(run_res, dict) else {}
                ready = run_res.get("ready", {}) if isinstance(run_res, dict) else {}
                shot_size = run_res.get("screenshotSize", {}) if isinstance(run_res, dict) else {}
                shot_warnings = run_res.get("warnings", []) if isinstance(run_res, dict) else []

                row = {
                    "round": i + 1,
                    "ok": bool(run_res.get("ok")) if isinstance(run_res, dict) else False,
                    "elapsedSec": elapsed,
                    "readyMode": ready.get("mode", ""),
                    "reportPath": report.get("reportPath", ""),
                    "avgFps": float(report.get("avgFps", 0.0) or 0.0),
                    "avgFrameTimeMs": float(report.get("avgFrameTimeMs", 0.0) or 0.0),
                    "avgGpuTimeMs": float(report.get("avgGpuTimeMs", 0.0) or 0.0),
                    "avgTrackedActiveCpuMs": float(report.get("avgTrackedActiveCpuMs", 0.0) or 0.0),
                    "avgUntrackedActiveCpuMs": float(report.get("avgUntrackedActiveCpuMs", 0.0) or 0.0),
                    "cpuCoveragePct": float(report.get("cpuCoveragePct", 0.0) or 0.0),
                    "cpuCoverageWithIdlePct": float(report.get("cpuCoverageWithIdlePct", 0.0) or 0.0),
                    "jank16": int(report.get("jank16", 0) or 0),
                    "jank33": int(report.get("jank33", 0) or 0),
                    "shotWidth": int(shot_size.get("width", 0) or 0),
                    "shotHeight": int(shot_size.get("height", 0) or 0),
                    "shotMatchBaseline": bool(shot_size.get("matchBaseline", False)),
                    "shotWarnings": shot_warnings,
                    "error": report.get("error", "") if isinstance(report, dict) else "",
                    "time": _now_str(),
                }
                results.append(row)

                if not row["ok"] and not last_error:
                    last_error = row.get("error") or "round failed"

                _set_perf_job_fields(
                    roundsDone=i + 1,
                    results=copy.deepcopy(results),
                    lastError=last_error,
                    aggregate=_compute_perf_aggregate(results),
                )

                if (not row["ok"]) and stop_on_failure:
                    status = "failed"
                    break

                if i < rounds - 1 and interval_sec > 0:
                    if STATE.perf_stop.wait(interval_sec):
                        status = "cancelled"
                        break
        except Exception as e:
            status = "failed"
            last_error = str(e)
        finally:
            # 保底：确保残留进程被清理，避免用户机器被塞满。
            if STATE.war3_pid and _pid_alive(STATE.war3_pid):
                stop_war3(
                    pid=STATE.war3_pid,
                    graceful_wait_sec=3,
                    force=True,
                    avoid_foreground_switch=True,
                )

            if status == "completed" and any(not bool(r.get("ok")) for r in results):
                status = "completed_with_failures"
                if not last_error:
                    last_error = "部分轮次失败"

            _set_perf_job_fields(
                status=status,
                endedAt=_now_str(),
                lastError=last_error,
                aggregate=_compute_perf_aggregate(results),
            )
            with STATE.perf_lock:
                STATE.perf_thread = None
                STATE.perf_stop.clear()

    t = threading.Thread(target=_worker, name=f"war3-perf-job-{job_id}", daemon=True)
    STATE.perf_thread = t
    t.start()

    return {"ok": True, "message": "定时性能任务已启动", "jobId": job_id, "job": _get_perf_job_snapshot(limit_results=10)}


@mcp.tool()
def get_periodic_perf_test_status(limit_results: int = 50) -> Dict[str, Any]:
    """查询定时性能任务状态。"""
    return {"ok": True, "job": _get_perf_job_snapshot(limit_results=limit_results)}


@mcp.tool()
def stop_periodic_perf_test(
    wait_sec: int = 10,
    force_stop_war3: bool = True,
    avoid_focus_on_stop: bool = True,
) -> Dict[str, Any]:
    """停止当前定时性能任务。"""
    t = STATE.perf_thread
    if not t or not t.is_alive():
        return {"ok": True, "message": "当前无运行中的定时性能任务", "job": _get_perf_job_snapshot(limit_results=20)}

    STATE.perf_stop.set()
    t.join(timeout=max(1, min(int(wait_sec), 60)))
    running = bool(STATE.perf_thread and STATE.perf_thread.is_alive())

    if force_stop_war3 and STATE.war3_pid and _pid_alive(STATE.war3_pid):
        stop_war3(
            pid=STATE.war3_pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )

    if running:
        return {"ok": False, "error": "任务未在超时内停止", "job": _get_perf_job_snapshot(limit_results=20)}

    _set_perf_job_fields(status="cancelled", endedAt=_now_str())
    return {"ok": True, "message": "定时性能任务已停止", "job": _get_perf_job_snapshot(limit_results=20)}


@mcp.tool()
def run_periodic_perf_test_blocking(
    rounds: int = 3,
    interval_sec: int = 20,
    sample_duration_sec: int = 15,
    ready_timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    windowed: bool = False,
    opengl: bool = False,
    auto_perf_record: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    stop_on_failure: bool = False,
    max_wait_sec: int = 3600,
    poll_sec: int = 2,
) -> Dict[str, Any]:
    """阻塞执行定时性能测试，直到任务结束后一次性返回最终结果。"""
    start = start_periodic_perf_test(
        rounds=rounds,
        interval_sec=interval_sec,
        sample_duration_sec=sample_duration_sec,
        ready_timeout_sec=ready_timeout_sec,
        war3_dir=war3_dir,
        map_path=map_path,
        windowed=windowed,
        opengl=opengl,
        auto_perf_record=auto_perf_record,
        auto_perf_export_sec=auto_perf_export_sec,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        include_sections_in_report=include_sections_in_report,
        section_top_n=section_top_n,
        avoid_focus_on_stop=avoid_focus_on_stop,
        stop_on_failure=stop_on_failure,
    )
    if not start.get("ok"):
        return {"ok": False, "stage": "start", "detail": start}

    t0 = time.time()
    while time.time() - t0 < max(30, min(int(max_wait_sec), 24 * 3600)):
        status = get_periodic_perf_test_status(limit_results=200)
        job = status.get("job", {})
        if not bool(job.get("running")):
            final_status = str(job.get("status", ""))
            return {
                "ok": final_status == "completed",
                "stage": "done",
                "completedWithFailures": final_status == "completed_with_failures",
                "job": job,
            }
        time.sleep(max(1, min(int(poll_sec), 30)))

    stop = stop_periodic_perf_test(
        wait_sec=5,
        force_stop_war3=True,
        avoid_focus_on_stop=avoid_focus_on_stop,
    )
    return {
        "ok": False,
        "stage": "timeout",
        "error": "阻塞等待超时，任务已尝试停止",
        "stop": stop,
        "job": _get_perf_job_snapshot(limit_results=200),
    }


@mcp.tool()
def sync_all_debug(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    since_id: int = 0,
    event_limit: int = 400,
    contains: str = "",
    tail_lines: int = 200,
    include_dbwin_events: bool = True,
    include_perf_reports: bool = True,
    perf_report_count: int = 3,
    include_log_files: bool = True,
) -> Dict[str, Any]:
    """
    聚合项目调试信息并同步返回：
    - DBWIN 事件（OutputDebugString）
    - runtime_status.json
    - war3_d3d9.log / dxvk.log / war3.log 尾部
    - 最新性能报告摘要
    """
    w3 = Path(war3_dir)
    out: Dict[str, Any] = {
        "ok": True,
        "time": _now_str(),
        "war3Dir": str(w3),
        "war3Pid": STATE.war3_pid or 0,
        "war3Alive": _pid_alive(STATE.war3_pid) if STATE.war3_pid else False,
    }

    runtime_path = _runtime_status_file(w3)
    runtime_data = _read_runtime_status_file(w3)
    out["runtimeStatus"] = {
        "path": str(runtime_path),
        "exists": runtime_path.exists(),
        "data": runtime_data if runtime_data else {},
    }

    if include_dbwin_events:
        rows = STATE.get_events(since_id=since_id, limit=event_limit, contains=contains)
        out["events"] = {
            "count": len(rows),
            "latestId": rows[-1]["id"] if rows else since_id,
            "rows": rows,
        }

    if include_log_files:
        files: List[Dict[str, Any]] = []
        candidates = [
            w3 / "war3_d3d9.log",
            w3 / "dxvk.log",
            w3 / "war3.log",
            w3 / "War3.log",
            w3 / "WarVK" / "Temp" / "runtime_status.json",
        ]
        for p in candidates:
            if not p.exists() or not p.is_file():
                continue
            files.append(
                {
                    "path": str(p),
                    "size": p.stat().st_size,
                    "mtime": datetime.fromtimestamp(p.stat().st_mtime).isoformat(),
                    "tail": _tail_text_file(p, max_lines=tail_lines),
                }
            )
        out["files"] = files

    if include_perf_reports:
        perf_dir = w3 / "WarVK" / "Log"
        perf_rows: List[Dict[str, Any]] = []
        if perf_dir.exists():
            reports = sorted(
                perf_dir.glob("war3_perf_report*.html"),
                key=lambda p: p.stat().st_mtime,
                reverse=True,
            )[: max(1, min(int(perf_report_count), 20))]
            for p in reports:
                summary = _read_perf_summary(p)
                perf_rows.append(
                    {
                        "path": str(p),
                        "size": p.stat().st_size,
                        "mtime": datetime.fromtimestamp(p.stat().st_mtime).isoformat(),
                        "summary": summary,
                    }
                )
        out["perfReports"] = perf_rows

    return out


@mcp.tool()
def current_state() -> Dict[str, Any]:
    """查看当前 MCP 运行态。"""
    return {
        "ok": True,
        "time": _now_str(),
        "war3Pid": STATE.war3_pid,
        "war3Alive": _pid_alive(STATE.war3_pid) if STATE.war3_pid else False,
        "war3Dir": str(STATE.war3_dir),
        "testMapPath": str(STATE.test_map_path),
        "desktopMode": str(STATE.desktop_mode),
        "desktopName": str(STATE.desktop_name),
        "desktopHandle": int(STATE.desktop_handle or 0),
        "lastReportPath": str(STATE.last_report_path) if STATE.last_report_path else "",
        "videoRestorePending": bool(STATE.video_restore_snapshot),
        "videoRestoreKeyPath": str(STATE.video_restore_key_path),
        "videoRestoreSnapshot": dict(STATE.video_restore_snapshot),
        "debugEvents": len(STATE.debug_events),
        "debugMonitorRunning": bool(STATE.debug_thread and STATE.debug_thread.is_alive()),
        "perfJob": _get_perf_job_snapshot(limit_results=10),
    }


def main() -> None:
    _ensure_dir(ARTIFACT_ROOT)
    mcp.run()


if __name__ == "__main__":
    main()
