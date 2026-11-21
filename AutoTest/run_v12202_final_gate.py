"""Bounded isolated smoke gates for the final v1.22.02 product DLL.

Reuses the RC1 runners (cross-map A->B->A, Hamg Walk probe, life-and-death
pressure route), but every instance is materialized from the dedicated
AutoTest sandbox: the instance pool now rejects the player installation as a
sandbox root. The player's Game.dll and maps remain pinned read-only inputs
and are re-verified after each gate. The player's d3d9.dll is absent and must
stay absent: it is checked before and after each gate and never created. The
candidate DLL identity (the build receipt's hash) is an explicit argument so a
rebuild needs a new invocation; the sandbox instance loads exactly that DLL.

--mode proxy (x64_proxy_gate_mode.py) runs the same gates with the product's
device in the x64 render host: --front names the x86 D3D9 front end, --host
the x64 host. --mode inprocess (the default) is the shipped arm.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil

import x64_proxy_gate_mode as x64mode


PLAYER = Path("E:/Work/Warcraft III")
SANDBOX = Path("E:/Work/War3_AutoTestSandbox")
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


def pressure(dll: Path, sha: str, run_id: str, arm=None) -> int:
    """The RC1 pressure route, rebased onto the AutoTest sandbox.

    arm: None for the in-process arm, else the proxy arm (x64_proxy_gate_mode).
    """
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
    proxy_kwargs = {}
    if arm is not None:
        # The data layer's log beside the front end's and the hosts' accounts.
        proxy_kwargs = dict(env_overrides_json=json.dumps(arm.env(arm.staging / "logs")),
                            natural_exit_before_stop=True)
    with (war3.game_stdio_capture(arm.staging) if arm is not None else contextlib.nullcontext()):
        result = war3.run_life_and_death_tdr_scenario(
            war3_dir=str(layout.instance_root), map_path=str(ascii_map),
            duration_sec=180, grid_size=3, ready_timeout_sec=180,
            use_isolated_desktop=True, desktop_name=layout.desktop_name,
            deploy_d3d9_before_launch=True, build_d3d9_path=str(dll),
            screenshot_count=4, birth_hold_sec=25,
            strict_external_graphics_hooks=True,
            acknowledge_difficulty_dialog=True,
            **proxy_kwargs,
        )
    for path, value in expected.items():
        if sha256(path) != value:
            raise RuntimeError(f"source changed during run: {path}")
    after = sha256(instance_dll) if instance_dll.is_file() else None
    print(f"instance_dll_before={before} after={after}", flush=True)
    if before is not None and after != before:
        raise RuntimeError("isolated instance DLL not restored")
    if arm is not None:
        # Gameplay from evidence, not from a posted key: the data layer's
        # world scenes and BeforeUi insertions advanced over the route.
        samples = [row.get("status", {}) for row in result.get("runtimeSamples", [])]
        verdict = pressure_proxy_verdict(result, samples, arm, layout.instance_root)
        (arm.staging / "pressure-proxy.json").write_text(
            json.dumps(verdict, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
        print("pressure_proxy=" + json.dumps(verdict["checks"], ensure_ascii=False), flush=True)
        if not verdict["ok"]:
            raise RuntimeError(f"pressure route (proxy) failed: {result.get('failureReason')}")
    if not result.get("ok") or result.get("newGpuEvents") or result.get("newGpuIncidents"):
        raise RuntimeError(f"pressure route failed: {result.get('failureReason')}")
    print("pressure_route_ok=True", flush=True)
    print(f"result_path={result['resultPath']}", flush=True)
    print(f"observed_seconds={result['durationObservedSec']}", flush=True)
    print(f"samples={len(result.get('runtimeSamples', []))}", flush=True)
    return 0


def pressure_proxy_verdict(result: dict, samples: list, arm, instance: Path) -> dict:
    """Proxy arm of the pressure route: the route's own result plus the
    transport account and world-frame evidence (x64_proxy_gate_mode)."""
    proxy_rows = [x64mode.proxy_status(row) for row in samples]
    world = x64mode.world_progress([row for row in proxy_rows if row])
    evidence = x64mode.collect_evidence(arm.staging, [arm.staging / "logs"], instance)
    natural = result.get("naturalExit") or {}
    acceptance = x64mode.proxy_acceptance(evidence, natural, world)
    lost = [row for row in proxy_rows if row.get("deviceLost")]
    checks = {"routeOk": bool(result.get("ok")),
              "noNewGpuEvents": not result.get("newGpuEvents"),
              "noNewGpuIncidents": not result.get("newGpuIncidents"),
              "proxyStatusSampled": bool([row for row in proxy_rows if row]),
              "noDeviceLostSample": not lost,
              **acceptance["checks"]}
    return {"ok": all(checks.values()), "checks": checks, "world": world,
            "proxyAcceptance": acceptance, "evidence": evidence, "naturalExit": natural,
            "resultPath": result.get("resultPath"),
            "notApplicable": {
                "statusFileFreshness": x64mode.not_applicable(
                    "the route prefers a fresh status file and falls back to the control plane; "
                    "the in-process arm does not assert freshness either"),
                "gpuIncidentFiles": x64mode.not_applicable(
                    "gpu_incident_*.json are written by the in-process renderer; the proxy arm's "
                    "equivalent is the host-lost report and the hosts' exit accounts")}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=("crossmap", "walk", "pressure"))
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--tag", required=True,
                        help="unique run suffix, e.g. final-20261111-a")
    parser.add_argument("--run-id", default="",
                        help="artifact folder name instead of v12202-<tag>-<gate>")
    x64mode.add_mode_arguments(parser)
    args = parser.parse_args()
    dll, sha = args.dll.resolve(), args.sha.upper()
    if not dll.is_file() or sha256(dll) != sha:
        raise RuntimeError(f"candidate identity mismatch: {dll}")
    # The player's DLL is only looked at; it must be the same after the gate.
    player_before = x64mode.player_dll_guard("before the gate")
    run_id = args.run_id or f"v12202-{args.tag}-{args.gate}"
    arm = None
    if args.mode == "proxy":
        artifacts = Path(__file__).resolve().parents[1] / "AutoTest/artifacts"
        if (artifacts / run_id).exists():
            raise RuntimeError("run artifact already exists")
        arm = x64mode.ProxyArm(args.front, args.host, artifacts / run_id / "proxy", args.proxy_env)

    if args.gate == "crossmap":
        import run_cross_map_ab_a_canary_20260925 as canary

        # Pins computed at import keep the player paths (read-only checks);
        # only the instance source moves to the sandbox.
        canary.PLAYER = SANDBOX
        canary.EXPECTED.pop(canary.DLL)
        canary.DLL = dll
        canary.EXPECTED[dll] = sha
        canary.RUN_ID = run_id if args.run_id else f"v12202-{args.tag}-crossmap"
        canary.SESSION_ID = "v12202map"
        canary.PROXY_ARM = arm
        result = canary.main()
    elif args.gate == "walk":
        import run_unit_walk_probe as walk

        walk.PLAYER = SANDBOX
        walk.PINNED.pop(walk.DLL)
        walk.DLL = dll
        walk.PINNED[dll] = sha
        walk.PINNED[walk.MAP] = WALK_MAP_SHA256
        walk.RUN_ID = run_id if args.run_id else f"v12202-{args.tag}-walk"
        walk.SESSION_ID = "v12202walk"
        walk.PROXY_ARM = arm
        result = walk.main()
    else:
        result = pressure(dll, sha, run_id if args.run_id else f"v12202-{args.tag}-pressure", arm)

    if not x64mode.player_dll_unchanged(player_before, x64mode.player_dll_state()):
        raise RuntimeError("player d3d9.dll changed during the gate")
    print(f"player_dll_unchanged=True gate={args.gate} mode={args.mode} exit={result}", flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
