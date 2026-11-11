"""Bounded isolated smoke gates for the final v1.22.02 product DLL.

Reuses the RC1 runners (cross-map A->B->A, Hamg Walk probe, life-and-death
pressure route), but every instance is materialized from the dedicated
AutoTest sandbox: the instance pool now rejects the player installation as a
sandbox root. The player's DLL, Game.dll and maps remain pinned read-only
inputs and are re-verified after each gate. The candidate DLL identity is an
explicit argument so a rebuild needs a new invocation.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil


PLAYER = Path("E:/Work/Warcraft III")
SANDBOX = Path("E:/Work/War3_AutoTestSandbox")
PLAYER_DLL_SHA256 = (
    "1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF"
)
WALK_MAP_SHA256 = (
    "692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F"
)
GAME_DLL_SHA256 = (
    "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A"
)
WAR3_EXE_SHA256 = (
    "EA8F5192EFEDE23F84BC60140C2D4A0085EA68B86F430C1D60C354A555922DEF"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def pressure(dll: Path, sha: str, run_id: str) -> int:
    """The RC1 pressure route, rebased onto the AutoTest sandbox."""
    from autotest_sessions import (
        build_instance_layout,
        deploy_content_addressed_map,
        materialize_instance_root,
    )
    import war3_autotest_mcp as war3

    root = Path(__file__).resolve().parents[1]
    source_map = PLAYER / "Maps/(4)生与死v1.28读档bug修复.w3x"
    expected = {
        dll: sha,
        PLAYER / "d3d9.dll": PLAYER_DLL_SHA256,
        PLAYER / "Game.dll": GAME_DLL_SHA256,
        PLAYER / "war3.exe": WAR3_EXE_SHA256,
        SANDBOX / "Game.dll": GAME_DLL_SHA256,
        SANDBOX / "war3.exe": WAR3_EXE_SHA256,
        source_map: "548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F",
    }
    for path, value in expected.items():
        if not path.is_file() or sha256(path) != value:
            raise RuntimeError(f"input identity mismatch: {path}")
    if any(str(row.get("exeName", "")).lower() in
           {"war3.exe", "ninja.exe", "cc1plus.exe", "worldeditydwe.exe"}
           for row in war3._snapshot_process_entries()):
        raise RuntimeError("game/build/editor process conflict")
    artifacts = root / "AutoTest/artifacts"
    layout = build_instance_layout(SANDBOX, artifacts, run_id, "pressure")
    if layout.instance_root.exists() or layout.artifact_dir.exists():
        raise RuntimeError("isolated instance or output already exists")
    preflight = war3.preflight_instance_pool(
        sandbox_root=str(SANDBOX), map_path=str(source_map), instance_count=1,
        run_id=run_id, session_prefix="pressure", artifact_root=str(artifacts))
    if not preflight.get("ok") or preflight.get("warnings"):
        raise RuntimeError(f"instance preflight: {preflight}")
    materialized = materialize_instance_root(layout)
    if not materialized.get("ok"):
        raise RuntimeError(f"instance materialization: {materialized}")
    staged = deploy_content_addressed_map(layout.instance_root, source_map)
    if staged["sha256"].upper() != expected[source_map]:
        raise RuntimeError("staged map differs")
    # 1.27a -loadfile crosses an ANSI boundary: use an ASCII alias copy.
    ascii_map = layout.instance_root / "Maps/AutoTest/V12202/LifeDeath.w3x"
    ascii_map.parent.mkdir(parents=True, exist_ok=False)
    with source_map.open("rb") as original, ascii_map.open("xb") as copied:
        shutil.copyfileobj(original, copied)
    if sha256(ascii_map) != expected[source_map]:
        raise RuntimeError("ASCII map alias differs")
    instance_dll = layout.instance_root / "d3d9.dll"
    before = sha256(instance_dll) if instance_dll.is_file() else None
    for key in tuple(os.environ):
        if key.startswith("DXVK_WAR3_") or key.startswith("DXVK_RUNTIME_"):
            os.environ.pop(key)
    result = war3.run_life_and_death_tdr_scenario(
        war3_dir=str(layout.instance_root), map_path=str(ascii_map),
        duration_sec=180, grid_size=3, ready_timeout_sec=180,
        use_isolated_desktop=True, desktop_name=layout.desktop_name,
        deploy_d3d9_before_launch=True, build_d3d9_path=str(dll),
        screenshot_count=4, birth_hold_sec=25,
        strict_external_graphics_hooks=True,
        acknowledge_difficulty_dialog=True,
    )
    for path, value in expected.items():
        if sha256(path) != value:
            raise RuntimeError(f"source changed during run: {path}")
    after = sha256(instance_dll) if instance_dll.is_file() else None
    print(f"instance_dll_before={before} after={after}", flush=True)
    if before is not None and after != before:
        raise RuntimeError("isolated instance DLL not restored")
    if not result.get("ok") or result.get("newGpuEvents") or result.get("newGpuIncidents"):
        raise RuntimeError(f"pressure route failed: {result.get('failureReason')}")
    print("pressure_route_ok=True", flush=True)
    print(f"result_path={result['resultPath']}", flush=True)
    print(f"observed_seconds={result['durationObservedSec']}", flush=True)
    print(f"samples={len(result.get('runtimeSamples', []))}", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=("crossmap", "walk", "pressure"))
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--tag", required=True,
                        help="unique run suffix, e.g. final-20261111-a")
    args = parser.parse_args()
    dll, sha = args.dll.resolve(), args.sha.upper()
    if not dll.is_file() or sha256(dll) != sha:
        raise RuntimeError(f"candidate identity mismatch: {dll}")
    player_dll = PLAYER / "d3d9.dll"
    if sha256(player_dll) != PLAYER_DLL_SHA256:
        raise RuntimeError("player DLL is not the pinned predecessor")

    if args.gate == "crossmap":
        import run_cross_map_ab_a_canary_20260925 as canary

        # Pins computed at import keep the player paths (read-only checks);
        # only the instance source moves to the sandbox.
        canary.PLAYER = SANDBOX
        canary.EXPECTED.pop(canary.DLL)
        canary.DLL = dll
        canary.EXPECTED[dll] = sha
        canary.RUN_ID = f"v12202-{args.tag}-crossmap"
        canary.SESSION_ID = "v12202map"
        result = canary.main()
    elif args.gate == "walk":
        import run_unit_walk_probe as walk

        walk.PLAYER = SANDBOX
        walk.PINNED.pop(walk.DLL)
        walk.DLL = dll
        walk.PINNED[dll] = sha
        walk.PINNED[walk.MAP] = WALK_MAP_SHA256
        walk.PINNED[player_dll] = PLAYER_DLL_SHA256
        walk.RUN_ID = f"v12202-{args.tag}-walk"
        walk.SESSION_ID = "v12202walk"
        result = walk.main()
    else:
        result = pressure(dll, sha, f"v12202-{args.tag}-pressure")

    if sha256(player_dll) != PLAYER_DLL_SHA256:
        raise RuntimeError("player DLL changed during the gate")
    print(f"player_dll_unchanged=True gate={args.gate} exit={result}", flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
