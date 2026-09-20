"""One isolated 1440p frame-ID canary, not a rare-fissure hunt or FPS benchmark.

Uses the established retained-native-process owner/restore helpers. Requires exact
candidate/live/player identities; refuses existing editor/game/build processes.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent/'dxvk/AutoTest'))
from run_frame_timeline_diagnostic import (war3,identity,copy_new,zero_processes,
    gpu_events_since,exact_isolated_resolution)
sys.path.insert(0,str(ROOT/'AutoTest'))
from shadow_pose_full_trace_control import _request
from analyze_frame_evidence import load,analyze,link_screenshots

GAME=Path('E:/Work/War3')
PLAYER=Path('E:/Work/Warcraft III/d3d9.dll')
GAME_SHA='E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'
def require(ok,message):
    if not ok:raise RuntimeError(message)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name',required=True)
    p.add_argument('--candidate-sha',required=True);p.add_argument('--candidate-size',type=int,required=True)
    p.add_argument('--live-sha',required=True);p.add_argument('--player-sha',required=True)
    p.add_argument('--map',type=Path,required=True);p.add_argument('--map-sha',required=True)
    p.add_argument('--allow-idle-editor-pid',type=int,default=0,
        help='narrow exception for a verified editor in the protected player tree; never stop it')
    p.add_argument('--apply',action='store_true')
    p.add_argument('--history-pre',type=int,default=4);p.add_argument('--history-post',type=int,default=2)
    p.add_argument('--observe-seconds',type=int,default=3)
    p.add_argument('--actual-draws',action='store_true')
    p.add_argument('--watcher-test',action='store_true')
    p.add_argument('--history-ms',type=int,default=0)
    p.add_argument('--window-shortcut',action='store_true')
    p.add_argument('--camera-motion',action='store_true')
    p.add_argument('--capture-hud',action='store_true')
    p.add_argument('--raw-inputs',action='store_true')
    p.add_argument('--output-root',type=Path)
    a=p.parse_args();require(a.name.replace('_','').isalnum(),'invalid name')
    live=GAME/'d3d9.dll';dll=ROOT/'build32/src/d3d9/d3d9.dll'
    out=ROOT/'AutoTest/artifacts/frame_evidence_runs'/a.name
    require(not out.exists(),'unique evidence directory required')
    editor_before=None
    def check_processes():
        nonlocal editor_before
        if not a.allow_idle_editor_pid:zero_processes();return
        # A known editor may stay open only if it owns no game/D3D9 module and
        # is not the test executable. Shared YDWE helper binaries are read-only.
        script=r'''$ErrorActionPreference='Stop'; $items=@(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^(war3|warcraft iii|worldedit|worldeditydwe|ydwe|ninja|cc1plus|g\+\+|meson)\.exe$'}); @($items | ForEach-Object { [pscustomobject]@{pid=$_.ProcessId;name=$_.Name;path=$_.ExecutablePath;created=$_.CreationDate.ToUniversalTime().ToString('o');modules=@((Get-Process -Id $_.ProcessId).Modules | ForEach-Object {$_.FileName})} }) | ConvertTo-Json -Depth 3 -Compress'''
        result=subprocess.run(['powershell','-NoProfile','-Command',script],capture_output=True,text=True,check=True)
        rows=json.loads(result.stdout or '[]');rows=rows if isinstance(rows,list) else [rows]
        for row in rows:
            require(row['pid']==a.allow_idle_editor_pid and row['name'].lower()=='worldeditydwe.exe' and
                    Path(row['path']).resolve()==PLAYER.parent/'worldeditydwe.exe','unapproved resource process')
            modules=[Path(s).name.lower() for s in row['modules']]
            require(not any(s in modules for s in ('d3d9.dll','game.dll','dxgi.dll')),'editor owns graphics/game module')
            pin={k:row[k] for k in ('pid','path','created')}
            if editor_before is None:editor_before=pin
            else:require(pin==editor_before,'editor PID reused or changed')
    check_processes()
    before,candidate,player,map_id=identity(live),identity(dll),identity(PLAYER),identity(a.map)
    require(before['sha256']==a.live_sha.upper(),'live identity')
    require(player['sha256']==a.player_sha.upper(),'player identity')
    require(candidate['sha256']==a.candidate_sha.upper() and candidate['size']==a.candidate_size,'candidate identity')
    require(map_id['sha256']==a.map_sha.upper(),'map identity')
    require(identity(GAME/'Game.dll')['sha256']==GAME_SHA,'Game identity')
    if not a.apply:
        print(json.dumps({'preflightOnly':True,'candidate':candidate,'live':before,'player':player,'map':map_id},indent=2));return 0
    out.mkdir(parents=True)
    def save(name,value):
        with (out/name).open('x',encoding='utf-8') as f:json.dump(value,f,indent=2,ensure_ascii=False,allow_nan=False,default=str)
    copy_new(live,out/'baseline.dll');copy_new(dll,out/'candidate.dll');copy_new(a.map,out/'map.w3x')
    copy_new(Path(__file__),out/'runner.py')
    for label,path in (('owner-helper.py',ROOT.parent/'dxvk/AutoTest/run_frame_timeline_diagnostic.py'),
                       ('autotest-helper.py',Path(war3.__file__)),('reader.py',ROOT/'AutoTest/analyze_frame_evidence.py')):
        copy_new(path,out/label)
    save('preflight.json',{'candidate':candidate,'live':before,'player':player,'map':map_id,'protectedIdleEditor':editor_before})
    prior=out/'before';prior.mkdir()
    previous=list(GAME.glob('*.log'))+list((GAME/'WarVK/Log').glob('*.log'))
    previous+=[p for p in (GAME/'WarVK/Temp').glob('*.json')]
    for i,path in enumerate(previous):copy_new(path,prior/(str(i)+'-'+path.name))
    old_shots={p.name for p in (GAME/'Screenshots').glob('*')};old_dumps={str(p) for p in GAME.rglob('*.dmp')}
    env={'DXVK_WAR3_FRAME_EVIDENCE':'1','DXVK_WAR3_FRAME_EVIDENCE_CASTERS':'0',
         'DXVK_WAR3_FRAME_EVIDENCE_INPUTS':'1' if a.raw_inputs else '0',
         'DXVK_WAR3_FRAME_EVIDENCE_DRAWS':'1' if a.actual_draws else '0',
         'DXVK_WAR3_NATIVE_MODEL_LIGHTS':'0','DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER':'0',
         'DXVK_WAR3_INTERNAL_TEST_API':'1','DXVK_WAR3_INTERNAL_EXIT_TEST':'1',
         'DXVK_WAR3_PERF_MONITOR':'1','DXVK_WAR3_PERF_LEVEL':'1','DXVK_WAR3_PERF_RECORD_ON_START':'0',
         'DXVK_WAR3_PERF_RECORD_ON_GAME_START':'0','DXVK_WAR3_DATA_COLLECTION_TREE':'0',
         'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE':'1','DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE':'1',
         'DXVK_WAR3_DEBUG_CONSOLE':'0','DXVK_WAR3_RENDER_LOG':'0','DXVK_WAR3_SCENARIO':a.name,
         'DISABLE_VULKAN_OBS_CAPTURE':'1'}
    if a.output_root:
        require(a.output_root.is_absolute() and a.output_root.is_dir(),'absolute existing output root required')
        env['DXVK_WAR3_FRAME_EVIDENCE_OUTPUT']=str(a.output_root.resolve())
    save('env.json',env)
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_','DXVK_RUNTIME_')):os.environ.pop(key)
    start=time.time();gpu_events_since(start)
    receipt={'runCount':0,'captureComplete':False,'rootCauseReady':False,'productAccepted':False,'globalInputUsed':False}
    pid=0;deployed=False;error=None
    try:
        check_processes();require(identity(live)==before and identity(PLAYER)==player and identity(a.map)==map_id,'pre-deploy race')
        require(identity(out/'baseline.dll')['sha256']==before['sha256'],'backup changed')
        require(identity(out/'candidate.dll')['sha256']==candidate['sha256'],'candidate changed')
        deployed=True # even a failed/partial copy must enter guarded restoration
        shutil.copyfile(out/'candidate.dll',live)
        require(identity(live)['sha256']==candidate['sha256'],'deployment mismatch')
        launch=war3.launch_war3_test(war3_dir=str(GAME),map_path=str(out/'map.w3x'),windowed=True,
            use_isolated_desktop=True,desktop_name=a.name,auto_perf_record=False,record_after_game_started=True,
            auto_perf_export_sec=0,deploy_d3d9_before_launch=False,enforce_video_baseline=True,
            baseline_width=2560,baseline_height=1440,profile='full_default',env_overrides_json=json.dumps(env),
            expected_map_sha256=map_id['sha256'])
        save('launch.json',launch);require(launch.get('ok') and launch.get('useIsolatedDesktop') is True,'launch/isolation')
        pid=int(launch['pid']);receipt.update(pid=pid,runCount=1)
        for k,v in env.items():
            require(launch.get('envOverrides',{}).get(k)==v,'env override '+k)
            if k.startswith('DXVK_WAR3_'):require(launch.get('effectiveWar3Environment',{}).get(k)==v,'effective env '+k)
        def witness():
            owner=war3.STATE.retained_native_process
            require(owner and owner.pid==pid and owner.poll() is None,'owned process stopped')
        ready=war3.wait_for_game_ready(timeout_sec=100,pid=pid,allow_fallback=False,auto_continue_loading=False)
        save('ready.json',ready);require(ready.get('ok'),'ready')
        res=exact_isolated_resolution(pid);save('resolution.json',res)
        rect=res.get('info',{}).get('clientRect',{});require(res.get('ok') and (rect.get('width'),rect.get('height'))==(2560,1440),'resolution')
        modules=war3.STATE.retained_native_process.snapshot_modules();save('modules.json',modules)
        require(modules and 'reshade' not in json.dumps(modules).lower(),'module/ReShade check')
        def command(label,payload,plane='frame_evidence'):
            witness();r=_request(pid,plane,payload,12);save(label+'.json',r)
            require(r.get('ok') and r.get('result',{}).get('ok'),'command '+label)
            return r['result']
        armed=command('arm',{'action':'arm','capacity':262144 if a.actual_draws else 32768});session=armed['session']
        history=command('history-arm',{'action':'arm','session':session,'preFrames':a.history_pre,'postFrames':a.history_post,'preMilliseconds':a.history_ms},'frame_history')
        history_session=history['session']
        require(1<=a.observe_seconds<=300,'observation bound')
        if a.camera_motion:
            camera=war3._invoke_internal_test_request(pid,GAME,'camera.snapshot',{},timeout_sec=6)
            save('camera-start.json',camera);require(camera.get('ok'),'camera snapshot')
            base=camera['result']
        for tick in range(a.observe_seconds*2):
            witness()
            if a.camera_motion and tick%4==0:
                step=(tick//4)%4
                motion=war3._invoke_internal_test_request(pid,GAME,'camera.apply',{
                    'targetX':base['targetX']+(80 if step<2 else -80),
                    'targetY':base['targetY']+(60 if step%2==0 else -60),
                    'rotation':base['rotation']+(-12 if step%2 else 12),
                    'angleOfAttack':base['angleOfAttack']+(-4 if step%2 else 4),'duration':1.8},timeout_sec=6)
                save('camera-motion-'+str(tick)+'.json',motion);require(motion.get('ok'),'camera motion')
            time.sleep(.5)
        if a.capture_hud:
            save('hud-armed-capture.json',war3._powershell_capture_window(pid,out/'hud-armed.png'))
        shot=war3._invoke_internal_test_request(pid,GAME,'screenshot.native_async',{'count':1},timeout_sec=6)
        save('screenshot-command.json',shot);require(shot.get('ok'),'screenshot dispatch')
        deadline=time.monotonic()+10;paths=[]
        while time.monotonic()<deadline:
            witness();paths=[p for p in (GAME/'Screenshots').glob('*.tga') if p.name not in old_shots]
            if len(paths)==1 and paths[0].stat().st_size==18+2560*1440*4:break
            time.sleep(.1)
        require(len(paths)==1,'exactly one completed screenshot')
        time.sleep(.3)
        if a.window_shortcut:
            shortcut=war3._invoke_internal_test_request(pid,GAME,'frame_history.test_shortcut',{},timeout_sec=6)
            save('window-shortcut.json',shortcut)
            require(shortcut.get('ok') and shortcut.get('result',{}).get('notice')==1,'installed window shortcut route')
        else:
            hist_trigger=command('history-trigger',{'action':'trigger','session':history_session},'frame_history')
            require(hist_trigger['state'] in (2,3,4,5,6,7,8),'history trigger state')
        # Let the normal Present cadence fill the two post-trigger image copies.
        for _ in range(60):witness();time.sleep(.1)
        history_status=None
        for i in range(30):
            history_status=command('history-status-'+str(i),{'action':'status'},'frame_history')
            if history_status['state'] in (6,7,8):break
            witness();time.sleep(.2)
        require(history_status and history_status['state']==6,'history export not complete')
        history_manifest=command('history-export',{'action':'export_manifest','session':history_session},'frame_history')
        save('history-manifest-command.json',history_manifest)
        require(history_manifest.get('manifest'),'history manifest missing')
        manifest=Path(history_manifest['manifest']);require(manifest.exists(),'history manifest path')
        copy_new(manifest,out/'history-manifest.json')
        history_files=[]
        for item in json.loads(manifest.read_text(encoding='utf-8')).get('frames',[]):
            hp=Path(item['file']);require(hp.exists() and hp.suffix.lower()=='.tga','history image missing')
            dst=out/hp.name;copy_new(hp,dst);history_files.append(dst)
        receipt['historyFrames']=len(history_files);receipt['historyStatus']=history_status
        # History trigger also requests the CPU recorder's bounded post window.
        # A second trigger must remain rejected rather than silently re-arm it.
        frozen=None
        for i in range(20):
            time.sleep(.1);frozen=command('status-'+str(i),{'action':'status'})
            if frozen['state']==3:break
        require(frozen and frozen['state']==3 and frozen['reason']==2,'post-window closure')
        exported=command('export',{'action':'export','session':session})
        path=Path(exported['path']).resolve()
        expected_root=(a.output_root or GAME/'WarVK/Log/FrameEvidence').resolve()
        require(path.parent==expected_root,'export escaped expected root')
        copy_new(path,out/'frame-evidence.json');copy_new(paths[0],out/paths[0].name)
        data=load(out/'frame-evidence.json');analysis=analyze(data)
        if a.raw_inputs:
            from analyze_frame_inputs import analyze_inputs
            provider=exported.get('inputs',{});require(provider.get('ok') and provider.get('manifest'),'missing input export')
            mp,bp=Path(provider['manifest']),Path(provider['binary'])
            require(mp.parent.resolve()==expected_root and bp.parent.resolve()==expected_root,'input output escaped')
            copy_new(mp,out/'inputs.json');copy_new(bp,out/'inputs.bin')
            inputs=analyze_inputs(load(mp),bp.read_bytes(),data);save('inputs-analysis.json',inputs)
            receipt['inputs']={k:v for k,v in inputs.items() if k not in ('draws','spanStatuses')}
            require(inputs['batchCount']>0 and inputs['reconstructedDraws']>0,'no reconstructed draw inputs')
            require(inputs['captureDrops']==0,'input capture dropped invocations')
        require(data['processId']==pid,'export process binding')
        analysis['linkedScreenshots']=link_screenshots(data,[out/paths[0].name]);save('analysis.json',analysis)
        receipt['imageLinkVerified']=len(analysis['linkedScreenshots'])==1
        receipt['producerLosses']=data['producerLosses'];receipt['unmatchedSpanEvents']=analysis['unmatchedSpanEvents']
        command('discard',{'action':'discard','session':session})
        if a.watcher_test:
            command('history-discard',{'action':'discard','session':session},'frame_history')
            time.sleep(.5)
            watcher_script=ROOT/'AutoTest/frame_history_watch.py'
            copy_new(watcher_script,out/'watcher-source.py')
            with (out/'watcher.log').open('x',encoding='utf-8') as stdout,(out/'watcher-error.log').open('x',encoding='utf-8') as stderr:
                child=subprocess.Popen([sys.executable,str(watcher_script),'--pid',str(pid),'--auto-trigger-after','5'],
                    stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
                # Raw-input export/analysis is frozen, bounded offline work, not
                # the recording window. Its larger schema needs a separate
                # watchdog; no extension of gameplay/sample or retry occurs.
                try:code=child.wait(timeout=180 if a.raw_inputs else 60)
                except subprocess.TimeoutExpired:
                    child.terminate();child.wait(timeout=10);raise RuntimeError('watcher canary timeout')
            require(code==0,'watcher canary failed')
            text=(out/'watcher.log').read_text(encoding='utf-8')
            locations=[line.removeprefix('Incident saved: ') for line in text.splitlines() if line.startswith('Incident saved: ')]
            require(len(locations)==1,'watcher output identity')
            incident=Path(locations[0]).resolve();require(incident.is_relative_to(expected_root),'watcher output root')
            watcher_out=out/'watcher-incident';watcher_out.mkdir()
            for path in incident.iterdir():
                if path.is_file():copy_new(path,watcher_out/path.name)
            watched=json.loads((watcher_out/'history-analysis.json').read_text())
            require(watched['historyWindowLinked'] and watched.get('preTriggerSeconds',0)>=1,'watcher linked one-second window')
            receipt['watcherCompleted']=True
            if a.capture_hud:
                prior={p.name for p in (GAME/'Screenshots').glob('*.tga')}
                request=war3._invoke_internal_test_request(pid,GAME,'screenshot.native_async',{'count':1},timeout_sec=6)
                save('hud-complete-capture.json',request);require(request.get('ok'),'HUD framebuffer request')
                deadline=time.monotonic()+10;hud=[]
                while time.monotonic()<deadline:
                    hud=[p for p in (GAME/'Screenshots').glob('*.tga') if p.name not in prior]
                    if len(hud)==1 and hud[0].stat().st_size==18+2560*1440*4:break
                    time.sleep(.1)
                require(len(hud)==1,'HUD framebuffer saved')
                copy_new(hud[0],out/'hud-complete.tga')
    except BaseException as exc:
        error=repr(exc);save('failure.json',{'error':error})
    finally:
        if pid or war3.STATE.war3_pid:
            owned=pid or war3.STATE.war3_pid
            try:save('end-game.json',war3._invoke_internal_test_request(owned,GAME,'game.end_for_exit_test',{},timeout_sec=6));time.sleep(3)
            except BaseException as exc:save('end-error.json',{'error':repr(exc)})
            stop=war3.stop_war3(pid=owned,graceful_wait_sec=10,force=False,avoid_foreground_switch=True)
            save('natural-exit.json',stop)
            if not stop.get('ok') or stop.get('nativeTermination',{}).get('exitCode')!=0:error=error or 'natural exit failed'
            if not stop.get('ok'):stop=war3.stop_war3(pid=owned,graceful_wait_sec=1,force=True,avoid_foreground_switch=True)
            save('stop.json',stop);final=stop.get('finalize',stop.get('stateFinalize',{}))
            if not (stop.get('ok') and final.get('desktop',stop.get('desktop',{})).get('closed') and
                    final.get('videoRestore',stop.get('videoRestore',{})).get('ok') and
                    final.get('terminationProof',stop.get('nativeTermination',{})).get('exact')):error=error or 'settlement failed'
        try:
            check_processes()
            if deployed:
                require(identity(live)['sha256']==candidate['sha256'] and identity(out/'baseline.dll')['sha256']==before['sha256'] and
                        identity(out/'baseline.dll')['size']==before['size'],'restore conflict')
                shutil.copyfile(out/'baseline.dll',live)
            receipt.update(restoreOk=identity(live)==before,playerUntouched=identity(PLAYER)==player,mapUntouched=identity(a.map)==map_id)
            require(all(receipt[k] for k in ('restoreOk','playerUntouched','mapUntouched')),'restoration/untouched')
        except BaseException as exc:error=error or repr(exc)
        if (GAME/'war3_d3d9.log').exists():copy_new(GAME/'war3_d3d9.log',out/'after-war3_d3d9.log')
        receipt['newDumps']=[str(p) for p in GAME.rglob('*.dmp') if str(p) not in old_dumps]
        try:receipt['gpuEvents']=gpu_events_since(start)
        except BaseException as exc:error=error or repr(exc)
        if receipt.get('gpuEvents') or receipt['newDumps']:error=error or 'GPU event/dump'
        receipt.update(ok=error is None,error=error);save('receipt.json',receipt)
        print(json.dumps(receipt,ensure_ascii=False,indent=2))
    return 0 if receipt['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
