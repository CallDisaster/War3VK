#!/usr/bin/env python3
"""Two-mode acceptance of the BeforeUi effect executors (x64 migration,
phases 4a and 4b: SSAO, weather snow, FXAA/SMAA, bloom and tone map).

One fixture (test_render_host_d3d9_scene_effects.cpp) issues the same D3D9
draws, scene records and stage markers through
  - the x86 in-process renderer: its own registered SSAO, weather and AA
    passes and its post-processing;
  - the x86 proxy and the x64 host: the shared executors, fed by the wire.
Every frame is read back. The modes must agree byte for byte, the effects must
change the picture, and the UI drawn after BeforeUi must be untouched. The
receipt also accounts x86 memory and frame time per mode; those are numbers,
not thresholds.

The host has one depth format (D32F), so the comparison uses it in both modes.
One more in-process arm repeats the workload on a combined depth+stencil
surface (D24S8): the executors must run and pass the fixture's own checks
there too. Its distance from the D32F reference is recorded, not judged.

Bloom was a D3D9-API effect compiled at run time; phase 4b ported it to a
Vulkan executor in both modes. One more in-process arm runs the earlier D3D9
implementation on the same bloom records (the fixture's legacy oracle): every
frame without bloom must still equal the reference, and the distance of the
bloom frames from the port is recorded, not judged.

No game process, no player DLL, an isolated non-input desktop, one fixture at
a time.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import render_lab_vvl365 as vvl
from run_render_host_validation_gate import check_loader
from render_lab_layer_source import create_empty_source, empty_directory_witness
from run_d3d9_draw_batch_gate import (ROOT, COMPILERS, PROBES, PLAYER, identity, pe_identity, require,
    strict_json, child_settled, execute, sources, IsolatedRenderDesktop, validate_probe_receipt,
    check_launch, json_lines, singleton, ENV_ALLOW, capture_raw_artifacts, verify_raw_artifacts, prefixed)
from run_d3d9_factory_clear_gate import (guarded_identity, loader_file_version, command_ring_rows,
    STREAM_ENV, COMMAND_RING_ENV)

FIXTURE = ROOT/'AutoTest/test_render_host_d3d9_scene_effects.cpp'
FRAMES = ('world', 'ssao', 'snow', 'both', 'both-later', 'off', 'fxaa', 'smaa', 'bloom', 'bloom-tone', 'all')
BLOOM_FRAMES = ('bloom', 'bloom-tone', 'all')
SAMPLES = ('world', 'effects', 'timed', 'closed')
WIDTH = HEIGHT = 64
UI_PIXELS = 12*12
# What the fixture sends (see its Run): 5 draws a frame, 11 hashed and 48
# timed frames; the timed frames with effects carry all four records.
TIMED_FRAMES = 24
DRAWS = 5*(len(FRAMES)+2*TIMED_FRAMES)
REFUSALS = 10
EFFECT_RECORDS = 1+1+1+1+2+2+1+1+1+1+4+4*TIMED_FRAMES
BEFORE_UI_RUNS = 9+TIMED_FRAMES
SSAO_RUNS = SNOW_RUNS = 4+TIMED_FRAMES  # ssao|snow, both, both-later, all
AA_RUNS = BLOOM_RUNS = 3+TIMED_FRAMES   # fxaa|bloom, smaa|bloom-tone, all
INSERTIONS_REFUSED = 3
# With WARVK_D3D9_ASYNC_SCENE=1 (--ring-env) the stream+ring arm publishes stage markers without
# waiting: the three markers only the host can refuse (they are INSERTIONS_REFUSED) are not sent.
ASYNC_SCENE_SKIPS = 3
# SsaoStage / SnowStage::CommandsRecorded, AaStage::SmaaRecorded,
# BloomStage::CommandsRecorded: the last run is a timed frame with all four.
SSAO_RECORDED, SNOW_RECORDED, AA_RECORDED, BLOOM_RECORDED = 8, 14, 7, 6
ANNOTATE_ENV = dict(WARVK_D3D9_ANNOTATIONS='1')
REFERENCE_ENV = dict(DXVK_WAR3_SCENE_EFFECTS_FIXTURE='1')
LEGACY_BLOOM_ENV = dict(DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM='1')
# label, in-process reference, stream profile, command ring, validated under VVL,
# combined depth+stencil surface (in-process only), legacy D3D9 bloom (in-process only).
# The last arm repeats the stream arm without the validation layer: its times
# are the ones to read; the layer makes the others slow.
ARMS = (('effects-reference', True, False, False, False, False, False),
        ('effects-reference-depth-stencil', True, False, False, False, True, False),
        ('effects-reference-legacy-bloom', True, False, False, False, False, True),
        ('effects-remote', False, False, False, True, False, False),
        ('effects-remote-stream', False, True, False, True, False, False),
        ('effects-remote-stream-ring', False, True, True, True, False, False),
        ('effects-remote-stream-plain', False, True, False, False, False, False))
DEPTH_STENCIL = 'effects-reference-depth-stencil'
LEGACY_BLOOM = 'effects-reference-legacy-bloom'


def image_difference(a, b):
    """Differing pixels and the largest channel difference of two BGRA images."""
    pixels = sum(a[i:i+4] != b[i:i+4] for i in range(0, len(a), 4))
    return dict(pixels=pixels, maxChannel=max((abs(x-y) for x, y in zip(a, b)), default=0))


def remote_environment(pairs):
    """--remote-env NAME=VALUE: x64 switch variables for the remote arms only."""
    result = {}
    for pair in pairs:
        name, separator, value = pair.partition('=')
        if not separator or not name.startswith('WARVK_D3D9_') or not value:
            raise ValueError('--remote-env takes WARVK_D3D9_NAME=VALUE')
        result[name] = value
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-receipt', type=Path, required=True, help='x64 host build receipt')
    ap.add_argument('--reference-dll', type=Path, required=True, help='fresh isolated x86 product candidate')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--validation', action='store_true', help='pinned VVL365 on the validated remote arms')
    ap.add_argument('--repin-loader', action='store_true', help='with --validation: accept the current system vulkan-1.dll')
    ap.add_argument('--remote-env', action='append', default=[], metavar='WARVK_D3D9_NAME=VALUE',
                    help='extra variable for the remote (proxy and host) arms only, e.g. a switch turned off for a control round')
    ap.add_argument('--ring-env', action='append', default=[], metavar='WARVK_D3D9_NAME=VALUE',
                    help='extra variable for the remote arms that run the stream profile over the command ring only '
                         '(the ordered asynchronous switches are refused anywhere else)')
    args = ap.parse_args()
    out = args.output.resolve()
    if args.repin_loader and not args.validation:
        ap.error('--repin-loader needs --validation')
    reference_dll = args.reference_dll.resolve()
    require(reference_dll.is_relative_to(ROOT) and reference_dll.suffix.lower() == '.dll', 'isolated reference DLL below repository')
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh output only')
    out.mkdir()
    receipt = dict(schema='warvk-x64-scene-effects-gate/v1', ok=False, gameTouched=False,
                   fullRendererMigrated=False, steps=[], cases=[], raw=[], copies=[])
    remote_extra = remote_environment(args.remote_env)
    receipt['remoteEnvironment'] = remote_extra
    ring_extra = remote_environment(args.ring_env)
    receipt['ringEnvironment'] = ring_extra
    frozen = []
    env = {k: v for k, v in os.environ.items() if k.upper() in ENV_ALLOW}
    env.update(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0')

    def step(argv, label):
        try:
            receipt['steps'].append(execute(argv, ROOT, out/label, 180, env))
        finally:
            receipt['raw'].append(capture_raw_artifacts(out, [label+'.stdout', label+'.stderr']))

    def copy(a, b):
        before = identity(a)
        shutil.copy2(a, b)
        after = identity(b)
        require((before['bytes'], before['sha256']) == (after['bytes'], after['sha256']), 'exact copy')
        receipt['copies'].append(after)

    try:
        build = strict_json(args.build_receipt.read_text('utf-8'))
        require(build['ok'] and build['sourcesPreserved'], 'frozen host build')
        require(all(identity(Path(r['path'])) == r for r in build['sources']), 'host source drift')
        host = Path(build['host']['path'])
        require(identity(host) == build['host'], 'host artifact drift')
        frozen = [identity(p) for p in sources()]
        frozen += [guarded_identity(p) for p in (Path(__file__).resolve(), FIXTURE,
            ROOT/'AutoTest/test_render_host_d3d9_parent.cpp', ROOT/'src/d3d9/war3/effects/war3_effect_scene.h',
            PLAYER, reference_dll, host, PROBES/'desktop-probe-32.exe', PROBES/'desktop-probe-64.exe')]
        receipt['referenceDll'] = identity(reference_dll)
        receipt['host'] = build['host']
        frozen += [identity(Path(m.__file__).resolve()) for m in list(sys.modules.values())
                   if getattr(m, '__file__', None) and Path(m.__file__).suffix == '.py'
                   and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest')]
        receipt['validationEnabled'] = args.validation
        if args.validation:
            pinned = ROOT/'AutoTest/artifacts/x64-dispatch-vvl365-candidate-20261023-r1/receipt.json'
            layer = strict_json(pinned.read_text('utf-8'))['layer']
            if args.repin_loader:
                current = identity(Path(layer['loader']['path']))
                receipt['loaderRepin'] = dict(pinned=layer['loader'], current=current, changed=current != layer['loader'],
                                              version=loader_file_version(Path(current['path'])))
                layer = dict(layer, loader=current)
            for kind in ('manifest', 'library', 'loader'):
                require(identity(Path(layer[kind]['path'])) == layer[kind], 'pinned VVL identity '+kind)
                frozen.append(layer[kind])
            frozen.append(identity(pinned))
            receipt['layer'] = layer
            manifest = Path(layer['manifest']['path'])
            library = Path(layer['library']['path'])
            loader = Path(layer['loader']['path'])
            loader_version = receipt['loaderRepin']['version'] if args.repin_loader else None
            vvl.check_manifest(strict_json(manifest.read_text('utf-8')))
            receipt['implicitSourceBefore'] = create_empty_source(out)
            empty = Path(receipt['implicitSourceBefore']['path'])
        receipt['inputs'] = frozen.copy()

        cc = COMPILERS/'i686-w64-mingw32-clang++.exe'
        common = [ROOT/'tools/render_host'/p for p in ('protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp', 'owned_child.cpp')]
        flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread', '-D_WIN32_WINNT=0x0a00']
        proxy = out/'proxy.dll'
        fixture = out/'scene-effects32.exe'
        step([cc, *flags, '-Wno-unused-parameter', '-Wno-microsoft-exception-spec', '-shared', '-Wl,--kill-at', '-DNOMINMAX',
              ROOT/'tools/render_host/d3d9_proxy.cpp', ROOT/'tools/render_host/d3d9_proxy.def', *common,
              ROOT/'tools/render_host/process_memory_probe.cpp', '-luser32', '-lbcrypt', '-ladvapi32', '-luuid', '-lpsapi',
              '-o', proxy], 'build-proxy')
        step([cc, *flags, '-municode', FIXTURE, ROOT/'tools/render_host/process_memory_probe.cpp',
              '-luser32', '-lpsapi', '-o', fixture], 'build-fixture')
        require(pe_identity(proxy) == pe_identity(fixture) == (0x14c, 0x10b), 'x86 COM caller')
        frozen += [identity(proxy), identity(fixture)]

        images = {}
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([PROBES/'desktop-probe-32.exe', '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', PROBES/'desktop-probe-64.exe', '--child-log', out/'probe-child.log'], cwd=out,
                stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['probe'] = probe
            receipt['raw'].append(capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log']))
            check_launch(probe, desktop.desktop_name, desktop.input_before)
            rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
            require(len(rows) == 2, 'both probes')
            validate_probe_receipt(rows[0], station='WinSta0', desktop=desktop.desktop_name,
                                   input_desktop=desktop.input_before, bits=32, child=rows[1], launch=probe)

            for label, reference, stream, ring, validated, depth_stencil, legacy_bloom in ARMS:
                folder = out/label
                folder.mkdir()
                exe = folder/'fixture.exe'
                dll = folder/'renderer.dll'
                copy(fixture, exe)
                copy(reference_dll if reference else proxy, dll)
                config = folder/'pipeline.conf'
                config.write_text('', encoding='utf-8')
                frozen.append(identity(config))
                local = env | dict(DXVK_LOG_PATH=str(folder), DXVK_CONFIG_FILE=str(config))
                if reference:
                    local.update(REFERENCE_ENV)
                    if legacy_bloom:
                        local.update(LEGACY_BLOOM_ENV)
                else:
                    copy(host, folder/'host.exe')
                    local.update(ANNOTATE_ENV, WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),
                                 WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                    local.update(remote_extra)
                if stream:
                    local.update(STREAM_ENV)
                if ring:
                    local.update(COMMAND_RING_ENV)
                    if stream and not reference:
                        local.update(ring_extra)
                validate = args.validation and validated
                if validate:
                    local.update(vvl.validation_env(folder, manifest, empty))
                case = dict(label=label, ok=False, reference=reference, stream=stream, commandRing=ring, validated=validate,
                            depth='d24s8' if depth_stencil else 'd32f', legacyBloom=legacy_bloom)
                receipt['cases'].append(case)
                case['process'] = desktop.run([exe, '--dll', dll, '--scenario', 'normal']+(['--reference'] if reference else [])
                    +(['--depth-stencil'] if depth_stencil else []),
                    cwd=folder, stdout_path=folder/'case.stdout', stderr_path=folder/'case.stderr', timeout_s=180, env=local)
                frame_files = [f'frame-{name}.bgra' for name in FRAMES]
                present = [name for name in frame_files if (folder/name).is_file()]
                receipt['raw'].append(capture_raw_artifacts(folder, ['case.stdout', 'case.stderr']+present
                    +([] if reference else ['host.log'])+(['validation.raw.log'] if validate else []), True))
                check_launch(case['process'], desktop.desktop_name, desktop.input_before)
                rows = json_lines(folder/'case.stdout')
                report = singleton(rows, 'warvk-d3d9-scene-effects-fixture/v1')
                require(report['ok'] and report['normalTeardown'] and report['memoryMeasured'] is True
                        and report['reference'] is reference and report['depth'] == case['depth']
                        and report['gameTouched'] is False
                        and report['x86VulkanAbsent'] is (not reference), 'complete scene effects fixture')
                skipped = ASYNC_SCENE_SKIPS if (not reference and stream and ring
                                                and ring_extra.get('WARVK_D3D9_ASYNC_SCENE') == '1') else 0
                case['asyncSceneSkips'] = skipped
                require(report['draws'] == DRAWS and report['refusals'] == REFUSALS-skipped
                        and report['refusalsSkipped'] == skipped
                        and report['timedFrames'] == TIMED_FRAMES, 'fixture coverage')
                require(tuple(f['name'] for f in report['frames']) == FRAMES
                        and all(f['uiPixels'] == UI_PIXELS for f in report['frames']), 'every frame, UI untouched')
                changed = {f['name']: f['changed'] for f in report['frames']}
                require(changed['world'] == changed['off'] == 0
                        and all(changed[name] > 0 for name in FRAMES if name not in ('world', 'off')),
                        'effects change the world, absence does not')
                require(tuple(s['name'] for s in report['samples']) == SAMPLES
                        and all(s['memory']['valid'] for s in report['samples']), 'complete x86 memory samples')
                require(present == frame_files, 'every frame image written')
                images[label] = {name: (folder/f'frame-{name}.bgra').read_bytes() for name in FRAMES}
                require(all(len(data) == WIDTH*HEIGHT*4 for data in images[label].values()), 'frame image extent')
                case['result'] = report
                if not reference:
                    pr = singleton(rows, 'warvk-d3d9-proxy/v1')
                    hs = singleton(json_lines(folder/'host.log'), 'warvk-d3d9-host/v1')
                    case['host'] = hs
                    case['proxy'] = pr
                    require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                            and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit'] == 0, 'healthy remote shutdown')
                    require(hs['ok'] and hs['bits'] == 64 and hs['normalTeardown'] and hs['liveResources'] == 0
                            and hs['submitted'] == hs['gpuCompleted'] == hs['retired'] and hs['draws'] == DRAWS,
                            'host ownership closed')
                    require(pr['exchanges'] == hs['accepted']+hs['rejected'], 'ordered command balance')
                    case['commandRing'] = command_ring_rows(prefixed(folder/'case.stderr', 'D3D9 command-ring-proxy: '),
                        prefixed(folder/'case.stderr', 'D3D9 terminal: '), prefixed(folder/'host.log', 'D3D9 command-ring-host: '),
                        ring, pr, hs)
                    fx = singleton(prefixed(folder/'host.log', 'D3D9 scene-effects-host: '), 'warvk-d3d9-scene-effects-host/v1')
                    require(fx['effectRecords'] == EFFECT_RECORDS and fx['beforeUiRuns'] == BEFORE_UI_RUNS
                            and fx['ssaoRecorded'] == SSAO_RUNS and fx['snowRecorded'] == SNOW_RUNS
                            and fx['aaRecorded'] == AA_RUNS and fx['bloomRecorded'] == BLOOM_RUNS
                            and fx['insertionsRefused'] == INSERTIONS_REFUSED-skipped
                            and fx['lastSsaoStage'] == SSAO_RECORDED and fx['lastSnowStage'] == SNOW_RECORDED
                            and fx['lastAaStage'] == AA_RECORDED and fx['lastBloomStage'] == BLOOM_RECORDED
                            and fx['resourceBytes'] > 0 and fx['beforeUiNs'] > 0, 'host executor account')
                    case['effectsHost'] = fx
                    case['childSettlement'] = child_settled(pr['childPid'], pr['childCreation'])
                    ds = singleton(json_lines(folder/'host.log'), 'warvk-d3d9-desktop/v1')
                    require(ds['pid'] == pr['childPid'] and ds['peerPid'] == case['process']['pid']
                            and ds['desktop'] == desktop.desktop_name and ds['inputDesktop'] == desktop.input_before, 'exact host/desktop')
                    if validate:
                        case['validation'] = vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                        case['loader'] = check_loader((folder/'host.log').read_text('utf-8'),
                            (folder/'host_warvk-d3d9-host.log').read_text('utf-8'), library, empty, loader, loader_version)
                memory = {s['name']: s['memory'] for s in report['samples']}
                keys = ('privateBytes', 'usedVirtualBytes', 'privateVirtualBytes')
                case['x86'] = dict(
                    world={k: memory['world'][k] for k in keys},
                    effectsDelta={k: memory['effects'][k]-memory['world'][k] for k in keys},
                    timedDelta={k: memory['timed'][k]-memory['world'][k] for k in keys},
                    peakPrivateBytes=memory['timed']['peakPrivateBytes'])
                case['frameTime'] = dict(worldUs=report['worldFrameUs'], effectsUs=report['effectsFrameUs'],
                    deltaUs=round(report['effectsFrameUs']-report['worldFrameUs'], 1), underValidation=validate)
                if not reference:
                    fx = case['effectsHost']
                    case['frameTime']['hostExecutorUsPerRun'] = round(fx['beforeUiNs']/fx['beforeUiRuns']/1000, 1)
                case['ok'] = True
            if args.validation:
                receipt['implicitSourceAfter'] = empty_directory_witness(empty)
                require(receipt['implicitSourceAfter'] == receipt['implicitSourceBefore'], 'private layer source drift')

        # The two modes on the same input: byte for byte, frame by frame.
        base = images['effects-reference']
        comparison = {}
        for label, frames in images.items():
            if label in ('effects-reference', DEPTH_STENCIL, LEGACY_BLOOM):
                continue
            comparison[label] = {name: image_difference(base[name], frames[name]) for name in FRAMES}
        # Another depth format is another input; how far it lands is a number.
        receipt['depthStencilDistance'] = {name: image_difference(base[name], images[DEPTH_STENCIL][name]) for name in FRAMES}
        # The legacy oracle replaces bloom only: every other frame is the reference.
        legacy = {name: image_difference(base[name], images[LEGACY_BLOOM][name]) for name in FRAMES}
        receipt['legacyBloomDistance'] = {name: legacy[name] for name in BLOOM_FRAMES}
        require(all(not legacy[name]['pixels'] for name in FRAMES if name not in BLOOM_FRAMES),
                'the legacy bloom arm differs outside bloom: '+json.dumps(legacy))
        receipt['comparison'] = comparison
        receipt['frameHashes'] = {c['label']: {f['name']: f['hash'] for f in c['result']['frames']} for c in receipt['cases']}
        different = {label: {name: d for name, d in frames.items() if d['pixels']} for label, frames in comparison.items()}
        different = {label: frames for label, frames in different.items() if frames}
        require(not different, 'in-process and x64 host images differ: '+json.dumps(different))
        receipt['modesEqual'] = True
        receipt['validatedSessions'] = sum(1 for c in receipt['cases'] if c['validated'])
        receipt['accounting'] = {c['label']: dict(x86=c['x86'], frameTime=c['frameTime'],
            hostResourceBytes=c.get('effectsHost', {}).get('resourceBytes')) for c in receipt['cases']}
        require(receipt['desktopSafety']['ok'], 'isolated cleanup')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            receipt['inputsPreserved'] = bool(frozen) and all(guarded_identity(Path(r['path'])) == r for r in frozen)
            require(receipt['inputsPreserved'], 'input drift')
            require(all(identity(Path(r['path'])) == r for r in receipt['copies']), 'case copy drift')
            for raw in receipt['raw']:
                verify_raw_artifacts(raw)
            receipt['rawPreserved'] = True
        except Exception as error:
            receipt['ok'] = False
            receipt['preservationError'] = str(error)
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({k: receipt.get(k) for k in ('ok', 'error', 'preservationError', 'inputsPreserved', 'modesEqual')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
