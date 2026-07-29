#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""YDWE/ydhost LAN 客户端协议的只读契约与命令计划。

本模块不包含进程创建 API，也不会启动 YDWE、Warcraft III 或 ydhost。它把
YDWE 1.32.13 MemoryHack 分发版与公开源码能证明的行为固化成机器可读契约，
并明确列出生产接线前仍未闭环的部分。

最重要的边界是：``-auto`` 不是 Warcraft III 的原生命令。YDWE 启动器先从
当前用户/机器注册表的全局 ``InstallPath`` 选择 War3 根目录，再创建挂起的
``war3.exe``、注入 ``LuaEngine.dll``。LuaEngine 随后加载 ``yd_loader.dll``，
后者才解释 ``-auto`` 并通过 LAN 数据包观察与窗口按键完成入房。因此不能把
现有的 ``war3.exe -loadfile`` 启动器描述为等价实现。
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from autotest_sessions import normalize_identifier


PROTOCOL_SCHEMA_VERSION = "ydwe-1.32.13-lan-protocol-v1"
CLIENT_PROTOCOL_BLOCKER = "YDWE_PER_INSTANCE_WAR3_ROOT_UNRESOLVED"
WAR3_INSTALL_PATH_REGISTRY = (
    r"HKEY_CURRENT_USER\Software\Blizzard Entertainment\Warcraft III\InstallPath"
)


@dataclass(frozen=True)
class RuntimeIdentity:
    relative_path: str
    size: int
    sha256: str
    role: str


# 这些哈希来自用户放入 SourceMap 的实际分发副本，而不是外部 YDWE 源码构建。
# 源码仓库只用于解释行为；运行时准入必须匹配这里的二进制/脚本身份。
AUDITED_RUNTIME_IDENTITIES: tuple[RuntimeIdentity, ...] = (
    RuntimeIdentity(
        "YDWE.exe",
        167424,
        "10741509c0c3802123aaae86e40fb5629c078d98ea7c1d0709ef17c45263438b",
        "解析 -war3 并进入 YDWEStartup",
    ),
    RuntimeIdentity(
        "bin/YDWEStartup.dll",
        209920,
        "480c76aee43f37f8ecb1d0eba2c207a65b14ab04423768c04f2cf59c773a9fa9",
        "选择 War3 根、创建挂起子进程并注入 LuaEngine",
    ),
    RuntimeIdentity(
        "bin/LuaEngine.dll",
        74240,
        "ebce00aef4429903e7c3c953e121c68988d8d182101e4639f97ae28f464c2583",
        "在 war3 进程内加载 script/war3/main.lua",
    ),
    RuntimeIdentity(
        "bin/EverConfig.cfg",
        620,
        "7cdfe72264efeea58f43f83adcc4e03ba77bf9e591f38971cbae5da0b76e6e9a",
        "决定 window/D3D9/虚拟 MPQ 等启动行为",
    ),
    RuntimeIdentity(
        "plugin/warcraft3/yd_loader.dll",
        30720,
        "8e062f4a379efaa9d9f6c760e798d40a6b9d7e2ec0f98d1b18d37c9a151fc20e",
        "解释 -auto、观察 LAN/Storm 状态并驱动入房",
    ),
    RuntimeIdentity(
        "plugin/warcraft3/config.cfg",
        147,
        "3db7377a6babd1654725e21151d1e816f143b8457e5e92a74db406c58195dfec",
        "控制随 Game.dll 加载的 Warcraft III 插件集合",
    ),
    RuntimeIdentity(
        "script/war3/main.lua",
        4233,
        "1dbc60214faa83ab2ecbac9b38c03177aa3d50732698f8a59ad57776534b9c1f",
        "Game.dll 加载后装入 yd_loader 与其余 War3 插件",
    ),
    RuntimeIdentity(
        "script/ydwe/ydwe_on_test.lua",
        5878,
        "c203241519f58b9fe85e78248be7feaac06083908761b0a79ca414bfbedfc3a8",
        "生成 ydhost 配置并发起 N 个 -war3 -auto 包装器",
    ),
    RuntimeIdentity(
        "plugin/ydhost/ydhost.exe",
        138752,
        "cb1f6e1fb1e3844400381302c2f0488d091fcddab6537523e289d9a638c34dd0",
        "LAN 主机进程",
    ),
)


SOURCE_EVIDENCE: tuple[dict[str, Any], ...] = (
    {
        "claim": "host 模式先启动 ydhost，再启动 N 个 YDWE.exe -war3 -auto 客户端",
        "distribution": "SourceMap/YDWE1.32.13 - MemoryHack/script/ydwe/ydwe_on_test.lua:105",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Component/script/ydwe/ydwe_on_test.lua:114",
    },
    {
        "claim": "YDWEStartup 从全局 Warcraft III InstallPath 读取 War3 根目录",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Core/ydwar3/warcraft3/directory.cpp:8",
    },
    {
        "claim": "YDWE 创建挂起的 war3.exe，注入 LuaEngine.dll 后恢复",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Core/YDWEStartup/LaunchWarcraft3.cpp:165",
    },
    {
        "claim": "LuaEngine 加载 war3/main.lua，后者加载 yd_loader.dll",
        "distribution": "SourceMap/YDWE1.32.13 - MemoryHack/script/war3/main.lua:43",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Core/LuaEngine/DllMain.cpp:33",
    },
    {
        "claim": "yd_loader 仅在命令行含 -auto 时启动 auto_enter",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/DllModule.cpp:64",
    },
    {
        "claim": "auto_enter 以 L、四次 TAB、J 驱动 UI，并以 connect 作为 JOIN 请求证据",
        "source": "E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/auto_enter.cpp:13",
        "networkSource": "E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/game_status.cpp:102",
    },
)


def _sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def audit_runtime_identity(
    ydwe_root: Path,
    identities: Iterable[RuntimeIdentity] = AUDITED_RUNTIME_IDENTITIES,
) -> dict[str, Any]:
    """只读核验一个 YDWE 根；任何缺失、大小或 SHA 漂移都会 fail-closed。"""

    root = Path(ydwe_root).resolve(strict=False)
    files: list[dict[str, Any]] = []
    errors: list[str] = []
    if not root.is_dir():
        return {
            "ok": False,
            "schemaVersion": PROTOCOL_SCHEMA_VERSION,
            "ydweRoot": str(root),
            "files": files,
            "errors": [f"YDWE 根目录不存在: {root}"],
        }

    for identity in identities:
        path = root / Path(identity.relative_path)
        row = {
            "relativePath": identity.relative_path,
            "path": str(path),
            "role": identity.role,
            "expectedSize": int(identity.size),
            "expectedSha256": identity.sha256,
            "exists": path.is_file(),
        }
        if not path.is_file():
            errors.append(f"缺少运行时文件: {identity.relative_path}")
            files.append(row)
            continue
        stat = path.stat()
        actual_sha = _sha256_file(path)
        row.update({"actualSize": int(stat.st_size), "actualSha256": actual_sha})
        if int(stat.st_size) != int(identity.size):
            errors.append(f"文件大小漂移: {identity.relative_path}")
        if actual_sha.casefold() != identity.sha256.casefold():
            errors.append(f"SHA-256 漂移: {identity.relative_path}")
        files.append(row)

    return {
        "ok": not errors,
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "ydweRoot": str(root),
        "files": files,
        "errors": errors,
    }


def describe_protocol() -> dict[str, Any]:
    """返回已证明的协议状态；不把源码关联误写为二进制源码证明。"""

    return {
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "protocolOwner": "YDWE yd_loader.dll",
        "autoIsNativeWar3Argument": False,
        "hostSequence": [
            "生成 map.cfg 与 ydhost.cfg",
            "隐藏启动 ydhost.exe（cwd=其独立运行目录）",
            "等待可验证的 listening/game-created 证据",
            "为每个客户端启动 YDWE.exe -war3 -closew2l -auto（当前分发脚本顺序）",
        ],
        "wrapperToClientSequence": [
            "YDWE.exe/YDWEStartup.dll 从全局注册表 InstallPath 选择 War3 根",
            "创建挂起的 war3.exe",
            "保留 -auto，移除 -war3/-closew2l，并追加 -ydwe <YDWE根>",
            "设置 ydwe-process-name=war3，注入 bin/LuaEngine.dll",
            "恢复 war3.exe；war3/main.lua 加载 yd_loader.dll",
        ],
        "autoEnterStateMachine": {
            "none": "每 300ms 向 War3 窗口发送 L",
            "searching": "发送 UDP_GAME_SEARCH 后进入 WAIT0",
            "advertisement": "收到 UDP_GAME_INFO 后解码主机地图路径并进入 WAIT1",
            "joinAttempt": "发送四次 TAB；地图归档名匹配后发送 J",
            "joinRequest": "connect hook 将状态推进到 JOIN 并显示窗口",
            "inGameReady": "源码枚举了后续状态，但没有在该实现中推进；不能作为 ready 证据",
        },
        "clientCount": (
            "HostTest.Option=0 时为 1；否则由 W3I 玩家数决定（固定势力时只计 TYPE=1 槽）"
        ),
        "hostSelection": (
            "-auto 不携带 run/game 标识；UI 选择依赖广播列表与焦点顺序。生产测试必须保证广播域内"
            "只有一个符合条件的主机，或实现可证明的显式选择协议。"
        ),
        "readyEvidenceRequired": "逐 PID 的 DBWIN/JAPI/game-start breadcrumb；connect 只证明入房请求",
        "sourceEvidence": list(SOURCE_EVIDENCE),
        "sourceProvenance": {
            "repository": "E:/Mycode/Source/Repos/YDWE",
            "observedCommit": "a69c315ebf2886ec54d66200707ab6e38bd3484d",
            "qualification": (
                "分发二进制版本为 1.32.12.181229；源码只用于行为关联。生产准入以分发文件 SHA 为准。"
            ),
        },
    }


def build_client_command_plan(
    ydwe_root: Path,
    expected_war3_root: Path,
    session_id: str,
    *,
    desktop_name: str,
    job_id: str,
) -> dict[str, Any]:
    """构造可审计但不可直接执行的客户端命令计划。

    计划故意保持 ``launchable=False``：原版 YDWE 没有 per-process War3 根参数，
    其 HKCU/HKLM ``InstallPath`` 选择方式与多实例独立根目录契约冲突。
    """

    session = normalize_identifier(session_id, "session")
    if not desktop_name:
        raise ValueError("desktop_name 不能为空")
    if not job_id:
        raise ValueError("job_id 不能为空")

    ydwe = Path(ydwe_root).resolve(strict=False)
    war3 = Path(expected_war3_root).resolve(strict=False)
    identity = audit_runtime_identity(ydwe)
    wrapper_command = [
        str(ydwe / "YDWE.exe"),
        "-war3",
        "-closew2l",
        "-auto",
    ]
    semantic_child_arguments = [
        "-window (由每实例 EverConfig.cfg 决定)",
        "-auto",
        f"-ydwe {ydwe}",
    ]
    blockers = [
        {
            "code": CLIENT_PROTOCOL_BLOCKER,
            "detail": (
                f"原版 YDWE 从 {WAR3_INSTALL_PATH_REGISTRY} 选择 War3；命令行没有已证明的"
                f" per-instance 根参数，无法保证本会话选择 {war3}。"
            ),
        },
        {
            "code": "YDWE_WRAPPER_CHILD_IDENTITY_UNPROVEN",
            "detail": (
                "包装器会很快退出；必须保留进程句柄，并以父 PID、创建时间、镜像 SHA、Job 和"
                " Desktop 共同确认实际 war3 子进程，不能按进程名猜测。"
            ),
        },
        {
            "code": "YDWE_AUTO_READY_SIGNAL_ABSENT",
            "detail": "yd_loader 的 connect hook 只证明 JOIN 请求，不能替代逐客户端 in-game ready。",
        },
        {
            "code": "YDWE_AUTO_HOST_SELECTION_NOT_RUN_SCOPED",
            "detail": "-auto 不携带 run/game id；同广播域多主机时选择并不确定。",
        },
        {
            "code": "YDWE_TRANSITIVE_RUNTIME_CATALOG_INCOMPLETE",
            "detail": (
                "当前身份契约锁定协议关键文件，但尚未封闭 bin/common/war3/plugin/MemHack 的"
                "全部传递依赖；生产启动前必须生成完整 catalog 并在 CreateProcess 前固定快照。"
            ),
        },
    ]
    if not identity["ok"]:
        blockers.insert(
            0,
            {
                "code": "YDWE_RUNTIME_IDENTITY_MISMATCH",
                "detail": "; ".join(identity["errors"]),
            },
        )

    return {
        "ok": False,
        "launchable": False,
        "code": CLIENT_PROTOCOL_BLOCKER,
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "sessionId": session,
        "desktopName": desktop_name,
        "jobId": job_id,
        "ydweRoot": str(ydwe),
        "expectedWar3Root": str(war3),
        "wrapperCommand": wrapper_command,
        "wrapperCwd": str(ydwe),
        "semanticChildImage": str(war3 / "war3.exe"),
        "semanticChildArguments": semantic_child_arguments,
        "mapLaunchMode": "ydhost-lan-no-loadfile",
        "identity": identity,
        "requiredCreationOrder": [
            "CreateProcess(YDWE.exe, CREATE_SUSPENDED, lpDesktop=session desktop)",
            "AssignProcessToJobObject(session job, wrapper process)",
            "ResumeThread(wrapper main thread)",
            "observe and retain the exact war3 child handle before wrapper exit",
            "verify child image/root/hash, creation time, Job membership and Desktop",
        ],
        "requiredRuntimeEvidence": [
            "ydhost listening 与 game-created（含 run artifact 来源）",
            "wrapper PID/创建时间/镜像 SHA/Job/Desktop",
            "war3 child PID/父 PID/创建时间/镜像 SHA/Job/Desktop",
            "逐客户端 joined 证据（不能只用 connect 布尔值）",
            "逐客户端 in-game ready DBWIN/JAPI breadcrumb",
        ],
        "blockers": blockers,
        "realProcessLaunchExecuted": False,
        "countsAsLanAcceptance": False,
    }


def build_lan_command_plan(
    ydwe_roots: Sequence[Path],
    war3_roots: Sequence[Path],
    run_id: str,
    session_prefix: str = "client",
) -> dict[str, Any]:
    """为 2..6 个客户端生成同构计划；只描述，不创建文件或进程。"""

    run = normalize_identifier(run_id, "run")
    prefix = normalize_identifier(session_prefix, "session_prefix")
    if len(ydwe_roots) != len(war3_roots):
        raise ValueError("ydwe_roots 与 war3_roots 数量必须一致")
    if len(ydwe_roots) < 2 or len(ydwe_roots) > 6:
        raise ValueError("LAN 命令计划仅允许 2-6 个客户端")

    normalized_ydwe = [Path(path).resolve(strict=False) for path in ydwe_roots]
    normalized_war3 = [Path(path).resolve(strict=False) for path in war3_roots]
    if len({str(path).casefold() for path in normalized_ydwe}) != len(normalized_ydwe):
        raise ValueError("每个客户端必须拥有独立 YDWE 根")
    if len({str(path).casefold() for path in normalized_war3}) != len(normalized_war3):
        raise ValueError("每个客户端必须拥有独立 War3 根")

    clients = []
    for index, (ydwe, war3) in enumerate(zip(normalized_ydwe, normalized_war3), start=1):
        session = normalize_identifier(f"{prefix}-{index:02d}", "session")
        clients.append(
            build_client_command_plan(
                ydwe,
                war3,
                session,
                desktop_name=f"War3AutoTest_{run}_{session}"[:120],
                job_id=f"War3AutoTestJob_{run}_{session}",
            )
        )

    return {
        "ok": False,
        "launchable": False,
        "code": CLIENT_PROTOCOL_BLOCKER,
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "runId": run,
        "clients": clients,
        "protocol": describe_protocol(),
        "realProcessLaunchExecuted": False,
        "countsAsLanAcceptance": False,
    }


def identity_contract() -> Mapping[str, Any]:
    """返回可序列化的静态身份契约。"""

    return {
        "schemaVersion": PROTOCOL_SCHEMA_VERSION,
        "scope": "minimum-protocol-chain",
        "transitiveRuntimeClosureComplete": False,
        "runtimeFiles": [
            {
                "relativePath": row.relative_path,
                "size": row.size,
                "sha256": row.sha256,
                "role": row.role,
            }
            for row in AUDITED_RUNTIME_IDENTITIES
        ],
        "registrySelector": WAR3_INSTALL_PATH_REGISTRY,
        "productionLaunchAllowed": False,
        "blocker": CLIENT_PROTOCOL_BLOCKER,
    }
