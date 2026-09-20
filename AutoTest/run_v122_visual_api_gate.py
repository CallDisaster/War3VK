"""One isolated 2560x1440 functional transaction; never an FPS/release gate.

Default uses native carriers; --jass-vm uses the separately pinned copied map
and requires receipts produced by its real JASS timer. These proofs stay separate.

Uses the existing audited AutoTest host read-only. Never edits the player install,
the input map, global input, another process or the frozen backup. Each invocation
has a new artifact directory and unconditionally settles its owned game process.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import struct
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
HELPERS = ROOT.parent / 'dxvk/AutoTest'
sys.path.insert(0, str(HELPERS))
from run_frame_timeline_diagnostic import (  # read-only existing launch host
    war3, identity, copy_new, zero_processes, gpu_events_since,
    exact_isolated_resolution,
)

GAME = Path('E:/Work/War3')  # Dedicated older test install, not the player's install.
PLAYER = Path('E:/Work/Warcraft III/d3d9.dll')
MAP = Path('E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x')
BASE_SHA = 'A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A'
PLAYER_SHA = 'B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993'
MAP_SHA = '11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF'
GAME_SHA = 'E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class VmRecipeComplete(Exception):
    """Leave the native-only recipe while retaining the common finally settlement."""


def main():
    global MAP, MAP_SHA
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--size', required=True, type=int)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--jass-vm', action='store_true', help='run the pinned copied-map JASS timer recipe')
    args = parser.parse_args()
    if args.jass_vm:
        manifest = ROOT/'AutoTest/artifacts/v122_visual_api_20260914/jass-vm-map-r3/manifest.json'
        pinned = json.loads(manifest.read_text())
        MAP = Path(pinned['map']['path'])
        MAP_SHA = pinned['map']['sha256']
        require(identity(MAP) == pinned['map'], 'pinned VM map changed')
        require(identity(ROOT/'WarVK/jass/warvk_api.j') == pinned['api'], 'JASS API differs from map')
        require(identity(ROOT/'AutoTest/v122_jass_vm_scenario.j') == pinned['harness'], 'JASS harness differs from map')
        for stage in range(10):
            require(not (GAME/f"WarVK/Temp/{pinned['receiptPrefix']}_stage_{stage}.txt").exists(), 'prior VM receipt exists')
    if not args.name.replace('_', '').isalnum():
        raise ValueError('invalid artifact name')
    out = ROOT / 'AutoTest/artifacts/v122_visual_api_20260914' / args.name
    if out.exists():
        raise ValueError('artifact already exists')
    source = ROOT / 'build32/src/d3d9/d3d9.dll'
    live = GAME / 'd3d9.dll'
    zero_processes()
    before, candidate, player = identity(live), identity(source), identity(PLAYER)
    require(before['sha256'] == BASE_SHA and before['size'] == 35946664, 'test baseline identity')
    require(player['sha256'] == PLAYER_SHA and player['size'] == 34577677, 'player identity')
    require(candidate['sha256'] == args.sha256.upper() and candidate['size'] == args.size, 'candidate identity')
    require(identity(MAP)['sha256'] == MAP_SHA, 'map identity')
    require(identity(GAME / 'Game.dll')['sha256'] == GAME_SHA, 'Game.dll identity')
    raw = source.read_bytes()
    pe = struct.unpack_from('<I', raw, 60)[0]
    require(raw[:2] == b'MZ' and raw[pe:pe+4] == b'PE\0\0', 'PE signature')
    require(struct.unpack_from('<H', raw, pe+4)[0] == 0x14c, 'i386 machine')
    require(struct.unpack_from('<H', raw, pe+24)[0] == 0x10b, 'PE32 magic')
    if not args.execute:
        print(json.dumps({'preflight': True, 'candidate': candidate, 'baseline': before,
                          'playerUntouched': player, 'runtimeAuthorizedByThisCall': False}))
        return 0
    out.mkdir()

    def save(name, value):
        with (out / name).open('x', encoding='utf-8') as f:
            json.dump(value, f, ensure_ascii=False, indent=2, allow_nan=False, default=str)

    def mark(message):
        print(time.strftime('%H:%M:%S'), message, flush=True)

    copy_new(live, out / 'baseline.dll')
    copy_new(source, out / 'candidate.dll')
    copy_new(Path(__file__), out / 'runner-source.py')
    save('preflight.json', {'baseline': before, 'candidate': candidate, 'player': player,
                          'map': identity(MAP), 'game': identity(GAME/'Game.dll'),
                          'host': identity(Path(war3.__file__)), 'helpers': identity(HELPERS/'run_frame_timeline_diagnostic.py')})
    old_logs = out / 'before-logs'
    old_logs.mkdir()
    previous = list(GAME.glob('*.log')) + list((GAME/'WarVK/Log').glob('*.log'))
    previous += [p for p in (GAME/'WarVK/Temp').glob('*.json') if p.is_file()]
    for i, path in enumerate(previous):
        copy_new(path, old_logs / f'{i}-{path.name}')
    initial_dumps = {str(p) for p in GAME.rglob('*.dmp')}
    start = time.time()
    gpu_events_since(start)
    overrides = {
        'DXVK_WAR3_INTERNAL_TEST_API': '1', 'DXVK_WAR3_VISUAL_API_DIAGNOSTICS': '1',
        'DXVK_WAR3_RENDER_LOG': '1', 'DXVK_WAR3_PERF_MONITOR': '1', 'DXVK_WAR3_PERF_LEVEL': '1',
        'DXVK_WAR3_PERF_RECORD_ON_START': '0', 'DXVK_WAR3_PERF_RECORD_ON_GAME_START': '0',
        'DXVK_WAR3_PERF_AUTO_EXPORT_SEC': '0', 'DXVK_WAR3_RUNTIME_BENCHMARK': '0',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE': '1',
        'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE': '1', 'DXVK_WAR3_SCENARIO': args.name,
        'DISABLE_VULKAN_OBS_CAPTURE': '1',
    }
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_', 'DXVK_RUNTIME_')):
            os.environ.pop(key)
    save('environment.json', overrides)
    pid, deployed, call_id = 0, False, 0
    receipt = {'productAccepted': False, 'jassBytecodeAccepted': False, 'runCount': 0,
               'globalInputUsed': False, 'calls': [], 'captures': [], 'errors': []}

    def witness():
        process = war3.STATE.retained_native_process
        if not process or process.pid != pid or process.poll() is not None:
            raise RuntimeError('owned native process exited')

    def invoke(command, payload=None):
        nonlocal call_id
        witness()
        result = war3._invoke_internal_test_request(pid, GAME, command, payload or {}, timeout_sec=6)
        call_id += 1
        save(f'call-{call_id:03d}.json', result)
        if not result.get('ok'):
            raise RuntimeError(f'{command}: {result}')
        return result['result']

    def api(command, arguments='', carrier='Preloader', error=0):
        value = invoke('jass.public_api', {'carrier': carrier, 'payload': 'warvk:v1;'+command+arguments})
        receipt['calls'].append({'command': command, 'arguments': arguments, 'carrier': carrier, **value})
        if not value.get('invoked') or value.get('errorCode') != error:
            raise RuntimeError(f'API result mismatch {command}: {value}')
        return value

    def capture(label):
        time.sleep(1)
        witness()
        path = out / (label+'.bmp')
        result = war3._request_internal_frame_capture(pid, path, GAME, timeout_sec=12)
        save(label+'-capture.json', result)
        if not result.get('ok'):
            raise RuntimeError('internal capture failed: '+label)
        from PIL import Image
        with Image.open(path) as image:
            image.load()
            if image.size != (2560, 1440):
                raise RuntimeError('actual backbuffer size mismatch')
        receipt['captures'].append({'label': label, **identity(path)})

    def wait_volume(backend, fog_count):
        first = invoke('visual.snapshot')['execution']
        deadline = time.monotonic()+12
        while time.monotonic() < deadline:
            value = invoke('visual.snapshot')
            e = value['execution']
            if (e['observedFrames'] >= first['observedFrames']+12 and
                    e['frameSerial'] > first['frameSerial'] and e['mapEpoch'] and e['deviceEpoch'] and
                    e['compositeSubmitted'] and e['effectiveBackend'] == backend and e['fogVolumes'] == fog_count):
                return value
            time.sleep(.25)
        raise RuntimeError('volume execution gate failed: '+str(value))

    def vm_recipe():
        labels = ['sun-on', 'sun-off', 'sphere-high', 'fog-disabled', 'box-high',
                  'cylinder-high', 'cylinder-medium', 'low-angle-high', 'inside-high', 'cleanup']
        receipt['vmStages'] = []
        for stage, label in enumerate(labels):
            path = GAME/f"WarVK/Temp/{pinned['receiptPrefix']}_stage_{stage}.txt"
            deadline = time.monotonic()+(65 if stage == 0 else 15)
            match = None
            while time.monotonic() < deadline:
                witness()
                if path.exists():
                    content = path.read_text(encoding='utf-8', errors='replace')
                    match = re.search(r'V122_VM_STAGE=(\d+);checks=(\d+);errors=(\d+)', content)
                    if match:
                        break
                time.sleep(.1)
            require(match is not None, 'JASS VM receipt timeout: '+label)
            copy_new(path, out/path.name)
            require(int(match[1]) == stage and int(match[3]) == 0, 'JASS VM assertions failed: '+content)
            if stage in (2, 4, 5, 6, 7, 8):
                value = wait_volume(1 if stage == 6 else 2, 1)
            else:
                time.sleep(.2)
                value = invoke('visual.snapshot')
            if stage in (0, 1):
                require(value['sunEnabled'] is (stage == 0), 'JASS sun mailbox mismatch')
            if stage == 9:
                require(value['fogCount'] == 0 and value['volumetricEnabled'] is False, 'JASS cleanup failed')
            save('vm-'+label+'.json', value)
            if stage != 9:
                capture('vm-'+label)
            receipt['vmStages'].append(dict(stage=stage, label=label, checks=int(match[2]), errors=int(match[3])))
            mark('JASS VM stage passed: '+label)
        receipt['jassBytecodeAccepted'] = True
        receipt['jassApiScope'] = 'sun-local-fog-backends-typed-point-and-scalar-string-fallback-not-all-109'
        receipt['nativeCarrierAndSubmissionGate'] = True

    try:
        zero_processes()
        require(identity(live) == before and identity(PLAYER) == player, 'pre-deploy identity race')
        require(identity(out/'baseline.dll')['sha256'] == BASE_SHA, 'backup identity')
        deployed = True
        shutil.copyfile(out/'candidate.dll', live)
        require(identity(live)['sha256'] == candidate['sha256'], 'deployed identity')
        mark('launch one isolated 2560x1440 functional run; player install unchanged')
        launch = war3.launch_war3_test(
            war3_dir=str(GAME), map_path=str(MAP), windowed=True, use_isolated_desktop=True,
            desktop_name=args.name, auto_perf_record=False, record_after_game_started=False,
            auto_perf_export_sec=0, deploy_d3d9_before_launch=False, enforce_video_baseline=True,
            baseline_width=2560, baseline_height=1440, profile='full_default', render_log=True,
            env_overrides_json=json.dumps(overrides), expected_map_sha256=MAP_SHA)
        save('launch.json', launch)
        pid = int(launch.get('pid') or war3.STATE.war3_pid or 0)
        receipt.update(pid=pid, runCount=int(pid > 0))
        if not launch.get('ok') or launch.get('useIsolatedDesktop') is not True:
            raise RuntimeError('launch/isolation failed')
        for key, value in overrides.items():
            if launch.get('envOverrides', {}).get(key) != value:
                raise RuntimeError('requested env mismatch '+key)
            if key.startswith('DXVK_WAR3_') and launch.get('effectiveWar3Environment', {}).get(key) != value:
                raise RuntimeError('effective env mismatch '+key)
        ready = war3.wait_for_game_ready(timeout_sec=100, pid=pid, allow_fallback=False, auto_continue_loading=False)
        save('ready.json', ready)
        if not ready.get('ok'):
            raise RuntimeError('in-game ready failed')
        size = exact_isolated_resolution(pid)
        save('resolution.json', size)
        if not size.get('ok'):
            raise RuntimeError('isolated client resize failed')
        invoke('game.pause', {'paused': False})
        bridge = invoke('jass.bridge_selftest', {'displayText': False})
        if not bridge.get('publicV1Ok'):
            raise RuntimeError('public carrier selftest failed')
        camera = invoke('camera.snapshot')
        x, y, z = (camera[k] for k in ('targetX', 'targetY', 'targetZ'))
        invoke('camera.fixed', {'targetX': x, 'targetY': y, 'targetDistance': 1800,
                               'angleDegrees': 304, 'rotationDegrees': 90, 'fieldOfViewDegrees': 70})
        if args.jass_vm:
            vm_recipe()
            raise VmRecipeComplete()
        api('lightingCycle.setCelestialMotionEnabled', ';b:0')
        api('lightingCycle.setTimeColorGradingEnabled', ';b:0')
        api('lightingClock.holdTime', ';r:12')
        api('sun.setDirection', ';r:-0.35;r:-0.8;r:-0.48')
        api('sun.setColorIntensity', ';r:1;r:0.9;r:0.7;r:1.5')
        api('sun.setEnabled', ';b:1')
        api('csm.setEnabled', ';b:1')
        api('volumetric.setEnabled', ';b:0')
        capture('sun-on')
        api('sun.setEnabled', ';b:0')
        require(invoke('visual.snapshot')['sunEnabled'] is False, 'sun setting not applied to mailbox')
        capture('sun-off')
        api('sun.setEnabled', ';b:1')
        capture('sun-restored')
        api('volumetric.setGlobalMediumEnabled', ';b:0')
        api('volumetric.setDensity', ';r:0')
        api('volumetricFog.setEnabled', ';b:0')
        api('volumetric.setScattering', ';r:2.1;r:0.96')
        api('volumetric.setQuality', ';i:16;r:1800')
        api('volumetric.setBackend', ';i:2')
        api('volumetric.setEnabled', ';b:1')
        create = f';r:{x:.3f};r:{y:.3f};r:{z+150:.3f};r:650;r:0.8;r:0.2'
        fog = api('localFog.createSphere', create, 'GetLocalizedHotkey')['integer']
        if fog <= 0:
            raise RuntimeError('fog create did not return a handle')
        mark('sphere created; waiting for actual Froxel High + composite')
        save('sphere-high.json', wait_volume(2, 1))
        capture('sphere-high')
        api('localFog.setEnabled', f';d:{fog};b:0')
        time.sleep(1)
        capture('fog-disabled')
        api('localFog.setEnabled', f';d:{fog};b:1')
        api('localFog.setDensity', f';d:{fog};r:0.55')
        api('localFog.setEdgeFeather', f';d:{fog};r:0.3')
        api('localFog.setSphereRadius', f';d:{fog};r:800')
        api('localFog.setPosition', f';d:{fog};r:{x+80:.3f};r:{y:.3f};r:{z+160:.3f}')
        save('sphere-edited.json', wait_volume(2, 1))
        api('localFog.setBoxSize', f';d:{fog};r:100;r:100;r:100', error=19)
        api('localFog.destroy', f';d:{fog}')
        require(api('localFog.isAlive', f';d:{fog}', 'GetLocalizedHotkey')['integer'] == 0, 'stale fog handle still alive')
        api('localFog.setDensity', f';d:{fog};r:0.5', error=19)
        center = f';r:{x:.3f};r:{y:.3f};r:{z+150:.3f}'
        for shape, tail in [('Box', ';r:1200;r:750;r:600;r:0.65;r:0.2'),
                            ('Cylinder', ';r:550;r:700;r:0.65;r:0.2')]:
            fog = api('localFog.create'+shape, center+tail, 'GetLocalizedHotkey')['integer']
            if fog <= 0:
                raise RuntimeError('shape create failed')
            api('localFog.setRotation', f';d:{fog};r:0;r:0;r:35')
            save(shape.lower()+'-high.json', wait_volume(2, 1))
            capture(shape.lower()+'-high')
            api('volumetric.setBackend', ';i:1')
            save(shape.lower()+'-medium.json', wait_volume(1, 1))
            # 1440p legacy 16-step sun scattering exceeds the frozen 350 Mi
            # texture-work bound (first run retained as FAILED). Test the
            # supported eight-step legacy setting without changing that bound.
            api('volumetric.setQuality', ';i:8;r:1800')
            api('volumetric.setBackend', ';i:0')
            save(shape.lower()+'-legacy.json', wait_volume(0, 1))
            api('volumetric.setBackend', ';i:2')
            api('volumetric.setQuality', ';i:16;r:1800')
            api('localFog.destroy', f';d:{fog}')
        api('volumetric.setBackend', ';i:3', error=19)
        api('volumetric.setDensity', ';r:nan', error=15)
        api('bloom.setEnabled', ';b:1', error=18)
        api('volumetric.setEnabled', ';b:0')
        require(invoke('visual.snapshot')['fogCount'] == 0, 'fog object leak')
        receipt['nativeCarrierAndSubmissionGate'] = True
        mark('native API and three-shape/backend gates passed; pixels await separate review')
    except VmRecipeComplete:
        pass
    except BaseException as exc:
        receipt['errors'].append(repr(exc))
        save('failure.json', {'error': repr(exc), 'traceback': traceback.format_exc()})
    finally:
        try:
            if pid:
                stop = war3.stop_war3(pid=pid, force=True, avoid_foreground_switch=True)
                save('stop.json', stop)
                if not stop.get('ok'):
                    raise RuntimeError('exact process/desktop settlement failed')
            zero_processes()
            if deployed:
                if (identity(live)['sha256'] != candidate['sha256'] or
                        identity(out/'baseline.dll')['sha256'] != BASE_SHA):
                    raise RuntimeError('restore identity conflict; refusing overwrite')
                shutil.copyfile(out/'baseline.dll', live)
            receipt['restored'] = identity(live)
            receipt['restoreOk'] = receipt['restored']['sha256'] == BASE_SHA
            receipt['playerUnchanged'] = identity(PLAYER) == player
            receipt['sourceUnchanged'] = identity(source) == candidate
            receipt['mapUnchanged'] = identity(MAP)['sha256'] == MAP_SHA
            receipt['zeroProcesses'] = zero_processes()
        except BaseException as exc:
            receipt['errors'].append('settlement: '+repr(exc))
        with war3.STATE.debug_lock:
            save('debug-events.json', list(war3.STATE.debug_events))
        for p in GAME.glob('*.log'):
            copy_new(p, out / ('after-'+p.name))
        receipt['newDumps'] = [str(p) for p in GAME.rglob('*.dmp') if str(p) not in initial_dumps]
        try:
            receipt['gpuEvents'] = gpu_events_since(start)
        except BaseException as exc:
            receipt['errors'].append('GPU query: '+repr(exc))
        if receipt.get('gpuEvents') or receipt['newDumps']:
            receipt['errors'].append('GPU event or new crash dump')
        receipt['ok'] = (not receipt['errors'] and receipt.get('restoreOk') is True and
                         receipt.get('playerUnchanged') is True and receipt.get('sourceUnchanged') is True and
                         receipt.get('mapUnchanged') is True and receipt.get('nativeCarrierAndSubmissionGate') is True)
        save('receipt.json', receipt)
        mark('游戏资源已释放' if receipt.get('restoreOk') and receipt.get('zeroProcesses') == [] else '恢复/清场需要处理')
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
