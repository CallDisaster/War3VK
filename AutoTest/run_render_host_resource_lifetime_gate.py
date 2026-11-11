"""R1/Q1 actual x86 COM/raw C2 lifetime peers -> existing x64 DXVK.

No budget/timeout expansion, product integration, game launch or deployment.
Run only under the main agent's build/GPU lease. Every GPU caller runs on a
measured separate Win32 desktop after the real x86/x64 CPU desktop probe.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, child_settled
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import sources, json_lines, native_uint
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests

FIXTURE = 'warvk-d3d9-resource-lifetime/v1'
REJECTION = 'warvk-d3d9-lifetime-rejection/v1'
PROXY = 'warvk-d3d9-proxy/v1'
HOST = 'warvk-d3d9-host/v1'
DESKTOP = 'warvk-d3d9-desktop/v1'
INVALID = 0x8876086c
OOM = 0x8876017c


def expected_rejections():
    """Independent finite population; missing/extra/reordered rows fail."""
    result = []
    def add(kind, slot, op, resource_id=0, generation=0, expected=INVALID, pixel=True):
        result.append(dict(kind=kind, slot=slot, op=op, id=resource_id,
                           generation=generation, expected=expected, pixelRecovery=pixel))
    for i in range(3): add('stale-upload', i, 5 if i == 2 else 3, i + 1, 1)
    for i in range(6): add('stale-destroy', i, 13, i + 1, 1)
    for kind in ('stale-draw', 'wrong-type-draw', 'future-draw'):
        for i in range(6): add(kind, i, 9)
    for i in range(3): add('future-upload', i, 5 if i == 2 else 3, i + 1, 3)
    for i in range(6): add('future-destroy', i, 13, i + 1, 3)
    for i in range(6): add('wrong-type-destroy', i, 13, i + 1, 2)
    add('wrong-type-upload', 0, 5, 1, 2)
    add('wrong-type-upload', 2, 3, 3, 2)
    add('bad-create-retry', 0, 2, 8, 1)
    add('typed-slots-full', 0, 2, 14, 1, expected=OOM)
    add('future-create', 0, 2, 14, 3)
    add('duplicate-live-create', 0, 2, 1, 2)
    for kind, op, resource_id, generation in (
        ('query-old-revision', 12, 7, 1), ('query-future-revision', 12, 7, 1),
        ('query-wrong-serial', 12, 7, 1), ('query-duplicate-issue', 11, 7, 1),
        ('query-gap-issue', 11, 7, 1), ('query-wrong-type-poll', 12, 1, 2),
        ('query-wrong-type-issue', 11, 1, 2), ('query-wrong-type-destroy', 13, 7, 1),
        ('query-old-key-poll', 12, 7, 1), ('query-old-key-issue', 11, 7, 1),
        ('query-old-key-destroy', 13, 7, 1), ('query-future-key-issue', 11, 7, 3),
        ('query-future-key-poll', 12, 7, 3)):
        add(kind, 0, op, resource_id, generation, pixel=False)
    require(len(result) == 61, 'checker population bug')
    return result


def uints(record, expected):
    for key, value in expected.items():
        require(native_uint(record[key]) and record[key] == value, f'exact integer {key}')


def read_records(reports, host_reports, mode, host_desktop):
    require(type(reports) is list and type(host_reports) is list, 'record streams must be lists')
    rows = {}; events = []; hosts = {}
    for row in reports:
        require(type(row) is dict and type(row.get('schema')) is str, 'parent schema')
        schema = row['schema']
        if schema == REJECTION:
            events.append(row)
        else:
            require(schema in (FIXTURE, PROXY) and schema not in rows, 'unexpected/duplicate/conflicting parent summary')
            rows[schema] = row
    require(set(rows) == {FIXTURE, PROXY}, 'missing parent summary')
    for row in host_reports:
        require(type(row) is dict and type(row.get('schema')) is str, 'host schema')
        schema = row['schema']
        require(schema in ({HOST, DESKTOP} if host_desktop else {HOST}) and schema not in hosts,
                'unexpected/duplicate/conflicting host summary')
        hosts[schema] = row
    require(set(hosts) == ({HOST, DESKTOP} if host_desktop else {HOST}), 'missing host summary')
    expected = expected_rejections() if mode == 'raw' else []
    require(len(events) == len(expected), 'negative-event population')
    for index, (row, wanted) in enumerate(zip(events, expected), 1):
        require(set(row) == {'schema', 'index', 'kind', 'slot', 'op', 'id', 'generation',
                            'expected', 'actual', 'transportHealthy', 'pixelRecovery'}, 'negative-event field set')
        uints(row, dict(index=index, slot=wanted['slot'], op=wanted['op'], id=wanted['id'],
                        generation=wanted['generation'], expected=wanted['expected'], actual=wanted['expected']))
        require(row['kind'] == wanted['kind'] and row['transportHealthy'] is True
                and row['pixelRecovery'] is wanted['pixelRecovery'], 'negative identity/recovery')
    return rows[FIXTURE], rows[PROXY], hosts[HOST], hosts.get(DESKTOP)


def check_case(reports, host_reports, launch, mode, desktop, input_desktop, host_desktop=True):
    require(mode in ('com', 'raw') and type(host_desktop) is bool, 'checker scope')
    check_launch(launch, desktop, input_desktop)
    f, proxy, host, ds = read_records(reports, host_reports, mode, host_desktop)
    require(f['ok'] is True and f['mode'] == mode, 'fixture did not complete')
    require(f['publicD3D9Api'] is (mode == 'com') and f['rawProtocolPeer'] is (mode == 'raw'), 'actual interface scope')
    require(f['resourceReleaseScope'] == ('public-COM-references-zero-not-host-destruction' if mode == 'com'
            else 'confirmed-remote-destroy-replies'), 'public Release is not remote destruction')
    for key in ('fixtureCleanupComplete', 'parentVulkanAbsent'):
        require(f[key] is True, 'fixture lifetime/process boundary ' + key)
    for row in (f, proxy, host):
        for key in ('productIntegrated', 'memorySavingsProven'):
            require(row[key] is False, 'unsupported product claim')
    for key in ('globalInputUsed', 'desktopSwitched', 'gameTouched', 'windowPixelsVerified', 'playerVisualAccepted'):
        require(f[key] is False, 'unsupported fixture scope ' + key)
    require(f['desktop'] == desktop and f['inputBefore'] == f['inputAfter'] == input_desktop
            and desktop != input_desktop, 'actual fixture desktop')
    common = dict(bits=32, pixelFailures=0)
    if mode == 'com':
        exact = dict(rounds=96, draws=96, readbacks=96, pixelsChecked=96*3328,
                     resourcesCreated=576, resourcesReleased=576, rejectionChecks=0, pixelRecoveries=0,
                     privateBoundDraws=96, finalPrivateBoundResources=6,
                     queryIssues=256, queryPolls=256, queryRecoveries=0, sameSerialChecks=0,
                     firstSerial=0, lastSerial=0, typedSlotsFilled=0, sameGenerationRetries=0)
        # Each COM texture now uploads through Begin + Chunk + Commit instead
        # of legacy UploadTexture. Two CPU-only protocol operations per round;
        # actual GPU uploads, submission and retirement counts stay unchanged.
        # Sampler setters coalesce to one state message before the first draw;
        # all 96 rounds reuse the same state. This is not one RPC per setter.
        host_exact = dict(accepted=2244 + 96*2 + 1, rejected=0, backendCalls=2242 + 1, draws=96, clears=96, readbacks=96,
                          submitted=673, gpuCompleted=673, retired=673,
                          uploads=288, uploadedBytes=96*176, buffersCreated=192, texturesCreated=96,
                          shadersCompiled=192, declarationsCreated=96, queries=256,
                          logicalHandlesCreated=577, logicalHandlesDestroyed=577, logicalHandlesReused=570,
                          resourcesReused=570, queryHandlesCreated=1, queryHandlesDestroyed=1,
                          queryHandlesReused=0, queryIssues=256, queryPolls=256)
    else:
        exact = dict(rounds=0, draws=51, readbacks=51, pixelsChecked=51*3328,
                     resourcesCreated=21, resourcesReleased=21, rejectionChecks=61, pixelRecoveries=48,
                     privateBoundDraws=0, finalPrivateBoundResources=0,
                     queryIssues=257, queryPolls=270, queryRecoveries=13, sameSerialChecks=257,
                     typedSlotsFilled=8, sameGenerationRetries=2, firstSerial=159, lastSerial=159)
        require(native_uint(f['firstSerial']) and f['firstSerial'] > 0
                and native_uint(f['lastSerial']) and f['lastSerial'] == f['firstSerial'],
                'reissued query serial must remain the same without GPU commands')
        host_exact = dict(accepted=734, rejected=61, backendCalls=730, draws=51, clears=51, readbacks=51,
                          submitted=162, gpuCompleted=162, retired=162,
                          uploads=6, uploadedBytes=352, buffersCreated=13, texturesCreated=2,
                          shadersCompiled=4, declarationsCreated=2, queries=257,
                          logicalHandlesCreated=23, logicalHandlesDestroyed=23, logicalHandlesReused=9,
                          resourcesReused=8, queryHandlesCreated=2, queryHandlesDestroyed=2,
                          queryHandlesReused=1, queryIssues=257, queryPolls=270)
    uints(f, common | exact)
    require(native_uint(f['maxChannelError'], 32) and f['maxChannelError'] <= 1, 'pixel tolerance')
    for key in ('peerVerified', 'x86VulkanAbsent', 'childNaturalExit', 'childProcessSettled'):
        require(proxy[key] is True, 'proxy settlement ' + key)
    for key in ('transportFailed', 'deviceLost', 'remoteTerminal', 'childSelfFailStop'):
        require(proxy[key] is False, 'proxy failed ' + key)
    uints(proxy, dict(childExit=0, exchanges=host_exact['accepted'] + host_exact['rejected']))
    for key in ('childPid', 'childCreation', 'primaryExitWaitMs'):
        require(native_uint(proxy[key]), 'proxy native integer ' + key)
    require(0 < proxy['childPid'] <= 0xffffffff and proxy['childCreation'] > 0
            and proxy['primaryExitWaitMs'] <= 5000, 'exact child identity/primary deadline')
    for key in ('ok', 'actualDxvkCore', 'actualDxso', 'normalTeardown', 'gpuInitialized', 'shutdownQueueDrained'):
        require(host[key] is True, 'real GPU lifetime ' + key)
    for key in ('gameTouched', 'presentImplemented'):
        require(host[key] is False, 'offscreen-only scope ' + key)
    uints(host, dict(bits=64, liveResources=0, liveLogicalBytes=0, liveLogicalHandles=0, liveQueries=0,
                     presentAttempts=0, acquiredImages=0, presentQueued=0, presentApiSucceeded=0,
                     presentGpuCompleted=0, presentRetired=0, presentedReadbacks=0, presentedFrame=0,
                     windowProcessId=0, windowThreadId=0) | host_exact)
    for key in ('submitted', 'gpuCompleted', 'retired', 'peakLogicalBytes', 'vendorId', 'deviceId'):
        require(native_uint(host[key]) and host[key] > 0, 'GPU measurement ' + key)
    require(host['submitted'] == host['gpuCompleted'] == host['retired'], 'GPU retirement mismatch')
    if host_desktop:
        require(ds['beforeGpu'] is True and ds['desktop'] == desktop and ds['inputDesktop'] == input_desktop
                and ds['windowStation'] == 'WinSta0', 'actual offscreen host desktop')
        uints(ds, dict(pid=proxy['childPid'], peerPid=launch['pid'], windowThreadId=0))
        require(native_uint(ds['threadId'], 32) and ds['threadId'] > 0, 'host thread identity')


def mutations(reports, host_reports, launch, mode, desktop, input_desktop, host_desktop):
    """Only deep-copy actual passing records; no native/GPU reruns."""
    args = (mode, desktop, input_desktop, host_desktop)
    check_case(reports, host_reports, launch, *args)
    tested = killed = duplicates = conflicts = 0
    def attempt(parents, hosts, process):
        nonlocal tested, killed
        tested += 1
        try: check_case(parents, hosts, process, *args)
        except (ValueError, KeyError, TypeError): killed += 1
    for stream, records in enumerate((reports, host_reports)):
        for row in records:
            for conflict in (False, True):
                lists = copy.deepcopy([reports, host_reports]); extra = copy.deepcopy(row)
                if conflict:
                    if extra['schema'] == REJECTION: extra['actual'] = 0
                    else:
                        field = next((key for key in ('ok', 'transportFailed', 'beforeGpu') if type(extra.get(key)) is bool), None)
                        require(field is not None, 'no actual typed verdict for conflict mutation')
                        extra[field] = not extra[field]
                    conflicts += 1
                else: duplicates += 1
                lists[stream].append(extra); attempt(*lists, copy.deepcopy(launch))
    for stream, records in enumerate((reports, host_reports)):
        for index, row in enumerate(records):
            fields = ('actual', 'index', 'pixelRecovery') if row['schema'] == REJECTION else tuple(
                k for k in ('ok', 'rounds', 'resourcesReleased', 'privateBoundDraws', 'finalPrivateBoundResources',
                            'resourceReleaseScope', 'queryIssues', 'sameSerialChecks', 'pixelsChecked',
                            'pixelFailures', 'liveLogicalHandles', 'liveQueries', 'logicalHandlesDestroyed',
                            'resourcesReused', 'queryHandlesReused', 'retired', 'childNaturalExit', 'childExit',
                            'shutdownQueueDrained', 'inputAfter', 'peerPid') if k in row)
            for field in fields:
                lists = copy.deepcopy([reports, host_reports]); value = row[field]
                lists[stream][index][field] = (not value) if type(value) is bool else value + 1 if type(value) is int else value + '-changed'
                attempt(*lists, copy.deepcopy(launch))
    for field in ('jobEmpty', 'processSettled', 'forcedTermination', 'timedOut', 'inputAfter'):
        process = copy.deepcopy(launch); value = process[field]
        process[field] = not value if type(value) is bool else value + '-changed'
        attempt(copy.deepcopy(reports), copy.deepcopy(host_reports), process)
    for field, value in (('actualExecutable', 'wrong-image.exe'), ('jobActiveProcesses', 1),
                         ('jobActiveProcesses', False)):
        process = copy.deepcopy(launch); process[field] = value
        attempt(copy.deepcopy(reports), copy.deepcopy(host_reports), process)
    require(tested > 0 and tested == killed, 'lifetime reader mutation survived')
    return dict(tested=tested, killed=killed, duplicates=duplicates, conflicts=conflicts,
                source='actual-passing-runtime-records', gpuRerun=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('toolchain', 'build', 'reference-dll', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--restricted-medium', action='store_true',
                    help='opt-in restricted medium-integrity children; never changes global security')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='R1_Q1_ACTUAL_X86_COM_RAW_TO_X64_DXVK', ok=False,
                   gameTouched=False, playerDllChanged=None, productIntegrated=False, memorySavingsProven=False,
                   windowPixelsVerified=False, hostDesktopIndependentlyObserved=False,
                   steps=[], cases=[], caseCopies=[], stepRawArtifacts=[], rawArtifactsPreserved=False,
                   restrictedMediumRequested=args.restricted_medium)
    def source_set():
        return set(sources()) | {ROOT/'AutoTest'/name for name in (
            'test_render_host_d3d9_resource_lifetime.cpp', 'run_render_host_resource_lifetime_gate.py',
            'run_render_host_presenter_gate.py', 'run_render_host_protocol_gate.py',
            'run_render_host_transport_gate.py', 'run_render_host_core_gate.py',
            'isolated_render_desktop.py', 'test_isolated_render_desktop_contract.py',
            'render_lab_token.py', 'test_render_lab_token.py',
            'render_lab_default_dacl.py', 'test_render_lab_default_dacl.py',
            'render_lab_raw_artifacts.py', 'test_render_lab_raw_artifacts.py',
            'test_render_host_desktop_probe.cpp')}
    def copy_artifact(source, target):
        original = identity(source); shutil.copy2(source, target); copied = identity(target)
        require(all(original[k] == copied[k] for k in ('bytes', 'sha256')), 'exact copy differs')
        receipt['caseCopies'].append(dict(source=original, copy=copied))
    try:
        tc = args.toolchain.resolve(strict=True); build = args.build.resolve(strict=True)
        reference = args.reference_dll.resolve(strict=True); player = Path('E:/Work/Warcraft III/d3d9.dll')
        receipt['referenceBefore'] = identity(reference); receipt['playerBefore'] = identity(player)
        paths = source_set(); before = [identity(p) for p in sorted(paths)]; receipt['sources'] = before
        boundary = check_host_compile_boundary(build); receipt['allowedSharedD3D9Inputs'] = boundary
        cc = {bits: tc/(arch + '-w64-mingw32-clang++.exe') for bits, arch in ((32, 'i686'), (64, 'x86_64'))}
        receipt['compilers'] = [identity(p) for p in cc.values()]
        env = {k: v for k, v in os.environ.items() if k.upper() in {
            'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA',
            'PROGRAMFILES', 'PROGRAMFILES(X86)', 'PROGRAMDATA', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'}}
        env.update(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0')
        receipt['steps'].append(execute([sys.executable, '-B', ROOT/'AutoTest/test_isolated_render_desktop_contract.py'],
                                       ROOT, out/'desktop-reader-tests', 30, env))
        common = [ROOT/'tools/render_host'/name for name in ('protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp', 'owned_child.cpp')]
        probes = {}; cpu = []
        for bits in (32, 64):
            probes[bits] = out/f'desktop-probe-{bits}.exe'
            receipt['steps'].append(execute([cc[bits], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                '-municode', '-D_WIN32_WINNT=0x0a00', ROOT/'AutoTest/test_render_host_desktop_probe.cpp', *common,
                '-luser32', '-lbcrypt', '-ladvapi32', '-o', probes[bits]], ROOT, out/f'probe-build-{bits}', 180, env))
            for name in ('resource_registry', 'wire'):
                exe = out/f'{name}-{bits}.exe'; extra = [ROOT/'tools/render_host/protocol.cpp'] if name == 'wire' else []
                receipt['steps'].append(execute([cc[bits], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                    ROOT/f'AutoTest/test_render_host_d3d9_{name}.cpp', *extra, '-o', exe], ROOT, out/f'{name}-build-{bits}', 180, env))
                cpu.append((name, bits, exe))
        target = 'tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe'; host = build/target
        receipt['steps'].append(execute(['ninja', '-C', build, '-j2', target], ROOT, out/'host-build', 600, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-n', target], ROOT, out/'host-no-work', 30, env))
        require('no work to do' in (out/'host-no-work.stdout').read_text('utf-8'), 'host not frozen')
        require(boundary == check_host_compile_boundary(build), 'host compile boundary changed')
        receipt['compileCommands'] = identity(build/'compile_commands.json')
        proxy = out/'warvk-d3d9-proxy-dev.dll'; fixture = out/'d3d9-resource-lifetime32.exe'
        receipt['steps'].append(execute([cc[32], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-microsoft-exception-spec', '-static', '-pthread', '-shared',
            '-Wl,--kill-at', '-DNOMINMAX', '-D_WIN32_WINNT=0x0a00', ROOT/'tools/render_host/d3d9_proxy.cpp',
            ROOT/'tools/render_host/d3d9_proxy.def', *common, ROOT/'tools/render_host/process_memory_probe.cpp',
            '-luser32', '-lbcrypt', '-ladvapi32', '-luuid', '-lpsapi', '-o', proxy],
            ROOT, out/'proxy-build', 240, env))
        receipt['steps'].append(execute([cc[32], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
            '-pthread', '-municode', '-D_WIN32_WINNT=0x0a00', ROOT/'AutoTest/test_render_host_d3d9_resource_lifetime.cpp',
            *common, '-luser32', '-lbcrypt', '-ladvapi32', '-o', fixture], ROOT, out/'fixture-build', 180, env))
        require(pe_identity(proxy) == pe_identity(fixture) == pe_identity(reference) == (0x14c, 0x10b), 'real x86 inputs')
        require(pe_identity(host) == (0x8664, 0x20b), 'real x64 host')
        for bits in (32, 64):
            require(pe_identity(probes[bits]) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'probe architecture')
        receipt['binaries'] = [identity(p) for p in (proxy, fixture, host, *probes.values(), *(row[2] for row in cpu))]
        with IsolatedRenderDesktop(restricted_medium=args.restricted_medium) as desktop:
            receipt['desktopName'] = desktop.desktop_name; receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name, '--expect-station', 'WinSta0',
                '--input-desktop', desktop.input_before, '--child', probes[64], '--child-log', out/'desktop-child.log'],
                cwd=out, stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['desktopProbe'] = probe
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'desktop-child.log'])
            require(probe['ok'] is True, 'CPU desktop probe failed')
            rows = json_lines(out/'probe.stdout') + json_lines(out/'desktop-child.log')
            require(len(rows) == 2, 'actual CPU desktop probe rows'); receipt['probeReports'] = rows
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station='WinSta0', bits=32, child=rows[1], launch=probe)
            for name, bits, exe in cpu:
                row = desktop.run([exe], cwd=out, stdout_path=out/f'{name}-{bits}.stdout',
                    stderr_path=out/f'{name}-{bits}.stderr', timeout_s=30, env=env)
                receipt['stepRawArtifacts'].append(capture_raw_artifacts(out, [f'{name}-{bits}.stdout', f'{name}-{bits}.stderr']))
                receipt['steps'].append(row); check_launch(row, desktop.desktop_name, desktop.input_before)
                text = (out/f'{name}-{bits}.stdout').read_text('utf-8')
                require(re.search(rf'checks=\d+ failures=0 bits={bits}\b', text) is not None, 'CPU registry/wire did not complete')
            for mode in ('com', 'raw'):
                directory = out/mode; directory.mkdir(); case = dict(mode=mode, ok=False); receipt['cases'].append(case)
                local_fixture = directory/fixture.name; local_proxy = directory/'proxy.dll'; local_host = directory/host.name
                copy_artifact(fixture, local_fixture); copy_artifact(proxy, local_proxy); copy_artifact(host, local_host)
                local = dict(env, WARVK_D3D9_HOST_EXE=str(local_host), WARVK_D3D9_HOST_LOG=str(directory/'host.log'))
                launch = desktop.run([local_fixture, '--mode', mode, '--dll', local_proxy,
                    '--expect-desktop', desktop.desktop_name, '--input-desktop', desktop.input_before],
                    cwd=directory, stdout_path=directory/'case.stdout', stderr_path=directory/'case.stderr', timeout_s=45, env=local)
                case['process'] = launch
                case['initialRawArtifacts'] = capture_raw_artifacts(directory, ['case.stdout', 'case.stderr', 'host.log'], True)
                require(launch['ok'] is True, 'native lifetime case failed: ' + mode)
                reports = json_lines(directory/'case.stdout'); hosts = json_lines(directory/'host.log')
                case['reports'] = reports; case['hostReports'] = hosts
                check_case(reports, hosts, launch, mode, desktop.desktop_name, desktop.input_before, True)
                case['readerMutations'] = mutations(reports, hosts, launch, mode, desktop.desktop_name,
                                                    desktop.input_before, True)
                _, p, _, _ = read_records(reports, hosts, mode, True)
                case['childSettlement'] = child_settled(p['childPid'], p['childCreation']); case['ok'] = True
        require(receipt['desktopSafety']['ok'] is True, 'desktop close/recovery failure')
        require(receipt['compileCommands'] == identity(build/'compile_commands.json'), 'compile database changed')
        require(receipt['binaries'] == [identity(Path(row['path'])) for row in receipt['binaries']], 'binaries changed')
        require(all(row['copy'] == identity(Path(row['copy']['path'])) for row in receipt['caseCopies']), 'executed copies changed')
        require(all(case['ok'] for case in receipt['cases']) and len(receipt['cases']) == 2, 'case completion population')
        receipt['hostDesktopIndependentlyObserved'] = True
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            if 'referenceBefore' in receipt:
                receipt['referenceAfter'] = identity(Path(receipt['referenceBefore']['path']))
                receipt['playerAfter'] = identity(Path(receipt['playerBefore']['path']))
                receipt['playerDllChanged'] = receipt['playerBefore'] != receipt['playerAfter']
                require(receipt['referenceBefore'] == receipt['referenceAfter'] and not receipt['playerDllChanged'], 'reference/player changed')
            if 'sources' in receipt:
                receipt['sourcesPreserved'] = paths == source_set() and before == [identity(p) for p in sorted(paths)]
                require(receipt['sourcesPreserved'], 'sources changed during gate')
            receipt['sourceAndReferencePreserved'] = receipt.get('sourcesPreserved', False) and receipt.get('playerDllChanged') is False
            captured = list(raw_manifests(receipt))
            for manifest in captured: verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(captured)
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'error', 'scope', 'sourceAndReferencePreserved')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
