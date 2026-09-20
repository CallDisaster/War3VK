"""One isolated 1440p stock-model fixture; no manual point-light creation.

Preserve every attempt. This first producer gate is NOT lighting acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import time
import traceback
from PIL import Image, ImageStat

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent/'dxvk/AutoTest'))
from run_frame_timeline_diagnostic import (
    war3, identity, copy_new, zero_processes, gpu_events_since,
    exact_isolated_resolution, checked_status)

GAME = Path('E:/Work/War3')
PLAYER = Path('E:/Work/Warcraft III/d3d9.dll')
BASE = 'A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A'
PLAYER_SHA = 'B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993'
GAME_SHA = 'E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'

def require(ok, message):
    if not ok: raise RuntimeError(message)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True)
    ap.add_argument('--consumer', choices=('0','1'), default='0')
    ap.add_argument('--capture-stages', action='store_true')
    ap.add_argument('--require-commit', action='store_true')
    ap.add_argument('--dump-shaders', action='store_true')
    ap.add_argument('--force-fallback', action='store_true')
    ap.add_argument('--unshadowed', action='store_true')
    ap.add_argument('--defaults', action='store_true', help='prove native light and sync candidate defaults without their environment switches')
    ap.add_argument('--point-debug', type=int, choices=range(4))
    ap.add_argument('--debug-distance', action='store_true')
    ap.add_argument('--fixture', required=True)
    ap.add_argument('--candidate', type=Path, help='explicit frozen comparison DLL; defaults to build32')
    ap.add_argument('--player-sha256', default=PLAYER_SHA, help='read-only protected player identity, never deployed here')
    ap.add_argument('--baseline-sha256', default=BASE, help='exact preflight-approved test DLL to back up and restore')
    ap.add_argument('--baseline-size', type=int, default=35946664)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--size', type=int, required=True)
    args = ap.parse_args()
    require(all(x.replace('_','').isalnum() for x in (args.name,args.fixture)), 'names')
    out = ROOT/'AutoTest/artifacts/v122_native_lights_20260914'/args.name
    require(not out.exists(), 'new transaction required')
    fixture_path = ROOT/'AutoTest/artifacts/v122_visual_api_20260914'/args.fixture/'manifest.json'
    fixture = json.loads(fixture_path.read_text())
    map_path = Path(fixture['map']['path'])
    require(identity(map_path) == fixture['map'], 'fixture identity')
    require(identity(Path(fixture['source']['path'])) == fixture['source'], 'author map identity')
    require(identity(Path(fixture['harness']['path'])) == fixture['harness'], 'fixture harness identity')
    if 'importedModel' in fixture:
        require(identity(Path(fixture['importedModel']['path']))==fixture['importedModel'],'user model identity')
    expected = args.sha256.upper()
    live = GAME/'d3d9.dll'; source = args.candidate or ROOT/'build32/src/d3d9/d3d9.dll'
    zero_processes()
    before, candidate, player = identity(live), identity(source), identity(PLAYER)
    baseline_sha = args.baseline_sha256.upper()
    require(before['sha256'] == baseline_sha and before['size'] == args.baseline_size, 'test baseline')
    require(player['sha256'] == args.player_sha256.upper(), 'protected player baseline')
    require(candidate['sha256'] == expected and candidate['size'] == args.size, 'candidate identity')
    require(identity(GAME/'Game.dll')['sha256'] == GAME_SHA, 'Game identity')
    pe_bytes = source.read_bytes(); pe = struct.unpack_from('<I',pe_bytes,60)[0]
    require(pe_bytes[:2]==b'MZ' and pe_bytes[pe:pe+4]==b'PE\0\0' and
        struct.unpack_from('<H',pe_bytes,pe+4)[0]==0x14c and
        struct.unpack_from('<H',pe_bytes,pe+24)[0]==0x10b, 'PE32/i386')
    receipt_paths = [GAME/f"WarVK/Temp/{fixture['receiptPrefix']}_stage_{i}.txt" for i in range(4)]
    require(not any(p.exists() for p in receipt_paths), 'old JASS receipt exists')
    out.mkdir(parents=True)
    def save(name, value):
        with (out/name).open('x',encoding='utf-8') as f:
            json.dump(value,f,indent=2,ensure_ascii=False,allow_nan=False,default=str)
    def mark(message): print(time.strftime('%H:%M:%S'),message,flush=True)
    copy_new(live,out/'baseline.dll'); copy_new(source,out/'candidate.dll')
    copy_new(Path(__file__),out/'runner-source.py'); copy_new(fixture_path,out/'fixture.json')
    preserve = out/'before-runtime'; preserve.mkdir()
    previous = list(GAME.glob('*.log')) + list((GAME/'WarVK/Log').glob('*.log'))
    previous += [GAME/'WarVK/Temp'/n for n in ('runtime_status.json', 'frame_capture_request.json',
        'frame_capture_result.json', 'internal_test_request.json', 'internal_test_result.json')
        if (GAME/'WarVK/Temp'/n).is_file()]
    for i,p in enumerate(previous): copy_new(p,preserve/f'{i}-{p.name}')
    dumps = {str(p) for p in GAME.rglob('*.dmp')}
    start = time.time(); gpu_events_since(start)
    overrides = {
        'DXVK_WAR3_NATIVE_MODEL_LIGHTS':'1', 'DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT':'1',
        'DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER':args.consumer,
        'DXVK_WAR3_NATIVE_MODEL_LIGHT_FORCE_FALLBACK':'1' if args.force_fallback else '0',
        'DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE':'1', 'DXVK_WAR3_INTERNAL_TEST_API':'1',
        'DXVK_WAR3_INTERNAL_EXIT_TEST':'1', 'DXVK_WAR3_PERF_MONITOR':'1',
        'DXVK_WAR3_PERF_LEVEL':'1', 'DXVK_WAR3_PERF_RECORD_ON_START':'0',
        'DXVK_WAR3_PERF_RECORD_ON_GAME_START':'0', 'DXVK_WAR3_DEBUG_CONSOLE':'0',
        'DXVK_WAR3_RENDER_LOG':'0','DXVK_WAR3_DATA_COLLECTION_TREE':'0',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE':'1',
        'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE':'1','DXVK_WAR3_SCENARIO':args.name,
        'DISABLE_VULKAN_OBS_CAPTURE':'1'}
    if args.defaults:
        for key in ('DXVK_WAR3_NATIVE_MODEL_LIGHTS','DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER',
                    'DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT','DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE'):
            overrides.pop(key)
    if args.unshadowed:
        overrides['DXVK_WAR3_NATIVE_MODEL_LIGHT_DIAGNOSTIC_UNSHADOWED'] = '1'
    if args.dump_shaders:
        (out/'shaders').mkdir()
        overrides['DXVK_SHADER_DUMP_PATH'] = str(out/'shaders')
    if args.point_debug is not None:
        overrides['DXVK_WAR3_SHADOW_DEBUG'] = '6'
        overrides['DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT'] = str(args.point_debug)
    if args.debug_distance:
        require(args.point_debug is not None,'distance view requires point-debug')
        overrides['DXVK_WAR3_POINT_SHADOW_DEBUG_DISTANCE']='1'
    for k in list(os.environ):
        if k.startswith(('DXVK_WAR3_','DXVK_RUNTIME_')): os.environ.pop(k)
    save('preflight.json',dict(candidate=candidate,live=before,player=player,map=fixture['map'],env=overrides))
    receipt = dict(runCount=0,globalInputUsed=False,lightingAccepted=False,shadowAccepted=False,productAccepted=False)
    pid=0; deployed=False; error=None
    try:
        zero_processes()
        require(identity(live)==before and identity(out/'baseline.dll')['sha256']==baseline_sha and
            identity(out/'baseline.dll')['size']==args.baseline_size, 'pre-deploy race')
        shutil.copyfile(out/'candidate.dll',live); deployed=True
        require(identity(live)['sha256']==expected, 'deployment SHA')
        mark('one stock-light fixture on isolated desktop 2560x1440')
        launch=war3.launch_war3_test(war3_dir=str(GAME),map_path=str(map_path),windowed=True,
            use_isolated_desktop=True,desktop_name=args.name,auto_perf_record=False,
            deploy_d3d9_before_launch=False,enforce_video_baseline=True,
            baseline_width=2560,baseline_height=1440,profile='full_default',
            env_overrides_json=json.dumps(overrides),expected_map_sha256=fixture['map']['sha256'])
        save('launch.json',launch)
        require(launch.get('ok'), 'launch failed')
        pid=int(launch['pid']); receipt.update(pid=pid,runCount=1)
        require(launch.get('useIsolatedDesktop') is True, 'isolation missing')
        for k,v in overrides.items():
            require(launch.get('envOverrides',{}).get(k)==v, 'requested env '+k)
            if k.startswith('DXVK_WAR3_'):
                require(launch.get('effectiveWar3Environment',{}).get(k)==v, 'effective env '+k)
        ready=war3.wait_for_game_ready(timeout_sec=100,pid=pid,allow_fallback=False,auto_continue_loading=False)
        save('ready.json',ready); require(ready.get('ok'), 'ready failed')
        resolution=exact_isolated_resolution(pid); save('resolution.json',resolution)
        rect=resolution.get('info',{}).get('clientRect',{})
        require(resolution.get('ok') and (rect.get('width'),rect.get('height'))==(2560,1440), 'resolution')
        modules=war3.STATE.retained_native_process.snapshot_modules(); save('modules.json',modules)
        require(modules and not any('reshade' in json.dumps(m).lower() for m in modules), 'ReShade loaded/modules unavailable')
        for i,p in enumerate(receipt_paths):
            deadline=time.monotonic()+60
            while not p.exists() and time.monotonic()<deadline:
                require(war3.STATE.retained_native_process.poll() is None, 'owned process exited')
                time.sleep(.25)
            require(p.exists() and f'V122_NATIVE_STAGE={i}' in p.read_text(errors='replace'), 'JASS stage missing '+str(i))
            if fixture.get('visual'):
                require(';errors=0;' in p.read_text(errors='replace'), 'JASS visual assertions '+str(i))
            copy_new(p,out/p.name)
            time.sleep(2)
            save(f'status-{i}.json',checked_status(pid,''))
            if args.capture_stages:
                shots = GAME/'Screenshots'; old = set(shots.glob('*.tga'))
                capture = war3._invoke_internal_test_request(pid,GAME,'screenshot.native_async',{'count':1},timeout_sec=6)
                save(f'capture-{i}.json',capture)
                deadline = time.monotonic()+10
                while time.monotonic()<deadline and not (set(shots.glob('*.tga'))-old):
                    require(war3.STATE.retained_native_process.poll() is None,'capture process exited')
                    time.sleep(.1)
                fresh = set(shots.glob('*.tga'))-old
                require(len(fresh)==1,'one asynchronous stage screenshot')
                shot = fresh.pop(); require(f'_{pid}_' in shot.name,'native asynchronous owner')
                copy_new(shot,out/shot.name)
                with Image.open(shot) as image:
                    image.load(); require(image.size==(2560,1440),'screenshot dimensions')
                    require(image.convert('RGBA').getchannel('A').getextrema()==(255,255),'screenshot alpha')
                    require(max(ImageStat.Stat(image.convert('RGB')).mean)>5,'black screenshot')
                    image.convert('RGB').save(out/f'stage-{i}.png')
            mark('native model fixture stage '+str(i)+' observed')
        time.sleep(3)
        receipt['jassStagesPassed']=True
    except BaseException as exc:
        error=repr(exc); save('failure.json',dict(error=error,traceback=traceback.format_exc())); mark(error)
    finally:
        if pid or war3.STATE.war3_pid:
            try:
                end=war3._invoke_internal_test_request(pid,GAME,'game.end_for_exit_test',{},timeout_sec=6)
                save('end-game.json',end); time.sleep(3)
            except BaseException as exc: save('end-error.json',dict(error=repr(exc)))
            stop=war3.stop_war3(pid=pid or war3.STATE.war3_pid,graceful_wait_sec=10,force=False,avoid_foreground_switch=True)
            save('natural-exit.json',stop)
            if not stop.get('ok') or stop.get('nativeTermination',{}).get('exitCode')!=0:
                error=error or 'natural zero exit failed'
            if not stop.get('ok'):
                stop=war3.stop_war3(pid=pid or war3.STATE.war3_pid,graceful_wait_sec=1,force=True,avoid_foreground_switch=True)
            save('stop.json',stop)
            final=stop.get('finalize',stop.get('stateFinalize',{}))
            if not (stop.get('ok') and final.get('desktop',stop.get('desktop',{})).get('closed') and
                final.get('videoRestore',stop.get('videoRestore',{})).get('ok') and
                final.get('terminationProof',stop.get('nativeTermination',{})).get('exact')):
                error=error or 'exact settlement failed'
        try:
            zero_processes()
            if deployed:
                require(identity(live)['sha256']==expected and identity(out/'baseline.dll')['sha256']==baseline_sha and
                    identity(out/'baseline.dll')['size']==args.baseline_size,'restore conflict')
                shutil.copyfile(out/'baseline.dll',live)
            receipt['restoreOk']=identity(live)==before
            receipt['playerUntouched']=identity(PLAYER)==player
            receipt['authorMapUntouched']=identity(Path(fixture['source']['path']))==fixture['source']
            require(receipt['restoreOk'] and receipt['playerUntouched'] and receipt['authorMapUntouched'],'restore/untouched failure')
        except BaseException as exc:
            error=error or repr(exc);receipt['restoreError']=repr(exc)
        logs=[]
        for i,p in enumerate(list(GAME.glob('*.log'))+list((GAME/'WarVK/Log').glob('*.log'))):
            dst=out/f'after-{i}-{p.name}'; copy_new(p,dst)
            # Keep other logs for forensics, but never borrow old sessions'
            # commits/census as this run's proof (especially on preflight failure).
            if receipt['runCount']==1 and p==GAME/'war3_d3d9.log' and p.stat().st_mtime>=start:
                logs.append(p.read_text(errors='replace'))
        all_logs='\n'.join(logs)
        rows=re.findall(r'NativeModelLights census frame=(\d+).*?lights=(\d+).*?templates=(\d+) clones=(\d+).*?overflow=(\d+)', all_logs)
        receipt['censusRows']=[list(map(int,row)) for row in rows]
        receipt['producerObserved']=bool(rows) and max(int(r[1]) for r in rows)>0
        receipt['colorLeases']=re.findall(r'NativeModelLights color lease[^\n]+',all_logs)
        receipt['colorFallbacks']=re.findall(r'NativeModelLights color fallback[^\n]+',all_logs)
        receipt['receiverFallbacks']=re.findall(r'NativeModelLights receiver fallback[^\n]+',all_logs)
        receipt['receiverCommits']=re.findall(r'NativeModelLights receiver committed[^\n]+',all_logs)
        if args.require_commit and not receipt['receiverCommits']: error=error or 'no automatic receiver commit'
        if args.force_fallback and (receipt['receiverCommits'] or not receipt['receiverFallbacks']):
            error=error or 'forced fallback not exercised exactly'
        receipt['newDumps']=[str(p) for p in GAME.rglob('*.dmp') if str(p) not in dumps]
        try: receipt['gpuEvents']=gpu_events_since(start)
        except BaseException as exc: error=error or repr(exc)
        if receipt.get('gpuEvents') or receipt['newDumps']: error=error or 'GPU event/dump'
        if not receipt['producerObserved']: error=error or 'producer did not identify native lights'
        receipt['error']=error;receipt['ok']=error is None
        save('receipt.json',receipt)
        mark('游戏资源已释放' if receipt.get('restoreOk') and not receipt.get('restoreError') else 'restoration requires attention')
    return 0 if receipt['ok'] else 1

if __name__=='__main__': raise SystemExit(main())
