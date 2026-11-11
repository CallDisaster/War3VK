"""Three isolated public-D3D9 shader pixel runs; root-owned native/build lease.

Builds ONLY the new x86 fixture. Reuses complete passing current M0 frozen
proxy/host/CPU probes. Old/new shipping DLLs and player files are read-only.
Each case has its own restricted-Medium, atomic-Job, non-input Desktop and a
real x86->x64 CPU probe before the GPU fixture. No default-Desktop fallback,
driver control overrides, new shader toggles, wire changes or exit relaxation.
"""
import argparse
from functools import lru_cache
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys

import run_render_host_memory_gate as memory
import run_render_host_memory_destroy_mutation as baseline_reader
from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_core_gate import execute
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts

FRAME = 'warvk-d3d9-shader-cache-equivalence-frame/v1'
SUMMARY = 'warvk-d3d9-shader-cache-equivalence/v1'
PROXY, HOST, DESKTOP = memory.PROXY, memory.HOST, memory.DESKTOP
SCOPE = 'SHARED_DXSO_ADAPTER_PIXEL_EQUIVALENCE_V1'
LABELS = ('old-reference', 'new-reference', 'remote')
PHASES = ('original-a', 'same-token-distinct-live-objects', 'original-a-restored',
    'recreated-while-peer-live', 'recreated-after-all-public-shaders-released',
    'only-immediate-constant-bytes-changed', 'original-constant-bytes-restored',
    'changed-token-distinct-live-objects', 'original-recreated-after-changed-shaders-destroyed')
FRAME_FIELDS = ('schema', 'cycle', 'phase', 'ordinal', 'expectedBGRA', 'pixelsChecked',
    'inner', 'clear', 'failures', 'maxChannelError', 'sampledPixelFnv', 'sameColorStable')
SUMMARY_FIELDS = ('schema', 'ok', 'bits', 'reference', 'adapter', 'stage', 'error', 'publicD3D9Api',
    'cycles', 'phasesPerCycle', 'frames', 'redFrames', 'greenFrames', 'pixelsChecked', 'pixelFailures',
    'maxChannelError', 'draws', 'vertexCreates', 'pixelCreates', 'vertexPublicZero', 'pixelPublicZero',
    'shaderByteRoundtrips', 'simultaneousDistinctCOMPairs', 'allPublicShaderRefsReleased',
    'queryIssues', 'queryPolls', 'desktopChecks', 'vertexTokenFnv', 'redTokenFnv', 'greenTokenFnv',
    'redSampledPixelFnv', 'greenSampledPixelFnv', 'signatureConsumedByDraw', 'definedConstantOutputVerified',
    'needsConstantCopiesCovered', 'needsConstantCopiesReason', 'cacheReuseCertifiedByFixture', 'cacheIdentityEvidence',
    'normalTeardown', 'deviceFinalRefs', 'factoryFinalRefs', 'resourceHeldDevice', 'deviceReleaseMs',
    'deviceReleaseLimitMs', 'parentVulkanAbsent', 'remoteChildExitVerifiedByFixture', 'windowOwnerClosed',
    'windowClassUnregistered', 'desktop', 'inputBefore', 'inputAfter', 'globalInputUsed', 'desktopSwitched',
    'gameTouched', 'productIntegrated', 'memorySavingsProven', 'windowPixelsVerified', 'playerVisualAccepted')
PROXY_FIELDS = ('schema', 'childPid', 'childCreation', 'childExit', 'exchanges', 'peerVerified',
    'transportFailed', 'x86VulkanAbsent', 'deviceLost', 'remoteTerminal', 'childNaturalExit',
    'childProcessSettled', 'childSelfFailStop', 'primaryExitWaitMs', 'productIntegrated', 'memorySavingsProven')
HOST_COUNTS = dict(bits=64, rejected=0, submitted=221, gpuCompleted=221, retired=221,
    draws=72, clears=72, uploads=3, uploadedBytes=176, readbacks=72, queries=72,
    buffersCreated=2, texturesCreated=1, shadersCompiled=88, declarationsCreated=1,
    liveResources=0, liveLogicalBytes=0, peakLogicalBytes=376, logicalHandlesCreated=93,
    logicalHandlesDestroyed=93, logicalHandlesReused=84, resourcesReused=84,
    queryHandlesCreated=1, queryHandlesDestroyed=1, queryHandlesReused=0, queryIssues=72,
    liveLogicalHandles=0, liveQueries=0, presentAttempts=0, acquiredImages=0, presentQueued=0,
    presentApiSucceeded=0, presentGpuCompleted=0, presentRetired=0, presentedReadbacks=0,
    presentedFrame=0, windowProcessId=0, windowThreadId=0, lastAcquireVkResult=1, lastPresentVkResult=1)


def fields(row, names, label):
    memory.fields(row, names, label)


def exact(row, expected):
    for key, value in expected.items():
        require(type(row[key]) is type(value) and row[key] == value, 'exact field ' + key)


def fnv(data):
    result = 14695981039346656037
    for value in data:
        result = ((result ^ value) * 1099511628211) & ((1 << 64) - 1)
    return f'{result:016x}'


@lru_cache(maxsize=1)
def token_hashes():
    # Literal original SM3 words, independent of compiled shader/cache records.
    vertex = (0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000,
        0x0200001f, 0x80000000, 0xe00f0000, 0x02000001, 0xe00f0000, 0x90e40000, 0xffff)
    red = (0xffff0300, 0x05000051, 0xa00f0000, 0x3f800000, 0, 0, 0x3f800000,
        0x02000001, 0x800f0800, 0xa0e40000, 0xffff)
    green = red[:3] + (0, 0x3f800000) + red[5:]
    return tuple(fnv(struct.pack('<11I', *words)) for words in (vertex, red, green))


@lru_cache(maxsize=2)
def pixel_hash(green):
    # 64x64 quad with fixed, independent edge guards. Solid 0/255 DEF output
    # and literal clear bytes are exactly representable, not a captured golden.
    sample = bytearray()
    for y in range(64):
        for x in range(64):
            clear = x <= 5 or x >= 58 or y <= 5 or y >= 58
            inner = 10 <= x <= 53 and 10 <= y <= 53
            if clear or inner:
                sample.extend(struct.pack('<I', 0xff13273b if clear else 0xff00ff00 if green else 0xffff0000))
    require(len(sample) == 3328 * 4, 'independent pixel hash population')
    return fnv(sample)


def check_medium(launch):
    require(not launch.get('error'), 'launch exception cannot become passing evidence')
    token = launch['restrictedToken']
    require(type(token) is dict, 'missing exact restricted-Medium token receipt')
    exact(token, dict(schema=1, mode='restricted-medium-per-child/v1', ready=True,
        parentTokenUnchanged=True, derivedTokenClosed=True, childTokenVerifiedBeforeResume=True,
        noHighIntegrityFallback=True, resumedAfterVerification=True, resumePreviousCount=1,
        globalSecurityChanged=False))
    require(token['cleanupErrors'] == [], 'derived token cleanup failed')


def records(reports, hosts, reference):
    require(type(reports) is list and type(hosts) is list and type(reference) is bool, 'record stream types')
    require(len(reports) == (73 if reference else 74), 'exact parent record population')
    frames = reports[:72]
    require(all(type(row) is dict and row.get('schema') == FRAME for row in frames),
        'all ordered frame observations precede terminal summaries')
    summary = reports[-1]
    require(type(summary) is dict and summary.get('schema') == SUMMARY, 'exact final fixture summary')
    if reference:
        require(hosts == [], 'reference cannot carry remote-host records')
        return frames, summary, None, None, None
    proxy = reports[-2]
    require(type(proxy) is dict and proxy.get('schema') == PROXY, 'proxy settlement must precede fixture summary')
    require(len(hosts) == 2 and type(hosts[0]) is dict and hosts[0].get('schema') == DESKTOP
        and type(hosts[1]) is dict and hosts[1].get('schema') == HOST, 'host desktop then unique terminal summary')
    return frames, summary, proxy, hosts[1], hosts[0]


def check_case(reports, hosts, launch, desktop, initial, reference):
    check_launch(launch, desktop, initial)
    check_medium(launch)
    frame_rows, f, proxy, host, ds = records(reports, hosts, reference)
    fields(f, SUMMARY_FIELDS, 'shader equivalence summary')
    exact(f, dict(schema=SUMMARY, ok=True, bits=32, reference=reference,
        adapter='shipping-d3d9' if reference else 'x64-development-host', stage='complete', error='',
        publicD3D9Api=True, cycles=8, phasesPerCycle=9, frames=72, redFrames=56, greenFrames=16,
        pixelsChecked=239616, pixelFailures=0, draws=72, vertexCreates=32, pixelCreates=56,
        vertexPublicZero=32, pixelPublicZero=56, shaderByteRoundtrips=88, simultaneousDistinctCOMPairs=24,
        allPublicShaderRefsReleased=8, queryIssues=72, desktopChecks=77, signatureConsumedByDraw=True,
        definedConstantOutputVerified=True, needsConstantCopiesCovered=False,
        needsConstantCopiesReason='relative-operands-not-admitted-by-current-host-profile',
        cacheReuseCertifiedByFixture=False, cacheIdentityEvidence='separate-host-cache-observation-required',
        normalTeardown=True, deviceFinalRefs=0, factoryFinalRefs=0, resourceHeldDevice=True,
        deviceReleaseLimitMs=5000, remoteChildExitVerifiedByFixture=False,
        windowOwnerClosed=True, windowClassUnregistered=True, desktop=desktop, inputBefore=initial, inputAfter=initial,
        globalInputUsed=False, desktopSwitched=False, gameTouched=False, productIntegrated=False,
        memorySavingsProven=False, windowPixelsVerified=False, playerVisualAccepted=False))
    require(native_uint(f['deviceReleaseMs']) and f['deviceReleaseMs'] <= 5000, 'unchanged final Release deadline')
    require(native_uint(f['queryPolls'], 32) and f['queryPolls'] >= 72, 'all actual EVENT polls observed')
    require(type(f['parentVulkanAbsent']) is bool and (reference or f['parentVulkanAbsent']), 'x86 Vulkan boundary')
    require(native_uint(f['maxChannelError'], 32) and f['maxChannelError'] <= 1, 'original pixel channel tolerance')
    expected_tokens = token_hashes()
    for key, value in zip(('vertexTokenFnv', 'redTokenFnv', 'greenTokenFnv'), expected_tokens):
        exact(f, {key: value})
    expected_hashes = {False: pixel_hash(False), True: pixel_hash(True)}
    exact(f, dict(redSampledPixelFnv=expected_hashes[False], greenSampledPixelFnv=expected_hashes[True]))
    maximum = 0
    for at, row in enumerate(frame_rows):
        fields(row, FRAME_FIELDS, 'frame')
        phase = at % 9; green = phase in (5, 7)
        exact(row, dict(schema=FRAME, cycle=at // 9 + 1, phase=PHASES[phase], ordinal=at + 1,
            expectedBGRA=0xff00ff00 if green else 0xffff0000, pixelsChecked=3328, inner=1936, clear=1392,
            failures=0, sampledPixelFnv=expected_hashes[green], sameColorStable=True))
        require(native_uint(row['maxChannelError'], 32) and row['maxChannelError'] <= 1, 'frame pixel tolerance')
        maximum = max(maximum, row['maxChannelError'])
    require(f['maxChannelError'] == maximum, 'summary/frame channel error mismatch')
    remote = None
    if not reference:
        fields(proxy, PROXY_FIELDS, 'proxy settlement')
        exact(proxy, dict(schema=PROXY, childExit=0, peerVerified=True, transportFailed=False,
            x86VulkanAbsent=True, deviceLost=False, remoteTerminal=False, childNaturalExit=True,
            childProcessSettled=True, childSelfFailStop=False, productIntegrated=False, memorySavingsProven=False))
        for name, bits in (('childPid', 32), ('childCreation', 64)):
            require(native_uint(proxy[name], bits) and proxy[name] > 0, 'exact remote child identity ' + name)
        require(native_uint(proxy['primaryExitWaitMs']) and proxy['primaryExitWaitMs'] <= 5000,
            'remote child failed unchanged primary five-second exit gate')
        counts = HOST_COUNTS | dict(queryPolls=f['queryPolls'], accepted=479 + f['queryPolls'],
            backendCalls=477 + f['queryPolls'])
        host_flags = dict(schema=HOST, ok=True, actualDxvkCore=True, actualDxso=True, normalTeardown=True,
            gpuInitialized=True, shutdownQueueDrained=True, gameTouched=False, presentImplemented=False,
            productIntegrated=False, memorySavingsProven=False)
        fields(host, (*counts, *host_flags, 'vendorId', 'deviceId'), 'host terminal')
        exact(host, counts | host_flags)
        for name in ('vendorId', 'deviceId'):
            require(native_uint(host[name], 32) and host[name] > 0, 'actual GPU identity ' + name)
        exact(proxy, dict(exchanges=counts['accepted']))
        fields(ds, ('schema', 'pid', 'threadId', 'peerPid', 'windowThreadId', 'windowStation',
            'desktop', 'inputDesktop', 'beforeGpu'), 'host desktop')
        exact(ds, dict(schema=DESKTOP, pid=proxy['childPid'], peerPid=launch['pid'], windowThreadId=0,
            windowStation='WinSta0', desktop=desktop, inputDesktop=initial, beforeGpu=True))
        require(native_uint(ds['threadId'], 32) and ds['threadId'] > 0 and ds['pid'] != launch['pid'],
            'distinct actual x86/x64 process identity')
        remote = dict(childPid=proxy['childPid'], childCreation=proxy['childCreation'], childExit=0,
            childPrimaryExitWaitMs=proxy['primaryExitWaitMs'], vendorId=host['vendorId'], deviceId=host['deviceId'])
    return dict(ok=True, reference=reference, frames=72, pixelsChecked=239616,
        redSampledPixelFnv=f['redSampledPixelFnv'], greenSampledPixelFnv=f['greenSampledPixelFnv'],
        vertexTokenFnv=f['vertexTokenFnv'], redTokenFnv=f['redTokenFnv'], greenTokenFnv=f['greenTokenFnv'],
        deviceReleaseMs=f['deviceReleaseMs'], deviceReleaseLimitMs=5000, queryPolls=f['queryPolls'],
        parentVulkanAbsent=f['parentVulkanAbsent'], remote=remote, cacheReuseCertified=False,
        gameTouched=False, productIntegrated=False, memorySavingsProven=False, playerVisualAccepted=False)


def check_desktop_closed(safety, probe, launch, desktop, initial):
    require(type(safety) is dict, 'Desktop closure receipt required')
    exact(safety, dict(schema=1, scope='NON_INPUT_DESKTOP_LAUNCH_CONTEXT', ok=True, desktop=desktop,
        desktopCreated=True, desktopClosed=True, creatorExited=True, desktopHandleInheritable=False,
        desktopSwitchRequested=False, restrictedMediumRequested=True, globalInputUsed=False, gameTouched=False,
        windowStation='WinSta0', desktopPath='WinSta0\\' + desktop))
    require(safety['cleanupErrors'] == [] and safety['launches'] == [probe, launch], 'exact probe/fixture Desktop launches')
    require(type(desktop) is str and desktop and type(initial) is str and initial
        and desktop.casefold() != initial.casefold() and not safety.get('error')
        and not safety.get('bodyError'), 'non-input Desktop with no concealed body/cleanup error')
    for name in ('inputBefore', 'inputAfterCreate', 'inputBeforeLaunch', 'inputAfterLaunch', 'inputAfterRun', 'inputAfterClose'):
        exact(safety, {name: initial})
    for row in (probe, launch):
        check_launch(row, desktop, initial); check_medium(row)


def check_probe(rows, probe, launch, desktop, initial):
    require(type(rows) is list and len(rows) == 2, 'exact independently observed x86/x64 CPU probe pair')
    validate_probe_receipt(rows[0], desktop=desktop, input_desktop=initial,
        station='WinSta0', bits=32, child=rows[1], launch=probe)
    require(rows[0]['main']['threadId'] == probe['initialThreadId']
        and (probe['pid'], probe['creation100ns']) != (launch['pid'], launch['creation100ns']),
        'distinct exact probe and GPU fixture owners')


def check_loader(case, reference):
    loader = case['loaderInvocation']; launch = case['process']
    fields(loader, ('renderer', 'absoluteDllArgument', 'modulePathIndependentlyReported'), 'loader invocation')
    unique_identities([loader['renderer']], 'copied renderer')
    path = loader['absoluteDllArgument']
    require(type(path) is str and Path(path).is_absolute()
        and str(Path(path).resolve()) == loader['renderer']['path'], 'exact absolute renderer argument')
    exact(loader, dict(modulePathIndependentlyReported=False))
    # The fixture has no GetModuleFileName(HMODULE) output. Record this as an
    # invocation/copy identity check, never as an independently observed module.
    expected = [launch['argv'][0], '--dll', path, '--expect-desktop', case['desktopName'],
        '--input-desktop', case['desktopSafety']['inputBefore']]
    if reference: expected.append('--reference')
    require(launch['argv'] == expected, 'exact fixture arguments and reference/remote scope')


def check_comparison(cases):
    require(type(cases) is list and len(cases) == 3, 'all three separately executed cases required')
    identities = set(); desktops = set(); comparisons = []
    for case, label in zip(cases, LABELS):
        require(case['label'] == label and case['ok'] is True, 'missing/reordered/failed comparison case')
        result = check_case(case['reports'], case['hostReports'], case['process'],
            case['desktopName'], case['desktopSafety']['inputBefore'], label != 'remote')
        check_desktop_closed(case['desktopSafety'], case['probe'], case['process'],
            case['desktopName'], case['desktopSafety']['inputBefore'])
        check_probe(case['probeReports'], case['probe'], case['process'],
            case['desktopName'], case['desktopSafety']['inputBefore'])
        check_loader(case, label != 'remote')
        if label == 'remote':
            require(case['childSettlement'] in ('process-object-absent', 'pid-reused-original-object-gone',
                'exact-process-handle-signaled'), 'independent exact-child settlement missing')
        key = (case['process']['pid'], case['process']['creation100ns'])
        require(key not in identities and case['desktopName'] not in desktops, 'reused process/Desktop cannot be three cases')
        identities.add(key); desktops.add(case['desktopName']); comparisons.append(result)
    hashes = ('vertexTokenFnv', 'redTokenFnv', 'greenTokenFnv', 'redSampledPixelFnv', 'greenSampledPixelFnv')
    require(all(all(row[key] == comparisons[0][key] for key in hashes) for row in comparisons),
        'old/new/remote original tokens or sampled pixels differ')
    return dict(ok=True, independentProcesses=3, independentDesktops=3, frames=216,
        pixelsChecked=718848, originalTokenHashesEqual=True, sampledPixelHashesEqual=True,
        cases=comparisons, cacheReuseCertified=False, playerVisualAccepted=False, memorySavingsProven=False,
        loaderArgumentAndCopyBound=True, loadedModulePathIndependentlyObserved=False)


def unique_identities(rows, label):
    require(type(rows) is list and rows, 'nonempty frozen ' + label)
    paths = []
    for row in rows:
        fields(row, ('path', 'bytes', 'sha256'), label + ' file')
        require(type(row['path']) is str and row['path'] and native_uint(row['bytes']) and row['bytes'] > 0
            and type(row['sha256']) is str and re.fullmatch('[0-9A-F]{64}', row['sha256']), 'frozen file identity types')
        require(Path(row['path']).is_absolute(), 'frozen input path must be absolute')
        paths.append(str(Path(row['path']).resolve()).casefold())
    require(len(paths) == len(set(paths)), 'duplicate frozen ' + label)


def baseline_inputs(path):
    # This reopens and independently validates all actual M0 raw records and
    # preserved inputs; a top-level ok=true alone never authorizes execution.
    baseline, reports, hosts = baseline_reader.verify_baseline(path)
    memory.verify_raw_receipt(baseline, path.parent)
    exact(baseline, dict(schema=1, scope='M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT', ok=True,
        sourcesPreserved=True, sourceAndReferencePreserved=True, playerDllChanged=False))
    require(type(baseline['cases']) is list and len(baseline['cases']) == 1
        and baseline['cases'][0]['ok'] is True, 'one complete passing M0 native case')
    baseline_reader.check_baseline_process_policy(baseline, True)
    for name in ('sources', 'binaries', 'compilers'):
        unique_identities(baseline[name], name)
    case = baseline['cases'][0]
    require(case['reports'] == reports and case['hostReports'] == hosts, 'baseline embedded/raw records disagree')
    safety = baseline['desktopSafety']
    exact(safety, dict(ok=True, desktopCreated=True, desktopClosed=True, creatorExited=True,
        restrictedMediumRequested=True, desktopHandleInheritable=False, desktopSwitchRequested=False,
        globalInputUsed=False, gameTouched=False))
    require(safety['cleanupErrors'] == [] and safety['launches'], 'baseline cleanup incomplete')
    for row in safety['launches']:
        check_launch(row, baseline['desktopName'], safety['inputBefore']); check_medium(row)
    for name in ('inputBefore', 'inputAfterCreate', 'inputBeforeLaunch', 'inputAfterLaunch', 'inputAfterRun', 'inputAfterClose'):
        exact(safety, {name: safety['inputBefore']})
    require(not safety.get('error') and not safety.get('bodyError'), 'baseline Desktop body/cleanup error')
    probe = baseline['desktopProbe']
    require(safety['launches'][0] == probe and safety['launches'][-1] == case['process'],
        'M0 CPU probe must precede the actual GPU fixture')
    raw_probes = json_lines(path.parent/'probe.stdout') + json_lines(path.parent/'desktop-child.log')
    require(raw_probes == baseline['probeReports'], 'baseline CPU probe embedded/raw mismatch')
    check_probe(raw_probes, probe, case['process'], baseline['desktopName'], safety['inputBefore'])
    exact(baseline, dict(processControls=dict(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0')))
    wanted = {'proxy': 'warvk-d3d9-proxy-dev.dll', 'host': 'warvk-d3d9-host-dev.exe',
        'probe32': 'desktop-probe-32.exe', 'probe64': 'desktop-probe-64.exe'}
    selected = {}
    for key, filename in wanted.items():
        found = [row for row in baseline['binaries'] if Path(row['path']).name == filename]
        require(len(found) == 1, 'one exact baseline ' + filename)
        selected[key] = Path(found[0]['path']).resolve(strict=True)
    return baseline, selected


def source_set():
    return memory.source_set() | {ROOT/'AutoTest'/name for name in (
        'test_render_host_shader_cache_equivalence.cpp', 'test_render_host_d3d9_parent.cpp',
        'run_render_host_shader_cache_equivalence.py', 'test_render_host_shader_cache_equivalence.py',
        'run_render_host_memory_destroy_mutation.py', 'test_render_host_memory_destroy_mutation.py')}


def protected(path):
    return dict(exists=True, identity=identity(path)) if path.is_file() else dict(exists=False, path=str(path))


def verify_case_artifacts(case):
    directory = Path(case['artifactDirectory']).resolve(strict=True)
    require(type(case['artifacts']) is list and case['artifacts'], 'initial case artifact population missing')
    paths = set()
    for row in case['artifacts']:
        fields(row, ('path', 'bytes', 'sha256'), 'raw artifact')
        path = Path(row['path']); canonical = str(path.resolve()).casefold()
        require(path.parent.resolve(strict=True) == directory
            and canonical not in paths and row == identity(path), 'raw artifact escaped/duplicate/change')
        paths.add(canonical)
    require(paths == {str(path.resolve()).casefold() for path in directory.iterdir() if path.is_file()},
        'case artifact population changed or incomplete')
    if case['ok'] or 'probeRawArtifacts' in case:
        verify_raw_artifacts(case.get('probeRawArtifacts'), directory,
            ['probe.stdout', 'probe.stderr', 'probe-child.log'], False)
    if case['ok'] or 'initialRawArtifacts' in case:
        verify_raw_artifacts(case.get('initialRawArtifacts'), directory,
            ['case.stdout', 'case.stderr'] + (['host.log'] if case['label'] == 'remote' else []), True)
    if case['ok']:
        require(Path(case['loaderInvocation']['absoluteDllArgument']).parent.resolve(strict=True) == directory,
            'renderer and raw artifact directories differ')
        require(case['reports'] == json_lines(directory/'case.stdout'), 'parent raw evidence changed after acceptance')
        hosts = json_lines(directory/'host.log') if case['label'] == 'remote' else []
        require(case['hostReports'] == hosts, 'host raw evidence changed after acceptance')
        require(case['probeReports'] == json_lines(directory/'probe.stdout') + json_lines(directory/'probe-child.log'),
            'CPU probe raw evidence changed after acceptance')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('memory-receipt', 'old-reference', 'new-reference', 'toolchain', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope=SCOPE, ok=False, priority='BelowNormal', parallelism=1,
        cases=[], steps=[], caseCopies=[], restrictedMediumRequested=True, gameTouched=False,
        playerDllChanged=None, shippingDllModified=False, sourcePreserved=False, cacheReuseCertified=False,
        nativeGpuRunRequested=True, productIntegrated=False, memorySavingsProven=False, playerVisualAccepted=False,
        rawFailurePreserved=True, driverOverridesAdded=False, wireChanged=False)
    files = set(); frozen = []; preserved_players = {}; baseline_identity = None
    def copy_file(source, destination):
        require(not destination.exists(), 'new exact case copy required')
        before = identity(source); shutil.copy2(source, destination); copied = identity(destination)
        require(all(before[k] == copied[k] for k in ('bytes', 'sha256')), 'case copy differs from frozen input')
        receipt['caseCopies'].append(dict(source=before, copy=copied))
        return destination
    try:
        require(os.name == 'nt', 'Windows root-owned native lease required')
        memory_path = args.memory_receipt.resolve(strict=True); baseline_identity = identity(memory_path)
        receipt['memoryReceipt'] = baseline_identity
        baseline, inputs = baseline_inputs(memory_path)
        receipt['baselineInputs'] = {key: identity(value) for key, value in inputs.items()}
        receipt['baselineRawArtifacts'] = memory.verify_raw_receipt(baseline, memory_path.parent)
        lineage = [*baseline['sources'], *baseline['binaries'], *baseline['compilers'],
            *(baseline[name] for name in ('compileCommands', 'referenceBefore', 'referenceAfter', 'playerBefore', 'playerAfter')),
            *baseline['cases'][0]['artifacts'],
            *(row[role] for row in baseline['caseCopies'] for role in ('source', 'copy')),
            *(record for manifest in receipt['baselineRawArtifacts'] for record in manifest['files'])]
        receipt['baselineFiles'] = list({row['path']: row for row in lineage}.values())
        refs = {'old-reference': args.old_reference.resolve(strict=True),
                'new-reference': args.new_reference.resolve(strict=True)}
        require(refs['old-reference'] != refs['new-reference'], 'old/new references must be distinct paths')
        receipt['referencesBefore'] = {name: identity(path) for name, path in refs.items()}
        require(receipt['referencesBefore']['old-reference']['sha256'] != receipt['referencesBefore']['new-reference']['sha256'],
            'old/new shipping references must have different frozen bytes')
        for player in (Path('E:/Work/Warcraft III/d3d9.dll'), Path('E:/Work/War3/d3d9.dll')):
            preserved_players[str(player)] = protected(player)
        receipt['playersBefore'] = preserved_players
        files = source_set(); frozen = [identity(path) for path in sorted(files)]; receipt['sources'] = frozen
        cc = args.toolchain.resolve(strict=True)/'i686-w64-mingw32-clang++.exe'
        receipt['compiler'] = identity(cc)
        require(receipt['compiler'] in baseline['compilers'], 'x86 compiler must be the frozen M0 compiler')
        for key, path in inputs.items():
            require(pe_identity(path) == ((0x8664, 0x20b) if key in ('host', 'probe64') else (0x14c, 0x10b)),
                'frozen input PE width ' + key)
        require(all(pe_identity(path) == (0x14c, 0x10b) for path in refs.values()), 'both references must be x86 PE DLLs')
        env = {key: value for key, value in os.environ.items() if key.upper() in memory.ENV_ALLOW}
        controls = dict(baseline['processControls']); env.update(controls)
        receipt['processControls'] = controls
        receipt['inheritedEnvironmentNames'] = sorted(key for key in env if key.upper() in memory.ENV_ALLOW)
        receipt['steps'].append(execute([sys.executable, '-B', ROOT/'AutoTest/test_render_host_shader_cache_equivalence.py'],
            ROOT, out/'reader-tests', 30, env))
        fixture = out/'shader-cache-equivalence32.exe'
        receipt['steps'].append(execute([cc, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
            '-pthread', '-municode', '-D_WIN32_WINNT=0x0a00', ROOT/'AutoTest/test_render_host_shader_cache_equivalence.cpp',
            '-luser32', '-o', fixture], ROOT, out/'fixture-build', 180, env))
        require(pe_identity(fixture) == (0x14c, 0x10b), 'actual x86 fixture required')
        receipt['fixture'] = identity(fixture)
        for label in LABELS:
            case_dir = out/label; case_dir.mkdir()
            case = dict(label=label, ok=False, deadlineSeconds=45, hostReports=[], reports=[],
                artifactDirectory=str(case_dir))
            receipt['cases'].append(case)
            local_fixture = copy_file(fixture, case_dir/fixture.name)
            renderer = copy_file(inputs['proxy'] if label == 'remote' else refs[label], case_dir/'renderer.dll')
            local_probes = {bits: copy_file(inputs['probe' + str(bits)], case_dir/f'desktop-probe-{bits}.exe') for bits in (32, 64)}
            case_controls = dict(DXVK_LOG_PATH=str(case_dir), DXVK_CONFIG_FILE=str(case_dir/'absent.conf'))
            require(not (case_dir/'absent.conf').exists(), 'reference config must be absent, not copied from player')
            if label == 'remote':
                local_host = copy_file(inputs['host'], case_dir/'warvk-d3d9-host-dev.exe')
                case_controls.update(WARVK_D3D9_HOST_EXE=str(local_host), WARVK_D3D9_HOST_LOG=str(case_dir/'host.log'))
            case['processControls'] = controls | case_controls
            try:
                with IsolatedRenderDesktop(name_prefix='WarVK_ShaderCache', restricted_medium=True) as desktop:
                    case['desktopName'] = desktop.desktop_name; case['desktopSafety'] = desktop.receipt
                    probe = desktop.run([local_probes[32], '--expect-desktop', desktop.desktop_name,
                        '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                        '--child', local_probes[64], '--child-log', case_dir/'probe-child.log'],
                        cwd=case_dir, stdout_path=case_dir/'probe.stdout', stderr_path=case_dir/'probe.stderr',
                        timeout_s=30, env=env)
                    case['probe'] = probe
                    case['probeRawArtifacts'] = capture_raw_artifacts(case_dir,
                        ['probe.stdout', 'probe.stderr', 'probe-child.log'])
                    check_launch(probe, desktop.desktop_name, desktop.input_before); check_medium(probe)
                    probe_rows = json_lines(case_dir/'probe.stdout') + json_lines(case_dir/'probe-child.log')
                    case['probeReports'] = probe_rows; require(len(probe_rows) == 2, 'exact CPU probe pair')
                    validate_probe_receipt(probe_rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                        station='WinSta0', bits=32, child=probe_rows[1], launch=probe)
                    argv = [local_fixture, '--dll', renderer, '--expect-desktop', desktop.desktop_name,
                        '--input-desktop', desktop.input_before]
                    if label != 'remote': argv.append('--reference')
                    case['loaderInvocation'] = dict(renderer=identity(renderer), absoluteDllArgument=str(renderer),
                        modulePathIndependentlyReported=False)
                    launch = desktop.run(argv, cwd=case_dir, stdout_path=case_dir/'case.stdout',
                        stderr_path=case_dir/'case.stderr', timeout_s=45, env=env | case_controls)
                    case['process'] = launch
                    case['initialRawArtifacts'] = capture_raw_artifacts(case_dir,
                        ['case.stdout', 'case.stderr'] + (['host.log'] if label == 'remote' else []), True)
                    case['reports'] = json_lines(case_dir/'case.stdout')
                    if label == 'remote': case['hostReports'] = json_lines(case_dir/'host.log')
                    case['acceptance'] = check_case(case['reports'], case['hostReports'], launch,
                        desktop.desktop_name, desktop.input_before, label != 'remote')
                    if label == 'remote':
                        proxy_row = case['reports'][-2]
                        case['childSettlement'] = child_settled(proxy_row['childPid'], proxy_row['childCreation'])
                check_desktop_closed(case['desktopSafety'], case['probe'], case['process'],
                    case['desktopName'], case['desktopSafety']['inputBefore'])
                case['ok'] = True
            except Exception as error:
                case['error'] = str(error)
                # Keep the exact failed launch and partial stdout/host artifacts.
                # Never promote render-only evidence to normal completion.
            finally:
                case['artifacts'] = [identity(path) for path in sorted(case_dir.iterdir()) if path.is_file()]
            if not case['ok']:
                # Independent later cases remain useful, but never run them if
                # failed cleanup left a process, Job, Desktop or input drift.
                safety = case.get('desktopSafety', {})
                require(safety.get('desktopClosed') is True and safety.get('creatorExited') is True
                    and safety.get('cleanupErrors') == [] and safety.get('inputAfterClose') == safety.get('inputBefore')
                    and all(row.get('processSettled') is True and row.get('jobEmpty') is True
                            for row in safety.get('launches', [])), 'failed case did not safely settle; no further launches')
        receipt['comparison'] = check_comparison(receipt['cases'])
        require(receipt['fixture'] == identity(fixture), 'compiled fixture changed')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            if baseline_identity:
                require(baseline_identity == identity(Path(baseline_identity['path'])), 'baseline receipt changed')
            if 'referencesBefore' in receipt:
                receipt['referencesAfter'] = {name: identity(Path(row['path'])) for name, row in receipt['referencesBefore'].items()}
                require(receipt['referencesBefore'] == receipt['referencesAfter'], 'old/new shipping input changed')
            if preserved_players:
                receipt['playersAfter'] = {name: protected(Path(name)) for name in preserved_players}
                receipt['playerDllChanged'] = receipt['playersAfter'] != preserved_players
                require(not receipt['playerDllChanged'], 'player DLL creation/removal/change detected')
            if frozen:
                receipt['sourcePreserved'] = files == source_set() and frozen == [identity(path) for path in sorted(files)]
                require(receipt['sourcePreserved'], 'frozen source set changed during gate')
            if 'baselineInputs' in receipt:
                require(receipt['baselineInputs'] == {name: identity(Path(row['path'])) for name, row in receipt['baselineInputs'].items()},
                    'baseline frozen executable changed')
            if 'baselineFiles' in receipt:
                require(all(row == identity(Path(row['path'])) for row in receipt['baselineFiles']),
                    'M0 raw evidence or frozen dependency changed')
            for manifest in receipt.get('baselineRawArtifacts', []): verify_raw_artifacts(manifest)
            if 'compiler' in receipt:
                require(receipt['compiler'] == identity(Path(receipt['compiler']['path'])), 'fixture compiler changed')
            require(all(row['source'] == identity(Path(row['source']['path']))
                and row['copy'] == identity(Path(row['copy']['path'])) for row in receipt['caseCopies']), 'case copy/source changed')
            for case in receipt['cases']:
                if 'artifacts' in case: verify_case_artifacts(case)
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        receipt['artifacts'] = [identity(path) for path in sorted(out.iterdir())
            if path.is_file() and path.name != 'receipt.json']
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'scope', 'error', 'preservationError',
        'sourcePreserved', 'playerDllChanged', 'cacheReuseCertified', 'memorySavingsProven')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
