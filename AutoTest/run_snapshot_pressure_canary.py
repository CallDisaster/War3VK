"""One bounded owner-controlled pressure/relief diagnostic, never release acceptance.

No builds, global input, desktop switching, forced termination, budget increase,
or change to caster eligibility. Restores the exact pre-run DLL, retains backups.
Imports this worktree's AutoTest (not the historical A-tree runner).
"""
import argparse
import ctypes
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import threading
import time

import war3_autotest_mcp as war3

ROOT = Path(__file__).resolve().parents[1]
SITE = Path('E:/Work/Warcraft III')
PROTECTED = Path('E:/Work/War3/d3d9.dll')
MAP = SITE/'Maps/(4)生与死v1.28读档bug修复.w3x'
HISTORICAL_PINS = {
    'candidate': ('BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3', 36359521),
    'live': ('F2A7A6FC8FFAD077CEE70D69A2F0B16B48AC514837D64BC7D66CE3804F2CC33D', 36358788),
    'game': ('E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A', 13187048),
    'exe': ('EA8F5192EFEDE23F84BC60140C2D4A0085EA68B86F430C1D60C354A555922DEF', 514536),
    'map': ('548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F', 62290145),
}
# Historical identities above are read-only.  They never authorize --apply.

FROZEN_BINDING_SCHEMA = 2
FROZEN_ROUTE = {
    'baselineSeconds': 20,
    'pressureMoveSeconds': 10,
    'offsets': [[0, 0], [900, 0], [900, 900], [0, 900], [-900, 0], [0, 0]],
    'reliefSeconds': 80,
    'cameraAngleOfAttack': 335,
    'cameraDurationSec': 1.5,
}
HEAVY_FORENSICS_OFF = (
    'DXVK_WAR3_FRAME_EVIDENCE', 'DXVK_WAR3_FRAME_EVIDENCE_INPUTS',
    'DXVK_WAR3_FRAME_EVIDENCE_DRAWS', 'DXVK_WAR3_FRAME_EVIDENCE_CASTERS',
    'DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT',
    'DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED',
    'DXVK_WAR3_NATIVE_MODEL_LIGHTS', 'DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER',
    'DXVK_WAR3_DEBUG_CONSOLE', 'DXVK_WAR3_RENDER_LOG',
    'DXVK_WAR3_DATA_COLLECTION_TREE',
)
REQUIRED_RUN_ENV_KEYS = (
    'DXVK_WAR3_INTERNAL_TEST_API', 'DXVK_WAR3_INTERNAL_EXIT_TEST',
    'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE',
    'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE', 'DXVK_WAR3_PERF_MONITOR',
    'DXVK_WAR3_PERF_LEVEL', 'DXVK_WAR3_ASYNC_SCREENSHOT',
    'DXVK_WAR3_PERF_AUTO_EXPORT_SEC',
    'DXVK_WAR3_PERF_HISTORY_FRAMES', 'DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB',
    'DXVK_WAR3_PROFILE', 'DXVK_WAR3_SCENARIO', 'DISABLE_VULKAN_OBS_CAPTURE',
    'DXVK_WAR3_STAGE11_BUDGET_CENSUS', 'DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE',
) + HEAVY_FORENSICS_OFF


def read_strict_json(path):
    text_value = path.read_text(encoding='utf-8-sig')
    return json.loads(text_value, object_pairs_hook=strict_pairs,
                      parse_constant=reject_constant)


def validate_frozen_manifest(manifest):
    require(isinstance(manifest, dict), 'frozen manifest must be object')
    require(manifest.get('schema') == FROZEN_BINDING_SCHEMA,
            'frozen manifest schema mismatch')
    run_id = str(manifest.get('runId', '')).strip()
    require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,95}', run_id) is not None,
            'frozen manifest runId must be a path-safe component')
    scenario = str(manifest.get('scenario', '')).strip()
    require(scenario != '', 'frozen manifest scenario missing')
    profile = str(manifest.get('profile', '')).strip()
    require(profile == 'full_default', 'frozen manifest profile must be full_default')
    matrix = str(manifest.get('matrix', '')).strip()
    require(matrix != '', 'frozen manifest matrix missing')
    require(manifest.get('resolution') == {'width': 2560, 'height': 1440},
            'frozen manifest resolution must be 2560x1440')
    require(manifest.get('route') == FROZEN_ROUTE,
            'frozen manifest route differs from fixed route')
    require(manifest.get('isolatedDesktop') is True,
            'frozen manifest must require isolated desktop')
    require(manifest.get('globalInputUsed') is False,
            'frozen manifest must forbid global input')
    identity_names = ('candidate', 'liveRecovery', 'map', 'game', 'exe')
    identities = {}
    for name in identity_names:
        value = manifest.get(name)
        require(isinstance(value, dict), 'frozen identity missing: ' + name)
        sha = str(value.get('sha256', '')).upper()
        size = value.get('size')
        require(re.fullmatch(r'[0-9A-F]{64}', sha) is not None,
                'frozen identity sha invalid: ' + name)
        require(type(size) is int and size > 0, 'frozen identity size invalid: ' + name)
        identities[name] = {'sha256': sha, 'size': size}
    env = manifest.get('env')
    require(isinstance(env, dict), 'frozen manifest env missing')
    for key in REQUIRED_RUN_ENV_KEYS:
        require(key in env, 'frozen manifest env missing: ' + key)
    for key in HEAVY_FORENSICS_OFF:
        require(str(env[key]) == '0', 'heavy forensics must be frozen off: ' + key)
    for key, expected in {
        'DXVK_WAR3_INTERNAL_TEST_API': '1',
        'DXVK_WAR3_INTERNAL_EXIT_TEST': '1',
        'DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE': '1',
        'DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE': '1',
        'DXVK_WAR3_PERF_MONITOR': '1',
        'DXVK_WAR3_PERF_LEVEL': '1',
        'DXVK_WAR3_ASYNC_SCREENSHOT': '1',
        'DXVK_WAR3_PERF_AUTO_EXPORT_SEC': '20',
    }.items():
        require(str(env[key]) == expected,
                'frozen manifest env value mismatch: ' + key)
    require(str(env['DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB']) == '384',
            'frozen manifest cap must be 384')
    require(str(env['DXVK_WAR3_PROFILE']) == 'full_default',
            'frozen manifest env profile mismatch')
    require(str(env['DXVK_WAR3_SCENARIO']) == scenario,
            'frozen manifest env scenario mismatch')
    require(str(env['DXVK_WAR3_PERF_HISTORY_FRAMES']) == '4000',
            'frozen manifest perf history frame count')
    require(str(env['DISABLE_VULKAN_OBS_CAPTURE']) == '1',
            'frozen manifest OBS capture must be off')
    require(str(env['DXVK_WAR3_STAGE11_BUDGET_CENSUS']) == '1',
            'frozen manifest census must be on')
    require(str(env['DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE']) in ('0', '1'),
            'frozen manifest upload range must be explicit 0 or 1')
    return {
        'schema': FROZEN_BINDING_SCHEMA,
        'runId': run_id,
        'scenario': scenario,
        'profile': profile,
        'matrix': matrix,
        'resolution': {'width': 2560, 'height': 1440},
        'route': dict(FROZEN_ROUTE),
        'isolatedDesktop': True,
        'globalInputUsed': False,
        'identities': identities,
        'env': {str(key): str(value) for key, value in env.items()},
    }


def load_frozen_manifest(path):
    manifest = read_strict_json(Path(path))
    normalized = validate_frozen_manifest(manifest)
    normalized['manifestPath'] = str(Path(path).resolve())
    normalized['manifestSha256'] = identity(Path(path))['sha256']
    return normalized


def build_binding_manifest(runId, scenario, candidate, liveRecovery, game_map, game,
                           exe, profile, matrix, resolution, route, env,
                           frozenManifestSha256=None):
    return {
        'schema': FROZEN_BINDING_SCHEMA,
        'runId': str(runId),
        'candidateSha256': str(candidate['sha256']).upper(),
        'candidateSize': int(candidate['size']),
        'liveRecoverySha256': str(liveRecovery['sha256']).upper(),
        'liveRecoverySize': int(liveRecovery['size']),
        'mapSha256': str(game_map['sha256']).upper(),
        'mapSize': int(game_map['size']),
        'gameSha256': str(game['sha256']).upper(),
        'gameSize': int(game['size']),
        'exeSha256': str(exe['sha256']).upper(),
        'exeSize': int(exe['size']),
        'profile': str(profile),
        'matrix': str(matrix),
        'resolution': dict(resolution),
        'route': dict(route),
        'isolatedDesktop': True,
        'globalInputUsed': False,
        'frozenManifestSha256': (str(frozenManifestSha256).upper()
                                 if frozenManifestSha256 else None),
        'env': {str(key): str(value) for key, value in dict(env).items()},
    }


MANIFEST_IDENTITY_TO_PATH = {
    'candidate': 'candidate',
    'liveRecovery': 'live',
    'map': 'map',
    'game': 'game',
    'exe': 'exe',
}


def check_frozen_identities(before, identities):
    require(isinstance(identities, dict), 'frozen identities must be object')
    require(set(identities) == set(MANIFEST_IDENTITY_TO_PATH),
            'frozen identity set mismatch')
    checked = {}
    for manifest_key, path_key in MANIFEST_IDENTITY_TO_PATH.items():
        require(path_key in before, 'observed identity missing: ' + path_key)
        expected = identities[manifest_key]
        require(isinstance(expected, dict) and 'sha256' in expected and 'size' in expected,
                'frozen identity shape: ' + manifest_key)
        require(before[path_key] == expected, 'identity changed: ' + manifest_key)
        checked[manifest_key] = before[path_key]
    return checked


def ready_wait_kwargs(launch):
    desktop = launch.get('desktop') or {}
    isolated = bool(launch.get('useIsolatedDesktop')) and bool(desktop.get('nonInteractiveOnly'))
    return {
        'timeout_sec': 100,
        'allow_fallback': False,
        'auto_continue_loading': isolated,
        'continue_key': 'SPACE',
        'continue_max_pulses': 1,
        'require_control_plane_for_continue': True,
    }


def continue_pulse_summary(ready):
    pulses = ready.get('continuePulses')
    if not isinstance(pulses, list):
        return {'count': 0, 'failed': 0, 'failures': [], 'coverage': 'continuePulses missing'}
    failures = [item for item in pulses if not (isinstance(item, dict) and item.get('ok') is True)]
    return {'count': len(pulses), 'failed': len(failures), 'failures': failures[:4],
            'coverage': 'pulse results captured'}


def _is_uint(value):
    return type(value) is int and value >= 0


def _missing_readiness_fields(status):
    if not isinstance(status, dict):
        return ['status']
    missing = []
    module = status.get('module')
    runtime = status.get('runtime')
    render = status.get('render')
    frame = status.get('frame')
    if not isinstance(module, dict) or not isinstance(module.get('state'), str):
        missing.append('module.state')
    if not isinstance(runtime, dict):
        missing.append('runtime')
    else:
        for key in ('jassReady', 'gameStarted', 'runtimeReady'):
            if type(runtime.get(key)) is not bool:
                missing.append('runtime.' + key)
    if not isinstance(render, dict):
        missing.append('render')
    else:
        for key in ('isInGame', 'isLoading', 'worldPtr', 'inGameRenderReady'):
            if key == 'worldPtr':
                if not (_is_uint(render.get(key))):
                    missing.append('render.worldPtr')
            elif type(render.get(key)) is not bool:
                missing.append('render.' + key)
    if not isinstance(frame, dict) or not (_is_uint(frame.get('frameNumber'))):
        missing.append('frame.frameNumber')
    if not _is_uint(status.get('frameIndex')):
        missing.append('frameIndex')
    if not _is_uint(status.get('timestampMs')):
        missing.append('timestampMs')
    return missing




def require(value, reason):
    if not value:
        raise RuntimeError(reason)


def identity(path):
    with path.open('rb') as f:
        sha = hashlib.file_digest(f, 'sha256').hexdigest().upper()
    return dict(size=path.stat().st_size, sha256=sha)


def copy_new(src, dst):
    with src.open('rb') as a, dst.open('xb') as b:
        shutil.copyfileobj(a, b)
    require(identity(src) == identity(dst), 'copy identity: '+str(dst))


def move_new(src, dst):
    # This runner is Windows-only. Path.rename there is fail-if-exists, unlike
    # POSIX rename. Keep the OS gate even in offline transaction tests.
    require(os.name == 'nt', 'Windows fail-if-exists rename required')
    require(not dst.exists(), 'rename target exists: '+str(dst))
    src.rename(dst)


def restore_live_from_backup(live, backup, expected_live, staged):
    """Restore a missing live DLL from the frozen backup without overwriting anything."""
    require(identity(backup) == expected_live, 'restore backup missing/corrupt')
    require(not live.exists(), 'restore target already exists')
    require(not staged.exists(), 'restore staging target exists')
    copy_new(backup, staged)
    require(identity(staged) == expected_live, 'restore staged identity')
    move_new(staged, live)
    require(identity(live) == expected_live, 'restore live identity')
    return dict(live=str(live), backup=str(backup), identity=expected_live)


def install_guarded(src, live, expected_live, expected_new, staged, parked):
    """Stage and verify before touching live; retain every backup/failure file.

    Both renames are on the same volume and never overwrite an unexpected file.
    If the second rename fails, restore the parked file only if live is absent.
    This is not a power-loss-atomic exchange; both copies remain recoverable.
    """
    require(staged.parent.resolve() == live.parent.resolve()
            and parked.parent.resolve() == live.parent.resolve(), 'same-directory transaction')
    require(len({p.resolve() for p in (src, live, staged, parked)}) == 4,
            'transaction paths must differ')
    require(not staged.exists() and not parked.exists(), 'transaction targets exist')
    require(identity(src) == expected_new and identity(live) == expected_live, 'install preflight')
    copy_new(src, staged)  # A partial copy never modifies the live DLL.
    require(identity(src) == expected_new and identity(staged) == expected_new,
            'staged/source identity')
    require(identity(live) == expected_live, 'live changed while staging')
    move_new(live, parked)
    try:
        move_new(staged, live)
    except BaseException:
        if not live.exists() and identity(parked) == expected_live:
            move_new(parked, live)
        raise
    require(identity(live) == expected_new and identity(parked) == expected_live,
            'installed/parked identity')
    return dict(live=str(live), parked=str(parked), identity=expected_new)






def classify_readiness(status):
    """Classify owner-observed readiness; missing fields never become ready."""
    missing = _missing_readiness_fields(status)
    if missing:
        return dict(state='status-incomplete', statusComplete=False,
            missingFields=missing, moduleRunning=False, jassReady=False,
            gameStarted=False, runtimeReady=False, isInGame=False,
            isLoading=False, worldPtr=0, inGameRenderReady=False,
            frameNumber=0, frameIndex=0, timestampMs=0)
    module = status['module']
    runtime = status['runtime']
    render = status['render']
    frame = status['frame']
    running = module['state'] == 'Running'
    world = int(render['worldPtr'])
    is_ingame = render['isInGame']
    game_started = runtime['gameStarted']
    runtime_ready = runtime['runtimeReady']
    in_game_ready = render['inGameRenderReady']
    if not running:
        state = 'module-not-running'
    elif world > 0 and game_started and runtime_ready and in_game_ready:
        state = 'in-game-ready'
    elif world > 0:
        state = 'world-present'
    elif game_started or is_ingame:
        state = 'game-started-render-not-ready'
    elif render['isLoading']:
        state = 'loading'
    elif runtime['jassReady']:
        state = 'menu-ready-not-started'
    else:
        state = 'jass-not-ready'
    return dict(state=state, statusComplete=True, missingFields=[],
        moduleRunning=running, jassReady=runtime['jassReady'],
        gameStarted=game_started, runtimeReady=runtime_ready,
        isInGame=is_ingame, isLoading=render['isLoading'], worldPtr=world,
        inGameRenderReady=in_game_ready,
        frameNumber=int(frame['frameNumber']),
        frameIndex=int(status['frameIndex']),
        timestampMs=int(status['timestampMs']))


def _perf_phase_fields(status):
    """Return the producer-owned completed-frame binding or fail closed."""
    require(isinstance(status, dict), 'runtimeStatus.perf missing: status')
    perf = status.get('perf')
    require(isinstance(perf, dict), 'runtimeStatus.perf missing')
    for key in ('enabled', 'recording', 'frameAnchorValid'):
        require(type(perf.get(key)) is bool and perf.get(key) is True,
                'runtimeStatus.perf.' + key + ' must be true')
    fields = {}
    for key in ('businessFrameSerial', 'perfFrameEpoch', 'producerAccumulationEpoch'):
        value = perf.get(key)
        require(_is_uint(value) and value > 0,
                'runtimeStatus.perf.' + key + ' must be a positive int')
        fields[key] = int(value)
    return fields


def _perf_initial_anchor_state(status):
    """Validate the exact pre-first-completed-frame producer schema.

    Returns (complete, fields).  A pending state is accepted only when the
    producer proves enabled/recording true, a positive accumulation epoch,
    frameAnchorValid=false, and both frame fields exactly zero.  Any malformed,
    disabled, missing, or inconsistent state fails immediately.
    """
    require(isinstance(status, dict), 'runtimeStatus.perf missing: status')
    perf = status.get('perf')
    require(isinstance(perf, dict), 'runtimeStatus.perf missing')
    for key in ('enabled', 'recording', 'frameAnchorValid'):
        require(type(perf.get(key)) is bool,
                'runtimeStatus.perf.' + key + ' must be bool')
    require(perf['enabled'] is True and perf['recording'] is True,
            'runtimeStatus.perf.enabled and recording must be true')
    accumulation = perf.get('producerAccumulationEpoch')
    require(_is_uint(accumulation) and accumulation > 0,
            'runtimeStatus.perf.producerAccumulationEpoch must be a positive int')
    if perf['frameAnchorValid'] is True:
        return True, _perf_phase_fields(status)
    serial = perf.get('businessFrameSerial')
    epoch = perf.get('perfFrameEpoch')
    require(_is_uint(serial) and _is_uint(epoch),
            'runtimeStatus.perf initial frame fields must be ints')
    require(serial == 0 and epoch == 0,
            'runtimeStatus.perf inconsistent initial anchor fields')
    return False, {
        'businessFrameSerial': int(serial),
        'perfFrameEpoch': int(epoch),
        'producerAccumulationEpoch': int(accumulation),
    }


def _control_plane_runtime_status(pid):
    try:
        response = war3._control_plane_request(
            pid=int(pid), command='get_runtime_status', payload={}, timeout_sec=1.5)
    except BaseException as exc:
        raise RuntimeError('phase marker control plane failed: ' + repr(exc))
    require(isinstance(response, dict), 'phase marker control plane response missing')
    require(response.get('transportOk') is True and response.get('ok') is True,
            'phase marker control plane transport/status failed: '
            + json.dumps(response, ensure_ascii=False, default=str))
    status = response.get('result')
    require(isinstance(status, dict), 'phase marker control plane result missing')
    return status


def readiness_diagnostic(pid, label, run_id=''):
    """Pre-ready diagnostic; may be absent/incomplete and never blocks ready wait."""
    status = None
    error = None
    try:
        response = war3._control_plane_request(
            pid=int(pid), command='get_runtime_status', payload={}, timeout_sec=1.5)
        if isinstance(response, dict) and response.get('transportOk') is True and response.get('ok') is True:
            status = response.get('result')
        else:
            error = repr(response)
    except BaseException as exc:
        error = repr(exc)
    return dict(phase=label, pid=int(pid), runId=str(run_id),
        wallUnix=float(time.time()), readiness=classify_readiness(status),
        frameDomain='diagnostic-not-phase-anchor',
        controlPlaneOk=error is None,
        controlPlaneError=None if error is None else error[:240])


def _phase_marker_from_status(status, label, pid, run_id):
    perf = _perf_phase_fields(status)
    return dict(phase=label, pid=int(pid), runId=str(run_id),
        wallUnix=float(time.time()), readiness=classify_readiness(status),
        frameDomain='workload.businessFrameSerial',
        businessFrameSerial=perf['businessFrameSerial'],
        perfFrameEpoch=perf['perfFrameEpoch'],
        producerAccumulationEpoch=perf['producerAccumulationEpoch'])


def phase_marker(pid, label, run_id='', previous=None, owner=None,
                 timeout_sec=5.0, interval_sec=0.1):
    """Acquire one completed phase anchor; bounded wait for a strictly newer frame."""
    require(float(timeout_sec) > 0.0 and float(interval_sec) > 0.0,
            'phase marker timing invalid')
    deadline = time.monotonic() + float(timeout_sec)
    initial_wait_attempts = 0
    initial_wait_pinned_accumulation = None
    last_initial_perf = None
    while True:
        if owner is not None:
            require(owner.poll() is None, 'phase marker owner exited')
        status = _control_plane_runtime_status(pid)
        if previous is None:
            complete, perf = _perf_initial_anchor_state(status)
            if (initial_wait_pinned_accumulation is not None
                    and perf['producerAccumulationEpoch'] != initial_wait_pinned_accumulation):
                raise RuntimeError(
                    'phase marker initial accumulation epoch changed: '
                    + str(initial_wait_pinned_accumulation) + ' -> '
                    + str(perf['producerAccumulationEpoch']))
            if not complete:
                if initial_wait_pinned_accumulation is None:
                    initial_wait_pinned_accumulation = perf['producerAccumulationEpoch']
                initial_wait_attempts += 1
                last_initial_perf = dict(perf)
                if time.monotonic() >= deadline:
                    facts = json.dumps(last_initial_perf, ensure_ascii=False,
                                       sort_keys=True, separators=(',', ':'))
                    raise RuntimeError(
                        'phase marker timeout waiting for initial completed frame; '
                        'attempts=' + str(initial_wait_attempts)
                        + '; lastPerf=' + facts)
                time.sleep(min(float(interval_sec),
                              max(0.0, deadline - time.monotonic())))
                continue
            marker = _phase_marker_from_status(status, label, pid, run_id)
            if initial_wait_attempts:
                marker['initialAnchorWait'] = {
                    'attempts': initial_wait_attempts,
                    'pinnedProducerAccumulationEpoch':
                        initial_wait_pinned_accumulation,
                }
            return marker
        marker = _phase_marker_from_status(status, label, pid, run_id)
        prev_serial = int(previous['businessFrameSerial'])
        prev_epoch = int(previous['perfFrameEpoch'])
        cur_serial = marker['businessFrameSerial']
        cur_epoch = marker['perfFrameEpoch']
        if (int(previous['producerAccumulationEpoch']) != marker['producerAccumulationEpoch']
                or cur_serial < prev_serial or cur_epoch < prev_epoch):
            raise RuntimeError('phase marker identity moved backward or epoch changed')
        if cur_serial > prev_serial and cur_epoch > prev_epoch:
            return marker
        if time.monotonic() >= deadline:
            raise RuntimeError('phase marker timeout waiting for next completed frame')
        time.sleep(min(float(interval_sec), max(0.0, deadline - time.monotonic())))


def strict_pairs(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError('duplicate JSON key: '+key)
        out[key] = value
    return out


def reject_constant(value):
    raise ValueError('non-finite JSON: '+value)


def read_report(path):
    text = path.read_text(encoding='utf-8-sig')
    roots = list(re.finditer(r'\bconst\s+data\s*=\s*(?=\{)', text))
    require(len(roots) == 1, 'one report root')
    data, end = json.JSONDecoder(object_pairs_hook=strict_pairs,
        parse_constant=reject_constant).raw_decode(text, roots[0].end())
    require(text[end:].lstrip().startswith(';'), 'report root boundary')
    return data


def zero_processes():
    script = (
        "$ErrorActionPreference='Stop'; $self=" + str(os.getpid()) +
        "; $p=@(Get-CimInstance Win32_Process | Where-Object { "
        "$_.ProcessId -ne $self -and ("
        "$_.Name -match '^(war3|warcraft iii|worldedit|worldeditor|worldeditydwe|ydwe|ninja|cc1plus|cl|link|ld|clang|clang\\+\\+|gcc|g\\+\\+|meson)\\.exe$' "
        "-or (($_.Name -match '^(python|python3|pythonw|py)\\.exe$') -and "
        "($_.CommandLine -match 'run_snapshot_pressure_canary\\.py|war3_autotest_mcp\\.py'))"
        ") } | Select-Object ProcessId,Name,ExecutablePath,CommandLine); "
        "ConvertTo-Json -InputObject $p -Compress"
    )
    r = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, check=True)
    values = json.loads(r.stdout or '[]')
    require(not values, 'resource process conflict: '+str(values))
    return values


def gpu_events_since(start):
    script = "$ErrorActionPreference='Stop'; $since=[DateTimeOffset]::FromUnixTimeSeconds("+str(int(start))+r").LocalDateTime; $e=@(Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$since} -ErrorAction SilentlyContinue | Where-Object {($_.ProviderName -match '^(nvlddmkm|Display|Microsoft-Windows-DxgKrnl)$') -and ($_.Level -le 2 -or $_.Id -in 153,4101)} | Select-Object Id,RecordId,ProviderName,TimeCreated); ConvertTo-Json -InputObject $e -Compress"
    r = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, check=True)
    return json.loads(r.stdout or '[]')


def exact_resolution(pid):
    first = war3._resize_window_client_native(pid, 2560, 1440, x=0, y=0)
    rect = first.get('info', {}).get('clientRect', {})
    if (rect.get('width'), rect.get('height')) == (2560, 1440):
        return first
    require(war3.STATE.desktop_handle and war3.STATE.war3_pid == pid, 'isolated owner required')
    hwnd = int(first.get('hwnd', 0))
    require(hwnd != 0, 'owned HWND absent')
    u = ctypes.WinDLL('user32', use_last_error=True)
    actual = ctypes.c_ulong()
    u.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(actual))
    require(actual.value == pid, 'HWND PID mismatch')
    u.GetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    u.GetWindowLongW.restype = ctypes.c_long
    u.SetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
    u.SetWindowLongW.restype = ctypes.c_long
    u.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    style = u.GetWindowLongW(hwnd, -16)
    borderless = (style & ~0x00CF0000) | 0x80000000
    u.SetWindowLongW(hwnd, -16, ctypes.c_long(borderless))
    changed = u.SetWindowPos(hwnd, None, 0, 0, 2560, 1440, 0x0034)  # no activation
    time.sleep(.5)
    info = war3._query_window_info_by_hwnd(hwnd, pid=pid)
    return dict(ok=bool(changed) and info.get('ok', False), info=info,
                firstAttempt=first, isolatedBorderless=True, oldStyle=style, globalInputUsed=False)


def launch_arguments(name, env):
    return dict(war3_dir=str(SITE), map_path=str(MAP), windowed=True,
        launcher_mode='direct', use_isolated_desktop=True, desktop_name=name,
        auto_perf_record=True, record_after_game_started=True, auto_perf_export_sec=20,
        deploy_d3d9_before_launch=False, enforce_video_baseline=True,
        baseline_width=2560, baseline_height=1440, profile='full_default',
        env_overrides_json=json.dumps(env))


def close_owned_window(launch, owner):
    """Post WM_CLOSE on a worker bound to the exact non-input desktop.

    No foreground switch or process termination; caller separately waits for
    exit through the retained HANDLE before restoring registry/desktop state.
    """
    pin = launch.get('nativeProcessWitness', {})
    desktop = launch.get('desktop', {})
    held = owner.snapshot() if owner else {}
    require(held.get('available') is True and pin.get('pid', 0) > 0
            and pin.get('creationEpochMs', 0) > 0
            and all(held.get(k) == pin.get(k) for k in
                    ('pid', 'creationEpochMs', 'canonicalExePath')), 'close witness mismatch')
    require(launch.get('useIsolatedDesktop') is True
            and desktop.get('nonInteractiveOnly') is True
            and desktop.get('name') and desktop['name'].casefold() != 'default',
            'close requires owned noninteractive desktop')
    require(war3.STATE.war3_pid == pin['pid']
            and war3.STATE.retained_native_process is owner, 'close owner changed')
    result = dict(pid=pin['pid'], forced=False, globalInputUsed=False, posted=[])

    def worker():
        from ctypes import wintypes as w
        u = ctypes.WinDLL('user32', use_last_error=True)
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        u.OpenDesktopW.argtypes = [w.LPCWSTR, w.DWORD, w.BOOL, w.DWORD]
        u.OpenDesktopW.restype = w.HANDLE
        u.GetThreadDesktop.argtypes = [w.DWORD]
        u.GetThreadDesktop.restype = w.HANDLE
        u.SetThreadDesktop.argtypes = [w.HANDLE]
        u.CloseDesktop.argtypes = [w.HANDLE]
        u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        u.EnumDesktopWindows.argtypes = [w.HANDLE, callback_type, w.LPARAM]
        old = u.GetThreadDesktop(k.GetCurrentThreadId())
        handle = None
        bound = False
        try:
            require(owner.poll() is None, 'close: exact process already exited')
            handle = u.OpenDesktopW(desktop['name'], 0, False, 0x1 | 0x40 | 0x80)
            require(bool(handle), 'open owned desktop')
            bound = bool(u.SetThreadDesktop(handle))
            require(bound, 'bind close worker to owned desktop')
            windows = []

            def enumerate_window(hwnd, unused):
                pid = w.DWORD()
                u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value == pin['pid']:
                    title = ctypes.create_unicode_buffer(256)
                    u.GetWindowTextW(hwnd, title, len(title))
                    if title.value == 'Warcraft III':
                        windows.append(hwnd)
                return True

            callback = callback_type(enumerate_window)
            require(bool(u.EnumDesktopWindows(handle, callback, 0)), 'enumerate owned desktop')
            require(len(windows) == 1, 'unique owned game window required')
            pid = w.DWORD()
            u.GetWindowThreadProcessId(windows[0], ctypes.byref(pid))
            require(owner.poll() is None and pid.value == pin['pid'], 'close HWND owner changed')
            require(bool(u.PostMessageW(windows[0], 0x10, 0, 0)), 'WM_CLOSE failed')
            result['posted'].append(int(windows[0]))
        except BaseException as exc:
            result['error'] = repr(exc)
        finally:
            unbound = not bound or bool(u.SetThreadDesktop(old))
            if handle:
                result['desktopHandleClosed'] = bool(unbound and u.CloseDesktop(handle))

    thread = threading.Thread(target=worker, name='owned-desktop-close', daemon=True)
    thread.start()
    thread.join(5)
    require(not thread.is_alive(), 'close worker timeout; no termination fallback')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--frozen-manifest', type=Path, default=None)
    args = parser.parse_args()
    require((not args.apply) or (args.frozen_manifest is not None and args.frozen_manifest.is_file()),
            '--apply requires --frozen-manifest with exact frozen identities')
    manifest = load_frozen_manifest(args.frozen_manifest) if args.frozen_manifest else None
    paths = dict(candidate=ROOT/'build32/src/d3d9/d3d9.dll', live=SITE/'d3d9.dll',
                 game=SITE/'Game.dll', exe=SITE/'war3.exe', map=MAP)
    zero_processes()
    before = {key: identity(path) for key, path in paths.items()}
    protected = identity(PROTECTED)
    if manifest is None:
        print(json.dumps(dict(preflightOnly=True, frozenManifestRequiredForApply=True,
            identities=before, protected=protected), ensure_ascii=False))
        return 0
    artifacts_root = (ROOT/'AutoTest/artifacts').resolve()
    out = artifacts_root / manifest['runId']
    require(out.parent.resolve() == artifacts_root, 'run output must stay under artifacts root')
    require(not out.exists(), 'single run output already exists; no retry')
    check_frozen_identities(before, manifest['identities'])
    require(bool(war3._prefer_inplace_relative_loadfile_arg(SITE, MAP)), 'must load map in place; no test-map overwrite')
    inspect.signature(war3.launch_war3_test).bind(**launch_arguments(out.name, {}))
    require(shutil.disk_usage(ROOT).free >= 2*1024**3, 'two GiB disk headroom')
    if not args.apply:
        print(json.dumps(dict(preflightOnly=True, frozenManifest=manifest,
            identities=before, protected=protected), ensure_ascii=False))
        return 0
    out.mkdir()
    copy_new(Path(manifest['manifestPath']), out/'frozen-manifest.json')

    def save(name, value):
        with (out/name).open('x', encoding='utf-8') as f:
            json.dump(value, f, ensure_ascii=False, indent=2, allow_nan=False, default=str)

    for key in ('candidate', 'live'):
        copy_new(paths[key], out/(key+'.dll'))
    for path in (Path(__file__), Path(war3.__file__)):
        copy_new(path, out/path.name)
    pe = paths['exe'].read_bytes()
    pe_offset = struct.unpack_from('<I', pe, 0x3c)[0]
    save('preflight.json', dict(identities=before, protected=protected,
        exeLaa=bool(struct.unpack_from('<H', pe, pe_offset+22)[0] & 0x20), processes=[]))
    env = dict(manifest['env'])
    save('env.json', env)
    for key in list(os.environ):
        if key.startswith(('DXVK_WAR3_', 'DXVK_RUNTIME_')) or key == 'DXVK_CONFIG':
            os.environ.pop(key)
    logs = SITE/'WarVK/Log'
    old_reports = {p.name: p.stat().st_mtime_ns for p in logs.glob('*.html')}
    old_dumps = {str(p) for p in SITE.rglob('*.dmp')}
    start = time.time()
    gpu_events_since(start)
    receipt = dict(runCount=0, productAccepted=False, mergeAuthorizedByResults=False,
                   globalInputUsed=False, pressureCovered=False, imageRecoveryProven=False)
    receipt['runId'] = manifest['runId']
    receipt['frozenManifestSha256'] = manifest['manifestSha256']
    phase_markers = []
    pid = 0
    deployed = False
    launch = {}
    error = None
    try:
        zero_processes()
        require(all(identity(path) == before[key] for key, path in paths.items()), 'pre-deploy race')
        require(identity(out/'live.dll') == before['live'] and identity(out/'candidate.dll') == before['candidate'], 'frozen copies changed')
        deployed = True
        receipt['deployment'] = install_guarded(out/'candidate.dll', paths['live'],
            before['live'], before['candidate'],
            SITE/('d3d9.dll.staged_deploy_'+out.name),
            SITE/('d3d9.dll.original_'+out.name))
        launch = war3.launch_war3_test(**launch_arguments(out.name, env))
        save('launch.json', launch)
        require(launch.get('ok') and launch.get('useIsolatedDesktop') is True, 'launch/isolation')
        desktop = launch.get('desktop') or {}
        require(desktop.get('nonInteractiveOnly') is True
                and str(desktop.get('name', '')).strip().casefold() not in ('', 'default'),
                'launch/noninteractive desktop')
        pid = int(launch['pid'])
        receipt.update(runCount=1, pid=pid)
        owner = war3.STATE.retained_native_process

        def witness():
            require(owner and owner.pid == pid and owner.poll() is None, 'owned process exited')

        for key, value in env.items():
            require(launch.get('envOverrides', {}).get(key) == value, 'env override '+key)
            if key.startswith('DXVK_WAR3_'):
                require(launch.get('effectiveWar3Environment', {}).get(key) == value, 'effective env '+key)
        res = exact_resolution(pid)
        save('resolution.json', res)
        rect = res.get('info', {}).get('clientRect', {})
        require(res.get('ok') and (rect.get('width'), rect.get('height')) == (2560, 1440), 'actual client dimensions')
        # Hypothesis under test, not an accepted cause: interface_r2 observed
        # jassReady=true/worldPtr=0.  One bounded HWND-scoped continue pulse is
        # allowed only on the owned non-interactive desktop; it is not global input.
        ready_preflight = readiness_diagnostic(pid, 'pre-ready', manifest['runId'])
        save('ready-preflight.json', ready_preflight)
        binding_doc = build_binding_manifest(
            runId=manifest['runId'], scenario=manifest['scenario'],
            candidate=before['candidate'], liveRecovery=before['live'],
            game_map=before['map'], game=before['game'], exe=before['exe'],
            profile=manifest['profile'], matrix=manifest['matrix'],
            resolution=dict(width=rect['width'], height=rect['height']),
            route=manifest['route'], env=env,
            frozenManifestSha256=manifest['manifestSha256'])
        save('binding.json', binding_doc)
        receipt['readyPreflight'] = ready_preflight
        receipt['binding'] = binding_doc
        try:
            ready = war3.wait_for_game_ready(pid=pid, **ready_wait_kwargs(launch))
        except BaseException as exc:
            ready = dict(ok=False, error='wait_for_game_ready exception: ' + repr(exc),
                         continuePulses=[])
            save('ready.json', ready)
            ready_failure = dict(state='status-incomplete', statusComplete=False,
                                 missingFields=['wait_for_game_ready-exception'],
                                 error=ready['error'], continuePulses=[])
            save('ready-failure.json', ready_failure)
            raise RuntimeError('map ready preflight=' + json.dumps(ready_failure, ensure_ascii=False))
        save('ready.json', ready)
        pulse_summary = continue_pulse_summary(ready)
        save('ready-pulses.json', pulse_summary)
        receipt['readyPulses'] = pulse_summary
        if not ready.get('ok'):
            ready_failure = classify_readiness(ready.get('runtimeStatus'))
            ready_failure['continuePulses'] = pulse_summary
            save('ready-failure.json', ready_failure)
            raise RuntimeError('map ready preflight=' + json.dumps(ready_failure, ensure_ascii=False))
        phase_markers.clear()

        def mark_phase(label):
            previous = phase_markers[-1] if phase_markers else None
            marker = phase_marker(pid, label, manifest["runId"],
                                  previous=previous, owner=owner)
            phase_markers.append(marker)
            save('phaseMarkers.'+label+'.json', marker)
            return marker

        mark_phase('sample-start')
        modules = owner.snapshot_modules()
        save('modules.json', modules)
        require(modules and 'reshade' not in json.dumps(modules).lower(), 'module/ReShade')

        def command(label, cmd, payload):
            witness()
            value = war3._invoke_internal_test_request(pid, SITE, cmd, payload, timeout_sec=6)
            save(label+'.json', value)
            require(value.get('ok'), 'internal '+cmd)
            return value.get('result', {})

        def capture(label):
            witness()
            value = war3._request_internal_frame_capture(pid, out/(label+'.bmp'), SITE, timeout_sec=10)
            save(label+'.json', value)
            require(value.get('ok'), 'capture '+label)
            with (out/(label+'.bmp')).open('rb') as f:
                header = f.read(54)
            require(header[:2] == b'BM' and struct.unpack_from('<i', header, 18)[0] == 2560
                    and abs(struct.unpack_from('<i', header, 22)[0]) == 1440, 'actual backbuffer dimensions')

        def observe(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                witness()
                time.sleep(.25)

        base = command('camera-start', 'camera.snapshot', {})
        receipt['sampleStartUnix'] = time.time()
        capture('normal-start')
        observe(20)
        mark_phase('pressure-start')
        offsets = [(0, 0), (900, 0), (900, 900), (0, 900), (-900, 0), (0, 0)]
        for index, (x, y) in enumerate(offsets):
            command('camera-low-'+str(index), 'camera.apply', dict(targetX=base['targetX']+x,
                targetY=base['targetY']+y, rotation=base['rotation']+(20 if index%2 else -20),
                angleOfAttack=335, duration=1.5))
            observe(10)
            if index in (2, 5):
                capture('pressure-'+str(index))
        mark_phase('pressure-end')
        command('camera-relief', 'camera.apply', {key: base[key] for key in
                ('targetX', 'targetY', 'rotation', 'angleOfAttack', 'targetDistance')})
        receipt['reliefStartUnix'] = time.time()
        mark_phase('relief-start')
        observe(80)  # > two 60-frame GC intervals if normal cadence is maintained; verify in reports
        mark_phase('relief-end')
        capture('relief-end')
        receipt['sampleEndUnix'] = time.time()
        receipt['routeCompleted'] = True
    except BaseException as exc:
        error = repr(exc)
        save('failure.json', dict(error=error))
    finally:
        owned = pid or war3.STATE.war3_pid
        owner = war3.STATE.retained_native_process
        if owned and owner and owner.pid == owned:
            if owner.poll() is None:
                try:
                    save('exit-command.json', war3._invoke_internal_test_request(owned, SITE,
                        'game.end_for_exit_test', {}, timeout_sec=6))
                except BaseException as exc:
                    save('exit-command-error.json', dict(error=repr(exc)))
                deadline = time.monotonic()+20
                while owner.poll() is None and time.monotonic() < deadline:
                    time.sleep(.25)
            if owner.poll() is None:
                try:
                    save('window-close.json', close_owned_window(launch, owner))
                    deadline = time.monotonic()+15
                    while owner.poll() is None and time.monotonic() < deadline:
                        time.sleep(.25)
                except BaseException as exc:
                    save('window-close-error.json', dict(error=repr(exc)))
            if owner.poll() is None:
                error = error or 'owned process did not exit naturally; no forced termination'
                receipt['residualPid'] = owned
            else:
                # This helper has a forceful branch: call only AFTER exact handle exit.
                try:
                    stop = war3.stop_war3(pid=owned, force=False, avoid_foreground_switch=True)
                    save('settlement.json', stop)
                    receipt['settlementOk'] = bool(stop.get('ok'))
                    if not stop.get('ok'):
                        error = error or 'exact settlement failed'
                except BaseException as exc:
                    error = error or repr(exc)
                    receipt['settlementError'] = repr(exc)
        try:
            if deployed:
                backup = out/'live.dll'
                backup_identity = identity(backup)
                require(backup_identity == before['live'], 'backup changed')
                if not paths['live'].exists():
                    receipt['restoration'] = restore_live_from_backup(
                        paths['live'], backup, before['live'],
                        SITE/('d3d9.dll.staged_restore_'+out.name))
                else:
                    current = identity(paths['live'])
                    if current != before['live']:
                        require(current == before['candidate'], 'restore conflict; refuse overwrite')
                        parked = SITE/('d3d9.dll.parked_'+out.name)
                        receipt['restoration'] = install_guarded(out/'live.dll', paths['live'],
                            before['candidate'], before['live'],
                            SITE/('d3d9.dll.staged_restore_'+out.name), parked)
                        receipt['parkedCandidate'] = str(parked)
            receipt['restoreOk'] = identity(paths['live']) == before['live']
            receipt['protectedUntouched'] = identity(PROTECTED) == protected
            receipt['sourceUnchanged'] = identity(paths['candidate']) == before['candidate']
            receipt['mapUnchanged'] = identity(MAP) == before['map']
            require(all(receipt[k] for k in ('restoreOk', 'protectedUntouched', 'sourceUnchanged', 'mapUnchanged')), 'identity closeout')
            receipt['zeroProcesses'] = zero_processes()
        except BaseException as exc:
            error = error or repr(exc)
            receipt['restoreOrResidualError'] = repr(exc)
        require(not (out/'phaseMarkers.json').exists(), 'phase aggregate already exists')
        save('phaseMarkers.json', phase_markers)
        receipt['phaseMarkers'] = phase_markers
        receipt['newDumps'] = [str(p) for p in SITE.rglob('*.dmp') if str(p) not in old_dumps]
        try:
            receipt['gpuEvents'] = gpu_events_since(start)
        except BaseException as exc:
            error = error or repr(exc)
        receipt['reports'] = []
        for path in logs.glob('*.html'):
            if path.stat().st_mtime_ns <= old_reports.get(path.name, 0):
                continue
            copy_new(path, out/path.name)
            item = dict(path=str(out/path.name), **identity(path))
            try:
                data = read_report(path)
                require(data.get('meta', {}).get('dllSha256', '').upper() == before['candidate']['sha256'], 'report DLL binding')
                require(data.get('meta', {}).get('dllFileSize') == before['candidate']['size'], 'report DLL size')
                require(data.get('meta', {}).get('runtimeProfile') == 'full_default', 'report profile')
                require(data.get('frameCount', 0) > 0, 'report frames')
                item.update(strictRootValid=True, scenarioBinding='owned-process + launch env + new file time; no report scenario field',
                    frameCount=data['frameCount'], meta=data.get('meta'),
                    shadowBudget=data.get('shadowBudgetSummary'))
            except BaseException as exc:
                item.update(strictRootValid=False, error=repr(exc))
                error = error or repr(exc)
            receipt['reports'].append(item)
        if receipt.get('newDumps') or receipt.get('gpuEvents'):
            error = error or 'new GPU event or dump'
        # A successful transaction is never a pressure-recovery pass. Main agent
        # must inspect pressure onset, frame series and final images separately.
        receipt.update(transactionOk=error is None, error=error)
        save('receipt.json', receipt)
        print(json.dumps({k: v for k, v in receipt.items() if k != 'reports'}, ensure_ascii=False, indent=2), flush=True)
    return 0 if error is None else 1


if __name__ == '__main__':
    raise SystemExit(main())
