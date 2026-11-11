"""Copy-TU A/B: same x64 host/owned window, only the actual surface HWND differs.

Fixed peer/host/host/peer order; no retries. Root-only build/native lease.
Original peer validation, renderer workload, 5s transport/exit and 45s outer
deadline stay unchanged. B is NOT presentation to the x86 client window.
This diagnostic never grants migration/Presenter/product acceptance.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import sys

import diagnose_render_host_initialization as init
from diagnose_render_host_presenter_exit import ExitMonitor, prove_host
from run_render_host_protocol_gate import ROOT, identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_presenter_gate import check_case_records, check_soak, check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, raw_manifests, verify_raw_artifacts
from render_lab_palette_watcher import FrozenImage, PE_PATH, PDB_PATH

HEADER = ROOT/'AutoTest/render_lab_owned_surface_target.h'
PREFIX = 'WARVK_OWNED_TARGET_V1 '
ARMS = ('peer', 'host', 'host', 'peer')
EVENTS = ('ready', 'surface-created', 'surface-retired', 'closed')


def instrument_source(original, header=HEADER):
    require(type(original) is str and 'diagnosticTarget' not in original and '\r' not in original,
            'one normalized uninstrumented backend required')
    edits = []
    def replace(text, anchor, replacement):
        require(text.count(anchor) == 1, 'exact owned-target anchor: '+anchor[:90])
        edits.append(dict(anchor=anchor, replacement=replacement))
        return text.replace(anchor, replacement, 1)
    source = replace(original, '#include "d3d9_backend.h"\n',
        '#include "d3d9_backend.h"\n#include "'+header.resolve().as_posix()+'"\n')
    source = replace(source, '  HWND window = nullptr;\n',
        '  diagnostic::OwnedSurfaceTarget diagnosticTarget;\n  HWND window = nullptr;\n')
    source = replace(source, '      m_stats.windowProcessId = windowProcessId;\n',
        '      diagnosticTargetStart(owner, window, windowProcessId, windowThreadId);\n'
        '      m_stats.windowProcessId = windowProcessId;\n')
    # Direct member call, no new global lifetime/entry point.
    source = source.replace('diagnosticTargetStart(owner, window, windowProcessId, windowThreadId)',
        'owner.diagnosticTarget.start(window, windowProcessId, windowThreadId, owner.windowMarker)')
    edits[-1]['replacement'] = edits[-1]['replacement'].replace(
        'diagnosticTargetStart(owner, window, windowProcessId, windowThreadId)',
        'owner.diagnosticTarget.start(window, windowProcessId, windowThreadId, owner.windowMarker)')
    anchor = '          return wsi::createSurface(ownerPtr->window, vki->getLoaderProc(), vki->instance(), surface);\n'
    source = replace(source, anchor,
        '          const HWND actualTarget = ownerPtr->diagnosticTarget.select(ownerPtr->window);\n'
        '          const auto result = wsi::createSurface(actualTarget, vki->getLoaderProc(), vki->instance(), surface);\n'
        '          ownerPtr->diagnosticTarget.surfaceCreated(actualTarget, static_cast<int>(result));\n'
        '          return result;\n')
    source = replace(source, '    presenter = nullptr;\n    blitter = nullptr;\n',
        '    presenter = nullptr;\n    diagnosticTarget.closeAfterSurfaceRetired();\n    blitter = nullptr;\n')
    # Original validateWindow() remains unchanged: it continues to reject peer
    # marker/destruction drift even in the host-target arm. Target validated at
    # surface creation and owned close; this fixed experiment does not mutate it.
    source, timing_edits = init.instrument_source(source)
    return source, edits, timing_edits


def read_target(text, mode, launch, witness, proxy):
    require(mode in ('peer', 'host'), 'exact arm required')
    rows = []
    for line in text.splitlines():
        if 'WARVK_OWNED_TARGET_V1' not in line:
            continue
        require(line.startswith(PREFIX), 'malformed owned-target prefix')
        rows.append(strict_json(line[len(PREFIX):]))
    require(len(rows) == 4, 'missing/extra owned-target events')
    stable = ('mode', 'pid', 'creation100ns', 'mainTid', 'peerHwnd', 'peerPid', 'peerTid',
        'peerMarker', 'ownedHwnd', 'ownedTid', 'ownedThreadCreation100ns', 'ownedMarker',
        'surfaceHwnd', 'surfacePid', 'surfaceTid', 'desktop', 'inputDesktop')
    numbers32 = ('sequence', 'pid', 'mainTid', 'peerPid', 'peerTid', 'ownedTid', 'surfacePid',
                 'surfaceTid', 'surfaceCalls', 'pulses', 'messages', 'destroyed', 'ncDestroyed', 'failure')
    numbers64 = ('creation100ns', 'peerHwnd', 'ownedHwnd', 'ownedThreadCreation100ns',
                 'surfaceHwnd', 'surfaceHinstance', 'closeMs')
    for i, row in enumerate(rows):
        require(type(row) is dict and row['event'] == EVENTS[i] and row['sequence'] == i+1,
                'owned-target event order')
        require(all(native_uint(row[k], 32) for k in numbers32)
                and all(native_uint(row[k], 64) for k in numbers64), 'owned-target native numeric fields')
        require(all(row[k] == rows[0][k] for k in stable), 'owned-target identity drift')
        require(row['mode'] == mode and row['pid'] == witness['pid'] == proxy['childPid']
                and row['creation100ns'] == proxy['childCreation'] and row['mainTid'] == witness['threadId'],
                'owned target not exact authenticated child')
        require(row['peerPid'] == launch['pid'] == witness['peerPid']
                and row['peerTid'] == witness['windowThreadId'] and row['pid'] != row['peerPid'],
                'original peer identity not preserved')
        require(row['peerHwnd'] > 0 and row['ownedHwnd'] > 0 and row['ownedHwnd'] != row['peerHwnd']
                and row['ownedTid'] > 0 and row['ownedTid'] not in (row['mainTid'], row['peerTid'])
                and row['ownedThreadCreation100ns'] >= row['creation100ns'] > 0,
                'owned target lacks distinct lifetime')
        require(type(row['peerMarker']) is str and row['peerMarker'].startswith('WarVK-D9C1-')
                and len(row['peerMarker']) == 43 and row['ownedMarker'] == row['peerMarker']+'-diagnostic-owned',
                'session-derived independent target marker')
        prefix = 'owned' if mode == 'host' else 'peer'
        require(row['surfaceHwnd'] == row[prefix+'Hwnd']
                and row['surfacePid'] == (row['pid'] if mode == 'host' else row['peerPid'])
                and row['surfaceTid'] == row[prefix+'Tid'], 'actual surface target differs from arm')
        require(row['desktop'] == witness['desktop'] == launch['desktop']
                and row['inputDesktop'] == witness['inputDesktop'] == launch['inputBefore']
                and row['desktop'] != row['inputDesktop'], 'target non-input Desktop')
        require(row['failure'] == 0 and row['surfaceCalls'] == (0 if i == 0 else 1)
                and row['pulses'] == (1 if i < 2 else 2), 'target creation/pump population')
        require(row['surfaceHinstance'] == 0 if i == 0 else row['surfaceHinstance'] > 0,
                'actual surface HINSTANCE observation')
        require(row['destroyed'] == row['ncDestroyed'] == int(i == 3)
                and row['classUnregistered'] is (i == 3), 'owned window destruction evidence')
        require(row['messages'] > 0 and (i == 3 or row['closeMs'] == 0), 'target pump/close timing')
        if i > 1:
            require(row['surfaceHinstance'] == rows[1]['surfaceHinstance'], 'surface HINSTANCE changed')
        if i:
            require(row['messages'] >= rows[i-1]['messages'], 'target pump count regressed')
    require(rows[-1]['closeMs'] < 1000, 'owned close exceeded its bounded wait')
    return dict(rows=rows, targetLifecycleComplete=True, peerWindowPresented=mode == 'peer',
                pixelDomain='actual-selected-swapchain-acquired-image-not-window', acceptancePassed=False)


def check_monitor_proxy(binding, proxy, witness):
    require(type(proxy['childPid']) is int and type(proxy['childCreation']) is int
            and proxy['childPid'] == binding['host']['pid'] == witness['pid']
            and proxy['childCreation'] == binding['host']['creation100ns']
            and witness == binding['nativeDesktopWitness'], 'proxy/monitor/host final identity disagreement')


def run_case(old, original, host, out, index, mode, desktop, palette, receipt, frozen):
    directory = out/f'{index+1}-{mode}'; directory.mkdir()
    row = dict(index=index+1, mode=mode, acceptancePassed=False, workloadPassed=False,
               diagnosticCollected=False); receipt['cases'].append(row)
    def copy(source, target):
        before = identity(source); require(not target.exists(), 'fresh case binary')
        shutil.copy2(source, target); after = identity(target)
        require(all(before[k] == after[k] for k in ('bytes', 'sha256')), 'case copy differs')
        receipt['copies'].append(dict(source=before, copy=after)); frozen.extend((before, after))
        return target
    argv = original['process']['argv']
    parent = copy(Path(argv[0]), directory/'d3d9-presenter-fault32.exe')
    proxy = copy(Path(argv[2]), directory/'proxy.dll')
    local_host = copy(host, directory/'warvk-d3d9-host-dev.exe')
    env = init.recipe_environment(dict(os.environ), directory, local_host)
    env['WARVK_DIAGNOSTIC_SURFACE_OWNER'] = mode
    row['actualSafeEnvironment'] = env
    args = init.replay_argv(argv, parent, proxy, desktop.desktop_name, desktop.input_before)
    monitor = ExitMonitor(desktop, parent, local_host, directory, case_name='r8-clear', palette_image=palette)
    monitor.start()
    try:
        row['process'] = desktop.run(args, cwd=directory, stdout_path=directory/'case.stdout',
            stderr_path=directory/'case.stderr', timeout_s=45, env=env)
    finally:
        monitor.stop_event.set(); monitor.join(2)
        row['monitor'] = monitor.result
        row['initialRawArtifacts'] = capture_raw_artifacts(directory,
            init.RAW_NAMES+[p.name for p in directory.glob('exit-snapshot-*.json')], True)
    require(not monitor.is_alive(), 'diagnostic monitor did not stop')
    init.check_diagnostic_settlement(row['process'], desktop.desktop_name, desktop.input_before)
    try:
        binding = monitor.result['binding']
        prove_host(row['process'], binding['nativeDesktopWitness'], binding['parent'], binding['host'],
                   parent, local_host, desktop.desktop_name, desktop.input_before, binding['actualParentPid'])
        require(not monitor.result['errors'], 'read-only monitor errors')
        monitor.result['identityCertified'] = True
        row['childSettlement'] = child_settled(binding['host']['pid'], binding['host']['creation100ns'])
        row['diagnosticCollected'] = True
    except Exception as error:
        row['monitorError'] = str(error)
    text = (directory/'host.log').read_text('utf-8')
    hosts = json_lines(directory/'host.log'); reports = json_lines(directory/'case.stdout')
    row['hostReports'] = hosts; row['reports'] = reports
    try:
        a, b = check_case_records(reports, hosts, 'warvk-d3d9-presenter-clear-soak/v1',
                                 'warvk-d3d9-host/v1', True)
        proxy_row = a['warvk-d3d9-proxy/v1']; witness = b['warvk-d3d9-desktop/v1']
        check_monitor_proxy(monitor.result['binding'], proxy_row, witness)
        check_soak(a['warvk-d3d9-presenter-clear-soak/v1'], proxy_row, b['warvk-d3d9-host/v1'],
                   witness, row['process'], desktop.desktop_name, desktop.input_before)
        row['target'] = read_target(text, mode, row['process'], witness, proxy_row)
        row['stages'] = init.read_bound_stages(text, hosts, reports, row['process'],
                                              desktop.desktop_name, desktop.input_before)
        row['workloadPassed'] = True
    except Exception as error:
        row['workloadError'] = str(error)  # A failed arm is evidence; not retried/replaced.
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--execute-native', action='store_true')
    args = ap.parse_args(); out = args.output.resolve()
    require(out.is_relative_to(ROOT/'AutoTest/artifacts') and out != ROOT/'AutoTest/artifacts', 'fresh artifacts subdirectory')
    out.mkdir(exist_ok=False)
    receipt = dict(schema=1, scope='COPY_TU_SURFACE_OWNERSHIP_ABBA_DIAGNOSTIC', acceptancePassed=False,
        gameTouched=False, productIntegrated=False, memorySavingsProven=False, systemChanged=False,
        originalTransportMilliseconds=5000, originalExitMilliseconds=5000, outerFixtureSeconds=45,
        rootBuildNativeLeaseRequired=True, priority='BelowNormal', parallelism=1, arms=list(ARMS),
        sameExeRequired=True, prepared=False, diagnosticComplete=False, steps=[], stepRawArtifacts=[], copies=[], cases=[])
    frozen = []; own = []
    try:
        old, case, build, original_rows = init.bind_baseline(); frozen.extend(original_rows)
        palette = FrozenImage.load(); receipt['paletteFrozenImage'] = palette.receipt()
        frozen.extend(identity(p) for p in (PE_PATH, PDB_PATH, HEADER, Path(__file__),
            Path(__file__).with_name('test_render_host_surface_owner.py')))
        frozen.extend(identity(Path(m.__file__).resolve()) for m in list(sys.modules.values())
            if getattr(m, '__file__', None) and Path(m.__file__).suffix == '.py'
            and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        raw = init.BACKEND.read_bytes(); original = raw.decode('utf-8').replace('\r\n', '\n')
        changed, edits, timing = instrument_source(original)
        source_dir = out/'substituted'; source_dir.mkdir(); source = source_dir/'d3d9_backend.cpp'
        # Mechanical copy-TU generation; original source/object/archive untouched.
        source.write_bytes(changed.replace('\n', '\r\n').encode('utf-8') if b'\r\n' in raw else changed.encode('utf-8'))
        delta = ''.join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                                           fromfile=str(init.BACKEND), tofile=str(source)))
        (out/'instrumentation.diff').write_text(delta, encoding='utf-8', newline='')
        own.extend((identity(source), identity(out/'instrumentation.diff')))
        receipt.update(originalSource=identity(init.BACKEND), diagnosticSource=identity(source),
                       edits=edits, timingEdits=timing, prepared=True)
        if args.execute_native:
            env = init.recipe_environment(dict(os.environ), out, out/'warvk-d3d9-host-dev.exe')
            receipt['buildSafeEnvironment'] = env
            host = init.build_diagnostic(old, build, source, out, env, receipt, frozen)
            own.extend((receipt['diagnosticObject'], receipt['diagnosticHost']))
            binaries = {Path(r['path']).name: Path(r['path']) for r in old['binaries']}
            with IsolatedRenderDesktop(restricted_medium=True) as desktop:
                receipt['desktopSafety'] = desktop.receipt
                probe_env = {k: v for k, v in env.items() if k in init.ENV_ALLOW or k in ('DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE')}
                try:
                    receipt['probe'] = desktop.run([binaries['desktop-probe-32.exe'], '--expect-desktop', desktop.desktop_name,
                        '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                        '--child', binaries['desktop-probe-64.exe'], '--child-log', out/'probe-child.log'], cwd=out,
                        stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=probe_env)
                finally:
                    receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log'], True)
                check_launch(receipt['probe'], desktop.desktop_name, desktop.input_before)
                rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
                require(len(rows) == 2, 'both bitness Desktop probes')
                validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                    station='WinSta0', bits=32, child=rows[1], launch=receipt['probe'])
                for i, mode in enumerate(ARMS):
                    run_case(old, case, host, out, i, mode, desktop, palette, receipt, frozen)
            safety = receipt['desktopSafety']
            require(safety['desktopClosed'] is True and safety['cleanupErrors'] == []
                    and safety['inputAfterClose'] == safety['inputBefore'], 'Desktop cleanup incomplete')
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['frozenInputs'] = frozen
        try:
            for row in frozen+own: init.copied.same_file(row)
            receipt['frozenInputsPreserved'] = bool(frozen)
        except Exception as error:
            receipt['preservationError'] = str(error)
        try:
            manifests = list(raw_manifests(receipt))
            for manifest in manifests: verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(manifests)
        except Exception as error:
            receipt['rawPreservationError'] = str(error)
        receipt['preparationComplete'] = bool(receipt['prepared'] and receipt.get('frozenInputsPreserved')
            and not any(k in receipt for k in ('error', 'preservationError')))
        receipt['diagnosticComplete'] = bool(receipt['preparationComplete'] and receipt.get('rawArtifactsPreserved')
            and len(receipt['cases']) == 4 and all(c['diagnosticCollected'] for c in receipt['cases'])
            and 'rawPreservationError' not in receipt)
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: receipt.get(k) for k in ('acceptancePassed', 'prepared', 'diagnosticComplete',
        'error', 'preservationError', 'rawPreservationError')}))
    return 0 if (receipt['diagnosticComplete'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    sys.exit(main())
