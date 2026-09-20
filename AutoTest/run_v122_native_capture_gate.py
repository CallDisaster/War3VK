"""One isolated, 1440p screenshot correctness transaction; never a FPS baseline.

No keyboard/mouse/global input. Uses the guarded native callback via the
render-owner internal API. Restores exact A570 test DLL, protects B1FCC player DLL. Requires natural exit; force is cleanup only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import time
import traceback
from pathlib import Path
from PIL import Image, ImageStat
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent/'dxvk/AutoTest'))
from run_frame_timeline_diagnostic import (
    MAP, MAP_SHA, GAME_SHA, war3, identity, copy_new,
    zero_processes, gpu_events_since, exact_isolated_resolution, checked_status)

GAME = Path('E:/Work/War3')
PLAYER = Path('E:/Work/Warcraft III/d3d9.dll')
PLAYER_SHA = 'B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993'
BASE = 'A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A'
CANDIDATE = '3D1F55E511CC57E9ABE8B135FF75D0A679C13576F3AE7F522B48AD61FD029576'
SIZE = 34548768
ART = ROOT/'AutoTest/artifacts/v122_native_capture_20260914'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--recording', choices=('on','off'), required=True)
    parser.add_argument('--candidate-sha256', required=True)
    parser.add_argument('--candidate-size', type=int, required=True)
    parser.add_argument('--persistent-sync', choices=('0','1','default'), default='default')
    parser.add_argument('--timeline', choices=('0','1'), default='1')
    parser.add_argument('--measure-seconds', type=int, default=0)
    parser.add_argument('--flicker-bursts', type=int, default=0)
    parser.add_argument('--aa', choices=('0','1','2','3','4','5'))
    parser.add_argument('--image-relocation-audit', choices=('0','1'), default='0')
    args = parser.parse_args()
    if not args.name.replace('_','').isalnum(): raise ValueError('name')
    if args.measure_seconds not in (0,20,30,60): raise ValueError('measurement duration')
    if not 0<=args.flicker_bursts<=24: raise ValueError('bounded capture count')
    expected_sha = args.candidate_sha256.upper()
    if len(expected_sha)!=64 or any(c not in '0123456789ABCDEF' for c in expected_sha): raise ValueError('candidate SHA')
    out = ART/args.name
    if out.exists(): raise ValueError('transaction already exists; no overwrite/retry')
    zero_processes()
    live = GAME/'d3d9.dll'; source = ROOT/'build32/src/d3d9/d3d9.dll'
    before = identity(live); candidate = identity(source)
    if before['sha256'] != BASE or before['size'] != 35946664: raise ValueError('live player identity')
    if candidate['sha256'] != expected_sha or candidate['size'] != args.candidate_size: raise ValueError('candidate identity')
    if identity(MAP)['sha256'] != MAP_SHA or identity(GAME/'Game.dll')['sha256'] != GAME_SHA: raise ValueError('map/Game identity')
    raw = source.read_bytes(); pe = struct.unpack_from('<I',raw,60)[0]
    if raw[:2] != b'MZ' or raw[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H',raw,pe+4)[0] != 0x14c or struct.unpack_from('<H',raw,pe+24)[0] != 0x10b: raise ValueError('PE32/i386')
    if identity(PLAYER)['sha256'] != PLAYER_SHA: raise ValueError('protected player DLL changed')
    out.mkdir(parents=True)
    def save(name, value):
        with (out/name).open('x',encoding='utf-8') as f:
            json.dump(value,f,ensure_ascii=False,indent=2,allow_nan=False,default=str)
    def mark(msg): print(time.strftime('%H:%M:%S'),msg,flush=True)
    copy_new(live,out/'baseline.dll'); copy_new(source,out/'candidate.dll')
    copy_new(Path(__file__),out/'runner-source.py')
    # Preserve overwritten startup logs and prior ephemeral runner files before
    # the existing AutoTest launcher clears/replaces its standard temp paths.
    preserve = out/'before-runtime'; preserve.mkdir()
    previous = list(GAME.glob('*.log')) + list((GAME/'WarVK/Log').glob('*.log'))
    temp = GAME/'WarVK/Temp'
    previous += [temp/n for n in ('runtime_status.json','frame_capture_request.json',
        'frame_capture_result.json','internal_test_request.json','internal_test_result.json') if (temp/n).is_file()]
    for n,p in enumerate(previous): copy_new(p,preserve/(str(n)+'-'+p.name))
    initial_reports = {str(p):p.stat().st_mtime_ns for p in (GAME/'WarVK/Log').glob('*.html')}
    initial_dumps = {str(p) for p in GAME.rglob('*.dmp')}
    screenshots = GAME/'Screenshots'
    initial_shots = {str(p) for p in screenshots.glob('*')}
    start = time.time(); gpu_events_since(start)
    receipt = {'productAccepted':False,'playerKeyAccepted':False,'globalInputUsed':False,
               'recordingRequested':args.recording,'runCount':0,'shots':[]}
    save('preflight.json',{'live':before,'candidate':candidate,'map':identity(MAP),
         'game':identity(GAME/'Game.dll'),'zeroProcesses':[]})
    overrides = {
        'DXVK_WAR3_FRAME_TIMELINE':args.timeline,
        'DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT':args.persistent_sync,
        'DXVK_WAR3_NATIVE_FRAME_SYNC_OBSERVE':'1', 'DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE':'1',
        'DXVK_WAR3_PERF_MONITOR':'1','DXVK_WAR3_PERF_LEVEL':'1',
        'DXVK_WAR3_PERF_HISTORY_FRAMES':'4000','DXVK_WAR3_RUNTIME_BENCHMARK':'0',
        'DXVK_WAR3_PERF_RECORD_ON_START':'0',
        'DXVK_WAR3_PERF_RECORD_ON_GAME_START':'1' if args.recording=='on' else '0',
        'DXVK_WAR3_PERF_AUTO_EXPORT_SEC':'20' if args.recording=='on' else '0',
        'DXVK_WAR3_DATA_COLLECTION_TREE':'0',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE':'1',
        'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE':'1','DXVK_WAR3_INTERNAL_TEST_API':'1',
        'DXVK_WAR3_SCENARIO':args.name,'DISABLE_VULKAN_OBS_CAPTURE':'1'}
    if args.aa is not None: overrides['DXVK_WAR3_AA']=args.aa
    overrides['DXVK_WAR3_IMAGE_RELOCATION_AUDIT']=args.image_relocation_audit
    overrides['DXVK_WAR3_RENDER_LOG'] = '0'
    overrides['DXVK_WAR3_DEBUG_CONSOLE'] = '0'
    overrides['DXVK_WAR3_INTERNAL_EXIT_TEST'] = '1'
    # Test app-local ReShade removal without disabling its implicit layer here.
    if args.persistent_sync == 'default':
        overrides.pop('DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT')
        overrides.pop('DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE')
    save('env.json',overrides)
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_','DXVK_RUNTIME_')): os.environ.pop(key)
    for key in ('DISABLE_VK_LAYER_reshade_1', 'RESHADE_DISABLE_LOADING_CHECK', 'RESHADE_BASE_PATH_OVERRIDE'):
        os.environ.pop(key, None)
    pid=0; deployed=False; error=None
    def witness():
        process = war3.STATE.retained_native_process
        if not process or process.pid != pid or process.poll() is not None: raise RuntimeError('owned process exited')
    def shots_after(old, count, label):
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            witness()
            paths = [p for p in screenshots.glob('*.tga') if str(p) not in old]
            if len(paths)>=count: break
            time.sleep(.1)
        if len(paths)!=count: raise RuntimeError(f'{label}: expected {count} files, got {len(paths)}')
        hashes=[]
        ordinals=[]
        for p in paths:
            if f'_{pid}_' not in p.name: raise RuntimeError('unexpected native synchronous output')
            with Image.open(p) as im:
                im.load()
                if im.size!=(2560,1440): raise RuntimeError('TGA dimensions')
                rgba=im.convert('RGBA')
                if rgba.getchannel('A').getextrema()!=(255,255): raise RuntimeError('opaque alpha')
                rgb=im.convert('RGB'); mean=ImageStat.Stat(rgb).mean
                if max(mean)<5: raise RuntimeError('black screenshot')
                if label=='single': rgb.resize((1280,720)).save(out/'single-preview.png')
            dst=out/p.name; copy_new(p,dst); info=identity(dst); hashes.append(info['sha256'])
            receipt['shots'].append({'label':label,'source':str(p),**info,'meanRgb':mean})
            if label.startswith('consecutive-'):
                match=re.search(r'_p(\d+)\.tga$',p.name)
                if not match: raise RuntimeError('missing Present ordinal')
                ordinals.append(int(match[1]))
        if label=='burst' and len(set(hashes))!=1: raise RuntimeError('same-frame burst pixels differ')
        if ordinals and sorted(ordinals)!=list(range(min(ordinals),min(ordinals)+3)):
            raise RuntimeError('consecutive burst has a gap; no silent replacement')
    try:
        zero_processes()
        if identity(live)!=before or identity(out/'baseline.dll')['sha256']!=BASE: raise RuntimeError('pre-deploy race')
        shutil.copyfile(out/'candidate.dll',live); deployed=True
        if identity(live)['sha256']!=expected_sha: raise RuntimeError('deployment identity')
        mark('launch one isolated 2560x1440 process, no global input, recording='+args.recording)
        launch=war3.launch_war3_test(war3_dir=str(GAME),map_path=str(MAP),windowed=True,
            use_isolated_desktop=True,desktop_name=args.name,auto_perf_record=args.recording=='on',
            record_after_game_started=True,auto_perf_export_sec=20 if args.recording=='on' else 0,
            deploy_d3d9_before_launch=False,enforce_video_baseline=True,
            baseline_width=2560,baseline_height=1440,profile='full_default',
            env_overrides_json=json.dumps(overrides),expected_map_sha256=MAP_SHA)
        save('launch.json',launch)
        if not launch.get('ok'): raise RuntimeError('launch failed')
        pid=int(launch['pid']);receipt.update(pid=pid,runCount=1)
        if launch.get('useIsolatedDesktop') is not True: raise RuntimeError('isolation missing')
        for key,value in overrides.items():
            if launch.get('envOverrides',{}).get(key)!=value: raise RuntimeError('envOverrides mismatch '+key)
            if key.startswith('DXVK_WAR3_') and launch.get('effectiveWar3Environment',{}).get(key)!=value: raise RuntimeError('effective env mismatch '+key)
        ready=war3.wait_for_game_ready(timeout_sec=100,pid=pid,allow_fallback=False,auto_continue_loading=False)
        save('ready.json',ready)
        if not ready.get('ok'): raise RuntimeError('ready failed')
        resolution=exact_isolated_resolution(pid); save('resolution.json',resolution)
        client=resolution.get('info',{}).get('clientRect',{})
        if not resolution.get('ok') or (client.get('width'),client.get('height'))!=(2560,1440): raise RuntimeError('resolution')
        receipt['isolated']=True
        modules = war3.STATE.retained_native_process.snapshot_modules()
        save('modules.json', modules)
        if not modules: raise RuntimeError('module snapshot unavailable')
        if any('reshade' in json.dumps(m).lower() for m in modules): raise RuntimeError('ReShade still loaded')
        receipt['reshadeAbsent'] = True
        time.sleep(3)
        status=checked_status(pid,'');save('status-start.json',status)
        actual_recording=status['response']['result']['perf']['recording']
        if actual_recording != (args.recording=='on'): raise RuntimeError('recording identity')
        if args.measure_seconds:
            def sync_status(label):
                value=war3._invoke_internal_test_request(pid,GAME,'native_frame_sync.status',{},timeout_sec=6)
                save(label+'.json',value)
                if not value.get('ok') or not value.get('response',{}).get('ok'): raise RuntimeError('sync status')
                return value['result']
            a=sync_status('sync-start')
            for _ in range(args.measure_seconds*2): witness(); time.sleep(.5)
            b=sync_status('sync-end')
            if a['persistent']!=(args.persistent_sync!='0') or b['persistent']!=a['persistent']: raise RuntimeError('persistent gate')
            if a.get('consoleAttached', True) or b.get('consoleAttached', True): raise RuntimeError('automatic console still attached')
            if a['frequency']!=b['frequency'] or b['frequency']<=0: raise RuntimeError('clock identity')
            count=b['presents']-a['presents']; ticks=int(b['queryTicks'])-int(a['queryTicks'])
            calls=b['calls']-a['calls']; elided=b['elided']-a['elided']; retained=b['retained']-a['retained']
            if count<=0 or ticks<=0: raise RuntimeError('no Present population')
            if args.persistent_sync!='0' and not (a['installed'] and b['installed'] and calls==elided and retained==0 and abs(calls-count)<=1):
                raise RuntimeError('persistent all-ordinary-frame elision failed')
            receipt['intervalMeasurement']={'presents':count,'calls':calls,'elided':elided,'retained':retained,
                'seconds':ticks/b['frequency'],'meanMs':ticks/b['frequency']*1000/count,
                'boundary':'owner command query to query, NOT exact individual Present intervals',
                'isolatedOnly':True,'includesScreenshot':False}
            mark('recording-independent interval '+str(receipt['intervalMeasurement']))
        for label,count,expected in [('single',1,1),('burst',4,3),('reuse',1,1)]:
            witness(); old={str(p) for p in screenshots.glob('*.tga')}
            cmd=war3._invoke_internal_test_request(pid,GAME,'screenshot.native_async',{'count':count},timeout_sec=6)
            save(label+'-command.json',cmd)
            if not cmd.get('ok'): raise RuntimeError(label+' callback dispatch failed')
            shots_after(old,expected,label)
            mark(label+' saved/decoded '+str(expected)+' TGA files')
            time.sleep(.5)
        for n in range(args.flicker_bursts):
            witness(); old={str(p) for p in screenshots.glob('*.tga')}
            label=f'consecutive-{n:02d}'
            cmd=war3._invoke_internal_test_request(pid,GAME,'screenshot.flicker_burst',{},timeout_sec=6)
            save(label+'-command.json',cmd)
            if not cmd.get('ok'): raise RuntimeError('burst admission failed')
            shots_after(old,3,label)
            mark(label+' three consecutive full-resolution frames saved')
            time.sleep(.5)
        # Leave time for one report and asynchronous GPU errors to surface.
        for _ in range(20): witness(); time.sleep(.5)
        save('status-end.json',checked_status(pid,''))
        receipt['screenshotChecksPassed']=True
    except BaseException as exc:
        error=repr(exc);receipt['error']=error
        save('failure.json',{'error':error,'traceback':traceback.format_exc()});mark(error)
    finally:
        if pid or war3.STATE.war3_pid:
            # WM_CLOSE while still in a map may open an interactive confirmation
            # instead of terminating. EndGame(false) uses the stock lifecycle
            # on the owned game thread, then WM_CLOSE closes the menu.
            try:
                witness()
                end = war3._invoke_internal_test_request(pid,GAME,'game.end_for_exit_test',{},timeout_sec=6)
                save('end-game.json',end)
                time.sleep(3)
            except BaseException as exc:
                save('end-game-error.json',{'error':repr(exc)})
            natural=war3.stop_war3(pid=pid or war3.STATE.war3_pid,graceful_wait_sec=10,force=False,avoid_foreground_switch=True)
            save('natural-exit.json',natural)
            receipt['naturalExit'] = natural
            natural_proof = natural.get('nativeTermination', {})
            if natural.get('ok') and natural_proof.get('exitCode') != 0:
                error=error or 'process disappeared with NONZERO exit code; not normal exit'
                receipt['naturalExitFailure'] = {
                    'exitCode':natural_proof.get('exitCode'), 'zeroExitRequired':True}
            stop = natural
            if not natural.get('ok'):
                error=error or 'native WM_CLOSE did not settle in 10s; not a fixed-exit claim'
                # Earlier debugger/minidump attempts were denied. Keep their
                # archived failures; do not repeatedly attach or block cleanup.
                try:
                    owned = war3.STATE.retained_native_process
                    save('hung-witness.json', owned.snapshot())
                    save('hung-modules.json', owned.snapshot_modules())
                except BaseException as exc:
                    save('hung-witness-error.json', {'error':repr(exc)})
                stop=war3.stop_war3(pid=pid or war3.STATE.war3_pid,graceful_wait_sec=1,force=True,avoid_foreground_switch=True)
            save('stop.json',stop);receipt['stop']=stop
            final = stop.get('finalize', stop.get('stateFinalize', {}))
            if not (stop.get('ok') and final.get('desktop',stop.get('desktop',{})).get('closed') and
                    final.get('videoRestore',stop.get('videoRestore',{})).get('ok') and
                    final.get('terminationProof',stop.get('nativeTermination',{})).get('exact')):
                error=error or 'exact stop/desktop/video restoration failed'
        try:
            zero_processes()
            if deployed:
                if identity(live)['sha256']!=expected_sha or identity(out/'baseline.dll')['sha256']!=BASE: raise RuntimeError('restore conflict; no overwrite')
                shutil.copyfile(out/'baseline.dll',live)
            receipt['restored']=identity(live);receipt['restoreOk']=receipt['restored']['sha256']==BASE
            receipt['zeroProcesses']=zero_processes()
            receipt['mapUnchanged']=identity(MAP)['sha256']==MAP_SHA
            receipt['playerUnchanged']=identity(PLAYER)['sha256']==PLAYER_SHA
            if not receipt['mapUnchanged'] or not receipt['playerUnchanged']: raise RuntimeError('protected player input changed')
        except BaseException as exc: receipt['restoreError']=repr(exc);error=error or repr(exc)
        with war3.STATE.debug_lock: events=list(war3.STATE.debug_events)
        save('debug-events.json',events)
        for p in GAME.glob('*.log'): copy_new(p,out/('after-root-'+p.name))
        for p in (GAME/'WarVK/Log').glob('*.html'):
            if p.stat().st_mtime_ns>initial_reports.get(str(p),0): copy_new(p,out/p.name)
        receipt['newDumps']=[str(p) for p in GAME.rglob('*.dmp') if str(p) not in initial_dumps]
        receipt['newScreenshotPaths']=[str(p) for p in screenshots.glob('*') if str(p) not in initial_shots]
        try: receipt['gpuEvents']=gpu_events_since(start)
        except BaseException as exc: receipt['gpuEventQueryError']=repr(exc);error=error or repr(exc)
        if receipt.get('gpuEvents') or receipt['newDumps']: error=error or 'GPU event or dump during run'
        receipt['ok']=error is None and receipt.get('restoreOk',False)
        save('receipt.json',receipt)
        mark('游戏资源已释放' if receipt.get('restoreOk') and not receipt.get('restoreError') else '恢复/清场需要处理')
    return 0 if receipt['ok'] else 1

if __name__=='__main__': raise SystemExit(main())
