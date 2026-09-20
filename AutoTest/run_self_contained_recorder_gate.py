"""Single isolated 2560x1440 recorder canary; no watcher or frame control commands.

Only internal camera/shortcut/exit commands. Preserve the player tree, guard
test DLL deployment/restoration, and analyze after the game has exited.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import time

from run_frame_evidence_gate import (ROOT, GAME, PLAYER, GAME_SHA, war3, identity,
    copy_new, zero_processes, gpu_events_since, exact_isolated_resolution, require)
from analyze_frame_evidence import load, analyze
from analyze_frame_history import analyze_history
from analyze_frame_inputs import analyze_inputs
from analyze_skin_palette_selection import summarize

RECORDER_ACTIVATION_VARS=('DXVK_WAR3_FRAME_EVIDENCE','DXVK_WAR3_FRAME_EVIDENCE_INPUTS',
    'DXVK_WAR3_FRAME_EVIDENCE_DRAWS','DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED')
BUILD_DEFAULT_VARS=RECORDER_ACTIVATION_VARS+('DXVK_WAR3_SKIN_PALETTE_CONTRACT',)

def use_build_defaults(env):
    result=dict(env)
    for key in BUILD_DEFAULT_VARS+('DXVK_WAR3_FRAME_EVIDENCE_OUTPUT',
        'DXVK_WAR3_FRAME_EVIDENCE_CASTERS','DXVK_WAR3_ASYNC_SCREENSHOT','DXVK_WAR3_IMGUI_BEFORE_UI'):
        result.pop(key,None)
    return result

def incident_paths(value, root, pid):
    require(value.get('schema') == 1 and value.get('selfContained') is True, 'incident schema/mode')
    require(value.get('processId') == pid and value.get('rawExportComplete') is True, 'incident owner/export')
    require(value.get('evidenceValidated') is False and value.get('rootCauseReady') is False, 'unsupported acceptance')
    images, cpu = value['history'], value['cpu']
    require(value['session'] == images['session'] == cpu['session'], 'incident session')
    require(value['processNonce'] == cpu['processNonce'], 'incident nonce')
    paths = {k: Path(v).resolve() for k, v in dict(history=images['manifest'], cpu=cpu['path'],
        inputs=cpu['inputs']['manifest'], binary=cpu['inputs']['binary']).items()}
    for path in paths.values():
        require(path.is_relative_to(root.resolve()) and path.is_file(), 'incident path escaped/missing')
    return paths


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('name', 'candidate-sha', 'live-sha', 'player-sha', 'map-sha'):
        p.add_argument('--'+name, required=True)
    p.add_argument('--map', type=Path, required=True)
    p.add_argument('--candidate-size', type=int, required=True)
    p.add_argument('--apply', action='store_true')
    p.add_argument('--build-defaults', action='store_true', help='Do not set recorder activation or output env; require compiled defaults')
    a = p.parse_args()
    require(a.name.replace('_', '').isalnum(), 'name')
    out = ROOT/'AutoTest/artifacts/self_contained_recorder_runs'/a.name
    output = GAME/'WarVK/Log/FrameEvidence' if a.build_defaults else out/'recorder'
    require(not out.exists(), 'unique run directory')
    zero_processes()
    live, candidate_path = GAME/'d3d9.dll', ROOT/'build32/src/d3d9/d3d9.dll'
    before, candidate, player, map_id = (identity(path) for path in (live, candidate_path, PLAYER, a.map))
    require(before['sha256'] == a.live_sha.upper(), 'test live identity')
    require(candidate['sha256'] == a.candidate_sha.upper() and candidate['size'] == a.candidate_size, 'candidate identity')
    require(player['sha256'] == a.player_sha.upper() and map_id['sha256'] == a.map_sha.upper(), 'player/map identity')
    require(identity(GAME/'Game.dll')['sha256'] == GAME_SHA, 'Game.dll identity')
    require(shutil.disk_usage(ROOT).free >= 7*1024**3, 'seven GiB test/evidence output budget')
    if not a.apply:
        print(json.dumps(dict(preflightOnly=True, candidate=candidate, live=before, player=player, map=map_id)));return 0
    out.mkdir(parents=True)
    output.mkdir(parents=True,exist_ok=a.build_defaults)
    def save(name, value):
        with (out/name).open('x', encoding='utf-8') as f:
            json.dump(value, f, indent=2, ensure_ascii=False, allow_nan=False, default=str)
    for src, name in ((live, 'baseline.dll'), (candidate_path, 'candidate.dll'), (a.map, 'map.w3x'),
                      (Path(__file__), 'runner.py'), (Path(war3.__file__), 'owner-helper.py')):
        copy_new(src, out/name)
    save('preflight.json', dict(candidate=candidate, live=before, player=player, map=map_id))
    env = {key: '1' for key in ('DXVK_WAR3_FRAME_EVIDENCE', 'DXVK_WAR3_FRAME_EVIDENCE_INPUTS',
        'DXVK_WAR3_FRAME_EVIDENCE_DRAWS', 'DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED',
        'DXVK_WAR3_SKIN_PALETTE_CONTRACT', 'DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT',
        'DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH', 'DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG',
        'DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME', 'DXVK_WAR3_INTERNAL_TEST_API',
        'DXVK_WAR3_INTERNAL_EXIT_TEST', 'DXVK_WAR3_ASYNC_SCREENSHOT',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE', 'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE',
        'DXVK_WAR3_PERF_MONITOR', 'DXVK_WAR3_PERF_LEVEL')}
    env.update({key: '0' for key in ('DXVK_WAR3_FRAME_EVIDENCE_CASTERS', 'DXVK_WAR3_NATIVE_MODEL_LIGHTS',
        'DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER', 'DXVK_WAR3_DEBUG_CONSOLE', 'DXVK_WAR3_RENDER_LOG',
        'DXVK_WAR3_IMGUI_BEFORE_UI', 'DXVK_WAR3_PERF_RECORD_ON_START', 'DXVK_WAR3_PERF_RECORD_ON_GAME_START',
        'DXVK_WAR3_DATA_COLLECTION_TREE')})
    env.update(DXVK_WAR3_FRAME_EVIDENCE_OUTPUT=str(output), DXVK_WAR3_SCENARIO=a.name, DISABLE_VULKAN_OBS_CAPTURE='1')
    if a.build_defaults:env=use_build_defaults(env)
    save('env.json', env)
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_', 'DXVK_RUNTIME_')):os.environ.pop(key)
    start = time.time();gpu_events_since(start)
    old_dumps = {str(p) for p in GAME.rglob('*.dmp')}
    receipt = dict(runCount=0, productAccepted=False, visualRepairProven=False, globalInputUsed=False,
        watcherProcessesStarted=0, frameControlCommandsSent=0,buildDefaults=a.build_defaults)
    pid = 0;deployed = False;error = None;captured = None
    try:
        zero_processes()
        require(identity(live) == before and identity(PLAYER) == player and identity(a.map) == map_id, 'pre-deploy race')
        require(identity(out/'baseline.dll')['sha256'] == before['sha256'], 'backup identity')
        require(identity(out/'candidate.dll')['sha256'] == candidate['sha256'], 'frozen candidate identity')
        deployed = True;shutil.copyfile(out/'candidate.dll', live)
        require(identity(live)['sha256'] == candidate['sha256'], 'deployment')
        launch = war3.launch_war3_test(war3_dir=str(GAME), map_path=str(out/'map.w3x'), windowed=True,
            use_isolated_desktop=True, desktop_name=a.name, auto_perf_record=False, record_after_game_started=True,
            auto_perf_export_sec=0, deploy_d3d9_before_launch=False, enforce_video_baseline=True,
            baseline_width=2560, baseline_height=1440, profile='full_default', env_overrides_json=json.dumps(env),
            expected_map_sha256=map_id['sha256'])
        save('launch.json', launch)
        require(launch.get('ok') and launch.get('useIsolatedDesktop') is True, 'launch/isolation')
        pid = int(launch['pid']);receipt.update(pid=pid, runCount=1)
        owner = war3.STATE.retained_native_process
        def witness():require(owner and owner.pid == pid and owner.poll() is None, 'owned process stopped')
        for k, v in env.items():
            require(launch.get('envOverrides', {}).get(k) == v, 'env override '+k)
            if k.startswith('DXVK_WAR3_'):require(launch.get('effectiveWar3Environment', {}).get(k) == v, 'effective env '+k)
        if a.build_defaults:
            for key in BUILD_DEFAULT_VARS+('DXVK_WAR3_FRAME_EVIDENCE_OUTPUT',):
                require(key not in launch.get('envOverrides',{}) and key not in launch.get('effectiveWar3Environment',{}),
                        'recorder default masked by env '+key)
        # Size correction precedes map readiness, before autonomous arm binds an extent.
        res = exact_isolated_resolution(pid);save('resolution.json', res)
        rect = res.get('info', {}).get('clientRect', {})
        require(res.get('ok') and (rect.get('width'), rect.get('height')) == (2560,1440), 'actual client dimensions')
        ready = war3.wait_for_game_ready(timeout_sec=100, pid=pid, allow_fallback=False, auto_continue_loading=False)
        save('ready.json', ready);require(ready.get('ok'), 'map readiness')
        modules = owner.snapshot_modules();save('modules.json', modules)
        require(modules and 'reshade' not in json.dumps(modules).lower(), 'module/ReShade')
        def internal(label, command, payload):
            witness();value = war3._invoke_internal_test_request(pid, GAME, command, payload, timeout_sec=6)
            save(label+'.json', value);require(value.get('ok'), 'internal '+command);return value.get('result', {})
        camera = internal('camera-start', 'camera.snapshot', {})
        for step in range(8):
            internal('camera-'+str(step), 'camera.apply', dict(targetX=camera['targetX']+(80 if step%2 else -80),
                targetY=camera['targetY']+(60 if step%2 else -60), rotation=camera['rotation']+(12 if step%2 else -12),
                angleOfAttack=315 if step%2 else 285, duration=1.8))
            time.sleep(2);witness()
        shortcut = internal('window-shortcut', 'frame_history.test_shortcut', {})
        require(shortcut.get('notice') == 1, 'real window callback accepted shortcut')
        # No arm/status/export/ack pipe calls: the DLL must finish unaided.
        deadline = time.monotonic()+90
        while time.monotonic()<deadline:
            witness();files = list(output.glob('history-'+str(pid)+'-*/incident.json'))
            require(len(files)<=1, 'one incident only')
            if files:
                try:captured = load(files[0])
                except (json.JSONDecodeError, OSError):pass  # read while CreateNew writer finishes
                else:break
            time.sleep(.2)
        require(captured is not None, 'autonomous incident export timeout')
        incident_paths(captured, output, pid)
        internal('hud-complete', 'screenshot.native_async', dict(count=1))
        time.sleep(1)
    except BaseException as exc:error = repr(exc);save('failure.json', dict(error=error))
    finally:
        if pid or war3.STATE.war3_pid:
            owned = pid or war3.STATE.war3_pid
            try:save('end-game.json', war3._invoke_internal_test_request(owned,GAME,'game.end_for_exit_test',{},timeout_sec=6));time.sleep(3)
            except BaseException as exc:save('end-error.json', dict(error=repr(exc)))
            stop = war3.stop_war3(pid=owned,graceful_wait_sec=10,force=False,avoid_foreground_switch=True)
            save('natural-exit.json', stop)
            if not stop.get('ok') or stop.get('nativeTermination',{}).get('exitCode') != 0:error = error or 'natural exit failed'
            if not stop.get('ok'):stop = war3.stop_war3(pid=owned,graceful_wait_sec=1,force=True,avoid_foreground_switch=True)
            save('stop.json', stop);final = stop.get('finalize',stop.get('stateFinalize',{}))
            if not (final.get('desktop',stop.get('desktop',{})).get('closed') and
                final.get('videoRestore',stop.get('videoRestore',{})).get('ok') and
                final.get('terminationProof',stop.get('nativeTermination',{})).get('exact')):error = error or 'settlement failed'
        try:
            zero_processes()
            if deployed:
                require(identity(live)['sha256']==candidate['sha256'] and
                    identity(out/'baseline.dll')['sha256']==before['sha256'] and
                    identity(out/'baseline.dll')['size']==before['size'], 'restore conflict')
                shutil.copyfile(out/'baseline.dll',live)
            receipt.update(restoreOk=identity(live)==before,playerUntouched=identity(PLAYER)==player,mapUntouched=identity(a.map)==map_id)
            require(all(receipt[k] for k in ('restoreOk','playerUntouched','mapUntouched')), 'restore/untouched')
        except BaseException as exc:error = error or repr(exc)
        if (GAME/'war3_d3d9.log').exists():copy_new(GAME/'war3_d3d9.log',out/'after-war3_d3d9.log')
        receipt['newDumps']=[str(p) for p in GAME.rglob('*.dmp') if str(p) not in old_dumps]
        try:receipt['gpuEvents']=gpu_events_since(start)
        except BaseException as exc:receipt['gpuEvents']=[];error=error or repr(exc)
        if receipt['gpuEvents'] or receipt['newDumps']:error = error or 'GPU event/dump'
    if error is None:
        try:
            paths = incident_paths(captured,output,pid);cpu=load(paths['cpu']);inputs=load(paths['inputs'])
            config=cpu['effectiveConfiguration']
            require(config==dict(frameEvidence=True,rawInputs=True,
                skinPaletteContract=True,localRecorderOwner=True,
                paletteObjectEvidence=False), 'actual gates')
            history_raw=load(paths['history'])
            require(cpu.get('capacity')==65536 and
                all(history_raw.get(key)==value for key,value in dict(
                    preFrames=96,postFrames=4,preMilliseconds=1000).items()) and
                inputs.get('slots')==224, 'actual high-pressure profile')
            save('cpu-analysis.json',analyze(cpu))
            history=analyze_history(history_raw,cpu);save('history-analysis.json',history)
            numeric=analyze_inputs(inputs,paths['binary'].read_bytes(),cpu);save('inputs-analysis.json',numeric)
            selection=summarize(inputs,cpu);save('skin-selection-analysis.json',selection)
            require(cpu['producerLosses']=='0' and numeric['captureDrops']==0 and numeric['reconstructedDraws']>0,'loss/input coverage')
            receipt.update(historyFrames=len(history['frames']),preTriggerSeconds=history['preTriggerSeconds'],
                reconstructedDraws=numeric['reconstructedDraws'],selectionEvents=selection['decisionEvents'],
                identities={k:identity(v) for k,v in paths.items()})
        except BaseException as exc:error=repr(exc)
    receipt.update(ok=error is None,error=error);save('receipt.json',receipt)
    print(json.dumps(receipt,ensure_ascii=False,indent=2));return 0 if error is None else 1

if __name__=='__main__':raise SystemExit(main())
