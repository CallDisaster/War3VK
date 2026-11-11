"""One isolated high-pressure route for the frozen v1.22.02 RC1 DLL.

The existing life-and-death runner owns the process, camera route, GPU event
check and exact restoration, but operates only on a new AutoTest instance.
The player's installed DLL and map are pinned read-only inputs.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil

from autotest_sessions import (
    build_instance_layout,
    deploy_content_addressed_map,
    materialize_instance_root,
)
import war3_autotest_mcp as war3


ROOT = Path(__file__).resolve().parents[1]
PLAYER = Path("E:/Work/Warcraft III")
CANDIDATE = Path(
    "D:/WarVK-Builds/v1.22.02-rc1-20260927/"
    "build32-product/src/d3d9/d3d9.dll"
)
MAP = PLAYER / "Maps/(4)生与死v1.28读档bug修复.w3x"
RUN_ID = "v12202-rc1b-pressure-20260928"
SESSION_ID = "pressure"
EXPECTED = {
    CANDIDATE: "07D6AE38F4969580F15C0E2C6B5966A955EA303ABF6201200C5BED8A92EBFC23",
    PLAYER / "d3d9.dll": "1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF",
    PLAYER / "Game.dll": "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A",
    PLAYER / "war3.exe": "EA8F5192EFEDE23F84BC60140C2D4A0085EA68B86F430C1D60C354A555922DEF",
    MAP: "548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def main() -> int:
    for path, expected in EXPECTED.items():
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"input identity mismatch: {path}")
    if any(str(row.get("exeName", "")).lower() in
           {"war3.exe", "ninja.exe", "cc1plus.exe", "worldeditydwe.exe"}
           for row in war3._snapshot_process_entries()):
        raise RuntimeError("game/build/editor process conflict")
    artifacts = ROOT / "AutoTest/artifacts"
    layout = build_instance_layout(PLAYER, artifacts, RUN_ID, SESSION_ID)
    if layout.instance_root.exists() or layout.artifact_dir.exists():
        raise RuntimeError("isolated instance or output already exists")
    preflight = war3.preflight_instance_pool(
        sandbox_root=str(PLAYER), map_path=str(MAP), instance_count=1,
        run_id=RUN_ID, session_prefix=SESSION_ID,
        artifact_root=str(artifacts),
    )
    if not preflight.get("ok") or preflight.get("warnings"):
        raise RuntimeError(f"instance preflight: {preflight}")
    materialized = materialize_instance_root(layout)
    if not materialized.get("ok"):
        raise RuntimeError(f"instance materialization: {materialized}")
    staged_map = deploy_content_addressed_map(layout.instance_root, MAP)
    if staged_map["sha256"].upper() != EXPECTED[MAP]:
        raise RuntimeError("staged map differs")
    # Warcraft III 1.27a's -loadfile path crosses an ANSI boundary. Keep the
    # original map read-only and give this isolated instance an ASCII alias.
    ascii_map = layout.instance_root / "Maps/AutoTest/V12202/LifeDeath.w3x"
    ascii_map.parent.mkdir(parents=True, exist_ok=False)
    with MAP.open("rb") as original, ascii_map.open("xb") as copied:
        shutil.copyfileobj(original, copied)
    if sha256(ascii_map) != EXPECTED[MAP]:
        raise RuntimeError("ASCII map alias differs")
    instance_dll = layout.instance_root / "d3d9.dll"
    if sha256(instance_dll) != EXPECTED[PLAYER / "d3d9.dll"]:
        raise RuntimeError("instance live DLL differs")

    # The host may have a developer recorder enabled globally. It can consume
    # the 32-bit address space before this high-pressure map is even ready.
    for key in tuple(os.environ):
        if key.startswith("DXVK_WAR3_") or key.startswith("DXVK_RUNTIME_"):
            os.environ.pop(key)
    result = war3.run_life_and_death_tdr_scenario(
        war3_dir=str(layout.instance_root), map_path=str(ascii_map),
        duration_sec=180, grid_size=3, ready_timeout_sec=180,
        use_isolated_desktop=True, desktop_name=layout.desktop_name,
        deploy_d3d9_before_launch=True, build_d3d9_path=str(CANDIDATE),
        screenshot_count=4, birth_hold_sec=25,
        strict_external_graphics_hooks=True,
        acknowledge_difficulty_dialog=True,
    )
    for path, expected in EXPECTED.items():
        if sha256(path) != expected:
            raise RuntimeError(f"source changed during run: {path}")
    if sha256(instance_dll) != EXPECTED[PLAYER / "d3d9.dll"]:
        raise RuntimeError("isolated instance DLL not restored")
    if not result.get("ok") or result.get("newGpuEvents") or result.get("newGpuIncidents"):
        raise RuntimeError(f"pressure route failed: {result.get('failureReason')}")
    print("pressure_route_ok=True", flush=True)
    print(f"result_path={result['resultPath']}", flush=True)
    print(f"observed_seconds={result['durationObservedSec']}", flush=True)
    print(f"samples={len(result.get('runtimeSamples', []))}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
