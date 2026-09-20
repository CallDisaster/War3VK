"""One identity-pinned, noninteractive Life/Death crash investigation.

No input plan, debugger attachment, map patch, build, or player DLL write.
Natural exit is recorded BEFORE any controlled teardown. Memory sampling uses
the retained launch HANDLE, never a later OpenProcess/PID guess. Diagnostic
completion is not gameplay/difficulty selection or candidate acceptance.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time

from run_frame_evidence_gate import ROOT, GAME, PLAYER, GAME_SHA, war3, identity, copy_new
from run_frame_evidence_gate import exact_isolated_resolution, gpu_events_since, require
from shadow_pose_full_trace_control import _request

CANDIDATE_SHA = '81339DFC85B3C3CD507FBE249E6160160A8D0A920D3CAD01383158B85BF89EFA'
LIVE_SHA = '74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73'
MAP = PLAYER.parent/'Maps/(4)生与死v1.28读档bug修复.w3x'
MAP_SHA = '548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F'


class Counters(C.Structure):
    # Microsoft PROCESS_MEMORY_COUNTERS_EX; SIZE_T is caller-native, not WOW64.
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD)] + [
        (key, C.c_size_t) for key in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class Region(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p),
        ('AllocationProtect', W.DWORD), ('PartitionId', W.WORD),
        ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


K = C.WinDLL('kernel32', use_last_error=True)
P = C.WinDLL('psapi', use_last_error=True)
P.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(Counters), W.DWORD]
P.GetProcessMemoryInfo.restype = W.BOOL
K.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(Region), C.c_size_t]
K.VirtualQueryEx.restype = C.c_size_t
K.CheckRemoteDebuggerPresent.argtypes = [W.HANDLE, C.POINTER(W.BOOL)]
K.CheckRemoteDebuggerPresent.restype = W.BOOL


def memory(handle, scan=False):
    result = {}; counters = Counters(); counters.cb = C.sizeof(counters)
    if P.GetProcessMemoryInfo(W.HANDLE(handle), C.byref(counters), counters.cb):
        result.update({key: int(getattr(counters, key)) for key, _ in counters._fields_ if key != 'cb'})
    else:
        result['memoryError'] = C.get_last_error()
    if scan:
        # Read metadata only: no target bytes, suspension, or page residency scan.
        # Report the explicitly bounded lower 4 GiB, not claimed allocatable VA.
        address = 0; counts = {'free': 0, 'reserved': 0, 'committed': 0}
        largest = 0; count = 0; started = time.monotonic()
        while address < 0x100000000 and count < 100000:
            info = Region()
            if not K.VirtualQueryEx(W.HANDLE(handle), C.c_void_p(address), C.byref(info), C.sizeof(info)):
                result['queryError'] = C.get_last_error(); break
            end = min(0x100000000, int(info.BaseAddress or 0) + int(info.RegionSize))
            if end <= address:
                result['queryError'] = 'non-progressing'; break
            size = end-address
            key = {0x10000: 'free', 0x2000: 'reserved', 0x1000: 'committed'}.get(info.State)
            if key: counts[key] += size
            if key == 'free': largest = max(largest, size)
            address = end; count += 1
        result['lower4GiB'] = dict(counts, largestFreeRegion=largest, scannedEnd=address,
            regionCount=count, scanMilliseconds=(time.monotonic()-started)*1000)
    return result


def env_for(mode, name, output):
    heavy = mode == 'full'
    env = {key: '1' for key in ('DXVK_WAR3_SKIN_PALETTE_CONTRACT',
        'DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT', 'DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH',
        'DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG', 'DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME',
        'DXVK_WAR3_INTERNAL_TEST_API', 'DXVK_WAR3_INTERNAL_EXIT_TEST',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE', 'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE',
        'DXVK_WAR3_ASYNC_SCREENSHOT', 'DXVK_WAR3_PERF_MONITOR', 'DXVK_WAR3_PERF_LEVEL')}
    env.update({key: '0' for key in ('DXVK_WAR3_FRAME_EVIDENCE_CASTERS',
        'DXVK_WAR3_NATIVE_MODEL_LIGHTS', 'DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER',
        'DXVK_WAR3_DEBUG_CONSOLE', 'DXVK_WAR3_RENDER_LOG', 'DXVK_WAR3_IMGUI_BEFORE_UI',
        'DXVK_WAR3_PERF_RECORD_ON_START', 'DXVK_WAR3_PERF_RECORD_ON_GAME_START',
        'DXVK_WAR3_DATA_COLLECTION_TREE')})
    env.update({key: str(int(heavy)) for key in ('DXVK_WAR3_FRAME_EVIDENCE',
        'DXVK_WAR3_FRAME_EVIDENCE_INPUTS', 'DXVK_WAR3_FRAME_EVIDENCE_DRAWS')})
    env.update(DXVK_WAR3_SCENARIO=name, DXVK_WAR3_FRAME_EVIDENCE_OUTPUT=str(output),
        DISABLE_VULKAN_OBS_CAPTURE='1')
    return env


def check_processes(pin=None):
    command = r'''$ErrorActionPreference='Stop'; $items=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^(war3|warcraft iii|worldedit|worldeditydwe|ydwe|ninja|cc1plus|g\+\+|meson)\.exe$'}); @($items | ForEach-Object { [pscustomobject]@{pid=$_.ProcessId;name=$_.Name;path=$_.ExecutablePath;created=$_.CreationDate.ToUniversalTime().ToString('o');modules=@((Get-Process -Id $_.ProcessId).Modules | ForEach-Object {$_.FileName})} }) | ConvertTo-Json -Depth 3 -Compress'''
    proc = subprocess.run(['powershell', '-NoProfile', '-Command', command], capture_output=True, text=True, check=True)
    rows = json.loads(proc.stdout or '[]'); rows = rows if isinstance(rows, list) else [rows]
    observed = []
    for row in rows:
        require(row['pid'] == 38544 and row['name'].lower() == 'worldeditydwe.exe' and
            Path(row['path']).resolve() == PLAYER.parent/'worldeditydwe.exe', 'resource conflict')
        require(not {'game.dll', 'd3d9.dll', 'dxgi.dll'} & {Path(p).name.lower() for p in row['modules']}, 'editor active')
        observed.append({key: row[key] for key in ('pid', 'path', 'created')})
    if pin is not None: require(pin == observed, 'protected editor changed')
    return observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--recorder', choices=['full', 'off'], required=True)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--apply', action='store_true')
    a = parser.parse_args()
    require(a.name.replace('_', '').isalnum() and 15 <= a.seconds <= 600, 'name/duration')
    require(C.sizeof(C.c_void_p) == 8 and C.sizeof(Region) == 48 and C.sizeof(Counters) == 80, '64-bit sampler ABI')
    live = GAME/'d3d9.dll'; dll = ROOT/'build32/src/d3d9/d3d9.dll'
    out = ROOT/'AutoTest/artifacts/skin_palette_crash_runs'/a.name
    require(not out.exists(), 'unique run required')
    editor = check_processes()
    before, player, candidate, map_id = (identity(p) for p in (live, PLAYER, dll, MAP))
    require(before['sha256'] == LIVE_SHA, 'test baseline')
    require(player['sha256'] == CANDIDATE_SHA and player['size'] == 35834384, 'protected player')
    require(candidate['sha256'] == CANDIDATE_SHA and candidate['size'] == 35834384, 'candidate')
    require(map_id['sha256'] == MAP_SHA and map_id['size'] == 62290145, 'map')
    require(identity(GAME/'Game.dll')['sha256'] == GAME_SHA, 'Game.dll')
    env = env_for(a.recorder, a.name, out/'recorder')
    if not a.apply:
        print(json.dumps(dict(preflightOnly=True, candidate=candidate, live=before, player=player,
            map=map_id, protectedEditor=editor, env=env), ensure_ascii=False)); return 0
    out.mkdir(parents=True); (out/'recorder').mkdir()
    def save(name, value):
        with (out/name).open('x', encoding='utf-8') as f:
            json.dump(value, f, ensure_ascii=False, indent=2, allow_nan=False, default=str)
    for src, dest in ((live,'baseline.dll'), (dll,'candidate.dll'), (MAP,'map.w3x'),
        (Path(__file__),'runner.py'), (Path(war3.__file__),'autotest-helper.py')):
        copy_new(src, out/dest)
    save('preflight.json', dict(live=before, candidate=candidate, player=player, map=map_id, editor=editor))
    save('env.json', env)
    old_files = {str(p): (p.stat().st_size, p.stat().st_mtime_ns) for sub in ('WarVK/Crash','Errors','WarVK/Log')
        for p in (GAME/sub).glob('*') if p.is_file()}
    if (GAME/'war3_d3d9.log').exists(): copy_new(GAME/'war3_d3d9.log',out/'before-war3_d3d9.log')
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_', 'DXVK_RUNTIME_')): os.environ.pop(key)
    start = time.time(); gpu_events_since(start)
    receipt = dict(runCount=0, productAccepted=False, globalInputUsed=False,
        difficultySelectedVerified=False, recorderMode=a.recorder, recorderArmed=False,
        resolutionVerified=False, readyVerified=False, naturalExitBeforeCleanup=None)
    owner = None; monitor = None; monitor_owner = None; stop_monitor = threading.Event()
    error = None; deployed = False; pid = 0
    events = (out/'events.jsonl').open('x', encoding='utf-8')
    def event(kind, value):
        events.write(json.dumps(dict(utc=time.time(), elapsed=time.time()-start, kind=kind, value=value),
            ensure_ascii=False, default=str)+'\n'); events.flush()
    try:
        check_processes(editor)
        require(identity(live)==before and identity(PLAYER)==player and identity(MAP)==map_id, 'deployment race')
        require(identity(out/'baseline.dll')['sha256']==LIVE_SHA and identity(out/'candidate.dll')['sha256']==CANDIDATE_SHA,'frozen bytes')
        deployed=True; shutil.copyfile(out/'candidate.dll',live)
        require(identity(live)['sha256']==CANDIDATE_SHA, 'deployment identity')
        launch=war3.launch_war3_test(war3_dir=str(GAME), map_path=str(out/'map.w3x'), windowed=True,
            use_isolated_desktop=True, desktop_name=a.name, auto_perf_record=False, record_after_game_started=True,
            auto_perf_export_sec=0, deploy_d3d9_before_launch=False, enforce_video_baseline=True,
            baseline_width=2560, baseline_height=1440, profile='full_default',
            env_overrides_json=json.dumps(env), expected_map_sha256=MAP_SHA)
        save('launch.json',launch)
        require(launch.get('ok') and launch.get('useIsolatedDesktop') is True, 'launch/isolation')
        pid=int(launch['pid']); owner=war3.STATE.retained_native_process
        receipt.update(pid=pid,runCount=1,isolated=True)
        require(owner and owner.pid==pid and owner.poll() is None, 'retained owner')
        save('owner.json', owner.snapshot())
        for key,value in env.items():
            require(launch.get('envOverrides',{}).get(key)==value, 'override '+key)
            if key.startswith('DXVK_WAR3_'): require(launch.get('effectiveWar3Environment',{}).get(key)==value, 'effective '+key)
        monitor_owner=owner.duplicate()
        debug=W.BOOL()
        require(K.CheckRemoteDebuggerPresent(W.HANDLE(monitor_owner._handle),C.byref(debug)) and not debug.value,'debugger attached/unknown')
        def sample():
            with (out/'process-memory.jsonl').open('x',encoding='utf-8') as f:
                for index in range(2400):
                    try:
                        code=monitor_owner.poll()
                        row=dict(utc=time.time(),elapsed=time.time()-start,exitCode=code,
                            memory=memory(monitor_owner._handle, index%10==0) if code is None else {})
                    except Exception as exc: row=dict(error=repr(exc),utc=time.time());code=None
                    f.write(json.dumps(row)+'\n');f.flush()
                    if code is not None or stop_monitor.wait(.5):break
        monitor=threading.Thread(target=sample,name='owned-memory',daemon=True);monitor.start()
        event('launch',{'pid':pid});print('Launched isolated PID '+str(pid),flush=True)
        # The loading/menu window may be clamped by the Windows maximum track
        # size despite 1440p registry settings. Verify it BEFORE waiting for a
        # map, not only after ready. This is HWND sizing, never activation/input.
        require(war3._wait_for_main_window_hwnd(pid,timeout_sec=8,require_visible=True),'owned window missing')
        initial_res=exact_isolated_resolution(pid);save('resolution-initial.json',initial_res)
        rect=initial_res.get('info',{}).get('clientRect',{})
        require(initial_res.get('ok') and (rect.get('width'),rect.get('height'))==(2560,1440),'initial resolution')
        ready=war3.wait_for_game_ready(timeout_sec=120,pid=pid,allow_fallback=False,auto_continue_loading=False)
        save('ready.json',ready)
        if not ready.get('ok') and owner.poll() is None:
            # Preserve a bounded internal framebuffer of an entry failure;
            # no fallback to desktop screenshots or synthetic keyboard input.
            try:save('entry-failure-image.json',war3._request_internal_frame_capture(
                pid,out/'entry-failure.bmp',GAME,timeout_sec=8))
            except Exception as exc:save('entry-failure-image-error.json',{'error':repr(exc)})
        require(ready.get('ok'),'ready failed');receipt['readyVerified']=True
        res=exact_isolated_resolution(pid);save('resolution.json',res)
        rect=res.get('info',{}).get('clientRect',{});require(res.get('ok') and (rect.get('width'),rect.get('height'))==(2560,1440),'resolution')
        receipt['resolutionVerified']=True
        modules=owner.snapshot_modules();save('modules.json',modules)
        require(modules and 'reshade' not in json.dumps(modules).lower(),'modules/ReShade')
        if a.recorder=='full':
            cpu=_request(pid,'frame_evidence',{'action':'arm','capacity':262144},8)
            save('cpu-arm.json',cpu);require(cpu.get('ok') and cpu.get('result',{}).get('ok'),'CPU arm')
            history=_request(pid,'frame_history',{'action':'arm','session':cpu['result']['session'],
                'preFrames':256,'postFrames':4,'preMilliseconds':1000},8)
            save('history-arm.json',history);require(history.get('ok') and history.get('result',{}).get('ok'),'history arm')
            receipt['recorderArmed']=True
        def internal(command,payload):
            require(owner.poll() is None,'natural process exit')
            reply=war3._invoke_internal_test_request(pid,GAME,command,payload,timeout_sec=4)
            event(command,reply);require(reply.get('ok'),'internal '+command);return reply.get('result',{})
        save('image-start-command.json',war3._request_internal_frame_capture(pid,out/'image-start.bmp',GAME,timeout_sec=10))
        base=internal('camera.snapshot',{});world=internal('camera.world_bounds',{})
        require(all(math.isfinite(float(world[k])) for k in ('minX','minY','maxX','maxY')),'world finite')
        internal('visibility.full_map',{'enabled':True})
        xs=[world['minX']+(world['maxX']-world['minX'])*(.1+.2*i) for i in range(5)]
        ys=[world['minY']+(world['maxY']-world['minY'])*(.1+.2*i) for i in range(5)]
        route=[(x,y) for j,y in enumerate(ys) for x in (xs if j%2==0 else list(reversed(xs)))]
        internal('camera.apply',dict(targetDistance=max(1650,base['targetDistance']),
            angleOfAttack=min(335,max(280,base['angleOfAttack']+24)),farZ=max(5000,base['farZ']),duration=0))
        event('cruise-start',{'difficultySelectedVerified':False,'route':route})
        print('Ready; running camera route, difficulty selection NOT yet proven',flush=True)
        deadline=time.monotonic()+a.seconds;tick=0;next_pan=0;next_status=0
        while time.monotonic()<deadline:
            require(owner.poll() is None,'natural process exit during cruise')
            now=time.monotonic()
            if now>=next_pan:
                x,y=route[tick%len(route)];internal('camera.pan_to',{'targetX':x,'targetY':y,'duration':3.5})
                tick+=1;next_pan=now+4
                if tick%5==0:internal('camera.snapshot',{})
            if now>=next_status:
                reply=war3._control_plane_request(pid=pid,command='get_runtime_status',payload={},timeout_sec=2)
                event('runtime',reply)
                if a.recorder=='full':event('history-peek',_request(pid,'frame_history',{'action':'peek'},2))
                require(reply.get('ok'),'runtime pipe failed')
                status=reply.get('result',{});require(not war3._runtime_status_device_lost(status),'device lost')
                next_status=now+5
            time.sleep(.25)
        receipt['cameraCommands']=tick;receipt['observationWindowCompleted']=True
        save('image-end-command.json',war3._request_internal_frame_capture(pid,out/'image-end.bmp',GAME,timeout_sec=10))
        save('modules-end.json',owner.snapshot_modules())
    except BaseException as exc:
        error=repr(exc);event('failure',error)
    finally:
        if owner:
            receipt['naturalExitBeforeCleanup']=owner.poll()
            event('pre-cleanup-exit',receipt['naturalExitBeforeCleanup'])
        owned=pid or war3.STATE.war3_pid
        if owned:
            if owner and owner.poll() is None:
                try:event('end-game',war3._invoke_internal_test_request(owned,GAME,'game.end_for_exit_test',{},timeout_sec=4));time.sleep(2)
                except BaseException as exc:event('end-error',repr(exc))
            stop=war3.stop_war3(pid=owned,graceful_wait_sec=8,force=False,avoid_foreground_switch=True)
            save('stop-graceful.json',stop)
            if not stop.get('ok'):stop=war3.stop_war3(pid=owned,graceful_wait_sec=1,force=True,avoid_foreground_switch=True)
            save('stop-final.json',stop)
            final=stop.get('finalize',stop.get('stateFinalize',{}))
            receipt['settlementOk']=bool(stop.get('ok') and final.get('desktop',stop.get('desktop',{})).get('closed') and
                final.get('videoRestore',stop.get('videoRestore',{})).get('ok') and
                final.get('terminationProof',stop.get('nativeTermination',{})).get('exact'))
        stop_monitor.set()
        if monitor:monitor.join(10)
        if monitor_owner and not (monitor and monitor.is_alive()):monitor_owner.close()
        try:
            receipt['finalProcesses']=check_processes(editor)
            if deployed:
                require(identity(live)['sha256']==CANDIDATE_SHA and identity(out/'baseline.dll')['sha256']==LIVE_SHA,'restore conflict')
                shutil.copyfile(out/'baseline.dll',live)
            receipt.update(restoreOk=identity(live)==before,playerUntouched=identity(PLAYER)==player,mapUntouched=identity(MAP)==map_id)
        except BaseException as exc:receipt['restoreError']=repr(exc)
        if (GAME/'war3_d3d9.log').exists():copy_new(GAME/'war3_d3d9.log',out/'after-war3_d3d9.log')
        changed=[];(out/'new-files').mkdir()
        for sub in ('WarVK/Crash','Errors','WarVK/Log'):
            for path in (GAME/sub).glob('*'):
                if path.is_file() and old_files.get(str(path))!=(path.stat().st_size,path.stat().st_mtime_ns):
                    dest=out/'new-files'/(sub.replace('/','_')+'-'+path.name)
                    copy_new(path,dest);changed.append(identity(dest))
        receipt['newFiles']=changed
        try:receipt['gpuEvents']=gpu_events_since(start)
        except BaseException as exc:receipt['gpuQueryError']=repr(exc)
        receipt.update(error=error,elapsedSeconds=time.time()-start)
        save('receipt.json',receipt);events.close()
        print(json.dumps(receipt,ensure_ascii=False,indent=2),flush=True)
    return 0 if error is None and receipt.get('settlementOk') and receipt.get('restoreOk') else 1


if __name__=='__main__':raise SystemExit(main())
