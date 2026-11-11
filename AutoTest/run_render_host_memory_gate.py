"""M0: real x86/host memory accounts around the existing C2 COM workload.

Only the root agent may grant the build/native/GPU lease. This runner uses one
BelowNormal/-j2 build owner and a fresh non-input desktop after CPU probes.
The paired before/host/after observations are time windows, NOT simultaneous
samples. OS byte ranges/deltas are observations, not invented pass thresholds.
This is an account, not same-input A/B memory savings or a product release.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, child_settled
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import sources, json_lines, native_uint
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests


SAMPLES = ((1, 0), (2, 1), (3, 1), (4, 1), (4, 16), (4, 48), (4, 95), (5, 96))
SEQUENCES = (3, 15, 20, 27, 328, 969, 1910, 2437)
SERIALS = (1, 5, 8, 8, 113, 337, 666, 673)
DRAWS = (0, 0, 1, 1, 16, 48, 95, 96)
UPLOADS = (0, 3, 3, 3, 48, 144, 285, 288)
KINDS = ('vertexBuffer', 'indexBuffer', 'texture', 'systemSurface',
         'vertexShader', 'pixelShader', 'declaration', 'presentedReadback')
ACCOUNT_FIELDS = ('objects', 'logicalBytes', 'cpuSizeBytes', 'cpuCapacityBytes')
OS_FIELDS = ('privateBytes', 'peakCommitBytes', 'workingSetBytes', 'usedVirtualBytes',
             'privateVirtualBytes', 'mappedVirtualBytes', 'regions')
CHECKPOINT = 'warvk-d3d9-memory-checkpoint/v1'
MIRROR = 'warvk-d3d9-memory-host/v1'
SUMMARY = 'warvk-d3d9-memory/v1'
PROXY = 'warvk-d3d9-proxy/v1'
HOST = 'warvk-d3d9-host/v1'
DESKTOP = 'warvk-d3d9-desktop/v1'
ENV_ALLOW = frozenset(('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'TEMP', 'TMP',
    'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)',
    'PROGRAMDATA', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'))


def uints(record, values):
    for field, expected in values.items():
        require(native_uint(record[field]) and record[field] == expected,
                'exact unsigned integer ' + field)


def fields(record, expected, label):
    require(type(record) is dict and set(record) == set(expected), label + ' exact field set')


def unsigned(record, names, bits=64):
    for name in names:
        require(native_uint(record[name], bits), 'native unsigned ' + name)


def os_snapshot(value, bits):
    fields(value, ('valid', 'win32', *OS_FIELDS), 'OS snapshot')
    require(value['valid'] is True, 'missing/failed OS measurement is not zero')
    unsigned(value, ('win32',), 32); unsigned(value, OS_FIELDS)
    require(value['win32'] == 0, 'OS probe returned an error')
    require(value['privateBytes'] > 0 and value['workingSetBytes'] > 0
            and value['usedVirtualBytes'] > 0 and value['regions'] > 0, 'empty live-process measurement')
    require(value['peakCommitBytes'] >= value['privateBytes'], 'peak committed bytes below current private bytes')
    require(value['privateVirtualBytes'] + value['mappedVirtualBytes'] <= value['usedVirtualBytes'],
            'virtual region type accounting exceeds used VA')
    require(value['usedVirtualBytes'] < 1 << bits, 'VA measurement exceeds process width')


def cpu_account(value):
    fields(value, ('valid', 'kinds'), 'CPU account')
    require(value['valid'] is True, 'invalid/overflowed owned CPU account')
    fields(value['kinds'], KINDS, 'CPU account kinds')
    for name, row in value['kinds'].items():
        fields(row, ACCOUNT_FIELDS, name)
        unsigned(row, ACCOUNT_FIELDS)
        require(row['cpuSizeBytes'] <= row['cpuCapacityBytes'], 'owned size exceeds capacity: ' + name)
        if not row['objects']:
            require(not any(row.values()), 'dead object retained counted payload: ' + name)


def decode_host(wire_hex):
    """Independent fixed little-endian decode; no native encoder/offset imports."""
    require(type(wire_hex) is str and re.fullmatch('[0-9a-f]{1024}', wire_hex) is not None,
            'exact canonical 512-byte host wire required')
    raw = bytes.fromhex(wire_hex)
    words = struct.unpack_from('<8I15Q', raw)
    names = ('magic', 'version', 'phase', 'ordinal', 'pid', 'thread', 'bits', 'logicalResources',
             'creation', 'nonceLow', 'nonceHigh', 'sequence', 'tickBegin', 'tickEnd',
             'logicalBytes', 'submitted', 'completed', 'retired', 'draws', 'uploads', 'readbacks',
             'fixedPacketPayloadBytes', 'ownerInlineBytes')
    host = dict(zip(names, words))
    uints(host, dict(magic=0x314d3944, version=1, bits=64))
    require(host['phase'] in range(1, 6) and host['pid'] > 0 and host['thread'] > 0
            and host['creation'] > 0 and (host['nonceLow'] or host['nonceHigh'])
            and host['sequence'] > 0, 'host phase/process/nonce/sequence')
    require(host['ordinal'] == 0 if host['phase'] == 1 else host['ordinal'] > 0,
            'host phase/ordinal contract')
    require(host['tickBegin'] <= host['tickEnd'] and host['retired'] <= host['completed'] <= host['submitted'],
            'host time or GPU serial ordering')
    os_values = struct.unpack_from('<2I7Q', raw, 152)
    require(os_values[0] in (0, 1), 'wire OS boolean')
    host['process'] = dict(zip(('valid', 'win32', *OS_FIELDS), (bool(os_values[0]), *os_values[1:])))
    valid, reserved = struct.unpack_from('<2I', raw, 216)
    require(valid in (0, 1) and reserved == 0 and raw[480:] == bytes(32), 'wire CPU boolean/reserved bytes')
    host['cpu'] = dict(valid=bool(valid), kinds={})
    for index, kind in enumerate(KINDS):
        host['cpu']['kinds'][kind] = dict(zip(ACCOUNT_FIELDS, struct.unpack_from('<4Q', raw, 224+index*32)))
    os_snapshot(host['process'], 64); cpu_account(host['cpu'])
    return host


def read_records(reports, host_reports):
    """No selecting the last good summary from an incomplete/conflicting run."""
    require(type(reports) is list and type(host_reports) is list, 'record streams must be lists')
    parent_rows = {}; host_rows = {}; checkpoints = []; mirrors = []
    for records, singles, repeat, samples in (
            (reports, parent_rows, CHECKPOINT, checkpoints), (host_reports, host_rows, MIRROR, mirrors)):
        allowed = {PROXY, SUMMARY} if repeat == CHECKPOINT else {HOST, DESKTOP}
        for row in records:
            require(type(row) is dict and type(row.get('schema')) is str, 'record schema required')
            schema = row['schema']
            if schema == repeat:
                require(not ({PROXY, SUMMARY, HOST} & singles.keys()), 'checkpoint after terminal summary')
                samples.append(row)
            else:
                require(schema in allowed and schema not in singles, 'unexpected/duplicate/conflicting summary')
                if schema == DESKTOP:
                    require(not samples and not singles, 'host desktop must precede GPU/checkpoints')
                else:
                    require(len(samples) == 8, 'terminal summary before all eight checkpoints')
                singles[schema] = row
        require(set(singles) == allowed and len(samples) == 8, 'missing or extra checkpoint/summary')
    return parent_rows, host_rows, checkpoints, mirrors


def check_lifetime(rows, hosts, launch, desktop, input_desktop):
    """The unchanged 96-round COM/pixel workload, plus eight diagnostic ops.

    Do not subtract operations from real records to feed the old R1 checker:
    diagnostic commands are actual accepted C2 commands but not backend work.
    """
    f = rows[SUMMARY]; proxy = rows[PROXY]; host = hosts[HOST]; ds = hosts[DESKTOP]
    require(f['ok'] is True and f['mode'] == 'com' and f['publicD3D9Api'] is True, 'actual public COM workload')
    for name in ('fixtureCleanupComplete', 'parentVulkanAbsent'):
        require(f[name] is True, 'fixture boundary ' + name)
    for row in (f, proxy, host):
        for name in ('productIntegrated', 'memorySavingsProven'):
            require(row[name] is False, 'unsupported product/savings claim')
    for name in ('globalInputUsed', 'desktopSwitched', 'gameTouched', 'windowPixelsVerified', 'playerVisualAccepted'):
        require(f[name] is False, 'fixture scope ' + name)
    require(f['desktop'] == desktop and f['inputBefore'] == f['inputAfter'] == input_desktop
            and desktop != input_desktop, 'actual fixture desktop')
    uints(f, dict(bits=32, rounds=96, draws=96, readbacks=96, pixelsChecked=319488, pixelFailures=0,
        resourcesCreated=576, resourcesReleased=576,
        privateBoundDraws=96, finalPrivateBoundResources=6, queryIssues=256, queryPolls=256,
        checkpoints=8))
    require(native_uint(f['maxChannelError'], 32) and f['maxChannelError'] <= 1, 'independent pixel tolerance')
    for name in ('peerVerified', 'x86VulkanAbsent', 'childNaturalExit', 'childProcessSettled'):
        require(proxy[name] is True, 'proxy boundary ' + name)
    for name in ('transportFailed', 'deviceLost', 'remoteTerminal', 'childSelfFailStop'):
        require(proxy[name] is False, 'proxy failure ' + name)
    fields(proxy, ('schema', 'childPid', 'childCreation', 'childExit', 'exchanges', 'peerVerified',
        'transportFailed', 'x86VulkanAbsent', 'deviceLost', 'remoteTerminal', 'childNaturalExit',
        'childProcessSettled', 'childSelfFailStop', 'primaryExitWaitMs', 'productIntegrated', 'memorySavingsProven'),
        'proxy final')
    uints(proxy, dict(childExit=0, exchanges=2445)); unsigned(proxy, ('childPid',), 32)
    unsigned(proxy, ('childCreation', 'primaryExitWaitMs'))
    require(proxy['childPid'] > 0 and proxy['childCreation'] > 0 and proxy['primaryExitWaitMs'] <= 5000,
            'exact child identity or unchanged primary deadline')
    for name in ('ok', 'actualDxvkCore', 'actualDxso', 'normalTeardown', 'gpuInitialized', 'shutdownQueueDrained'):
        require(host[name] is True, 'real GPU lifetime ' + name)
    for name in ('gameTouched', 'presentImplemented'):
        require(host[name] is False, 'offscreen-only scope ' + name)
    host_exact = dict(bits=64, accepted=2445, rejected=0, backendCalls=2243, draws=96, clears=96, readbacks=96,
        submitted=673, gpuCompleted=673, retired=673, uploads=288, uploadedBytes=16896,
        buffersCreated=192, texturesCreated=96, shadersCompiled=192, declarationsCreated=96, queries=256,
        logicalHandlesCreated=577, logicalHandlesDestroyed=577, logicalHandlesReused=570, resourcesReused=570,
        queryHandlesCreated=1, queryHandlesDestroyed=1, queryHandlesReused=0, queryIssues=256, queryPolls=256, queryWaits=0,
        liveResources=0, liveLogicalBytes=0, liveLogicalHandles=0, liveQueries=0, presentAttempts=0,
        acquiredImages=0, presentQueued=0, presentApiSucceeded=0, presentGpuCompleted=0, presentRetired=0,
        presentedReadbacks=0, presentedFrame=0, windowProcessId=0, windowThreadId=0,
        lastAcquireVkResult=1, lastPresentVkResult=1)
    uints(host, host_exact)
    fields(host, ('schema', 'ok', 'actualDxvkCore', 'actualDxso', 'normalTeardown', 'gpuInitialized',
        'shutdownQueueDrained', 'gameTouched', 'presentImplemented', 'productIntegrated', 'memorySavingsProven',
        'peakLogicalBytes', 'vendorId', 'deviceId', *host_exact), 'host final')
    unsigned(host, ('peakLogicalBytes',)); unsigned(host, ('vendorId', 'deviceId'), 32)
    require(host['peakLogicalBytes'] > 0 and host['vendorId'] > 0 and host['deviceId'] > 0, 'GPU identity/peak observation')
    require(ds['beforeGpu'] is True and ds['desktop'] == desktop and ds['inputDesktop'] == input_desktop
            and ds['windowStation'] == 'WinSta0', 'independent host desktop before GPU')
    uints(ds, dict(pid=proxy['childPid'], peerPid=launch['pid'], windowThreadId=0))
    unsigned(ds, ('threadId',), 32); require(ds['threadId'] > 0, 'host owner thread')
    require(ds['pid'] != launch['pid'], 'x86 parent and x64 host must be distinct processes')
    fields(ds, ('schema', 'pid', 'threadId', 'peerPid', 'windowThreadId', 'windowStation', 'desktop',
                'inputDesktop', 'beforeGpu'), 'host desktop')


def check_owned_account(parent, host, summary):
    """Scene-derived payload counts; capacities are measured, never predicted."""
    live = parent['phase'] in (2, 3, 5)
    sizes = dict(vertexBuffer=160, indexBuffer=12, texture=4,
                 vertexShader=summary['vertexShaderBytes'], pixelShader=summary['pixelShaderBytes'], declaration=32)
    for kind in KINDS:
        p = parent['cpu']['kinds'][kind]; h = host['cpu']['kinds'][kind]
        system = kind == 'systemSurface'; count = int(live and kind in sizes)
        logical = sizes.get(kind, 0) if count else 0
        uints(p, dict(objects=1 if system else count, logicalBytes=16384 if system else logical,
                      cpuSizeBytes=16384 if system else logical))
        host_logical = 24 if count and kind == 'declaration' else logical
        host_cpu = 12 if count and kind == 'indexBuffer' else 0
        if count and kind in ('vertexShader', 'pixelShader'):
            stage = 'vertex' if kind == 'vertexShader' else 'pixel'
            host_cpu = summary[stage+'FloatConstants'] * summary['hostFloatConstantBytes']
            require(p['cpuCapacityBytes'] >= 16384 and p['cpuCapacityBytes'] % 4 == 0,
                    'proxy bounded token scanner capacity')
            require(h['cpuCapacityBytes'] % 20 == 0, 'host float-constant vector capacity unit')
        uints(h, dict(objects=count, logicalBytes=host_logical, cpuSizeBytes=host_cpu))
        if not count or kind in ('vertexBuffer', 'texture', 'declaration'):
            uints(h, dict(cpuCapacityBytes=0))
    uints(host, dict(logicalBytes=sum(row['logicalBytes'] for row in host['cpu']['kinds'].values())))
    uints(host, dict(logicalResources=sum(row['objects'] for row in host['cpu']['kinds'].values())))


def metric_ranges(rows):
    result = {}
    for name in OS_FIELDS:
        values = [row[name] for row in rows]
        result[name] = dict(samples=len(values), first=values[0], last=values[-1],
                            minimum=min(values), maximum=max(values), delta=values[-1]-values[0])
    return result


def check_summary(summary, launch, desktop, input_desktop):
    required = ('schema', 'ok', 'bits', 'mode', 'publicD3D9Api', 'rounds', 'draws', 'readbacks',
        'pixelsChecked', 'pixelFailures', 'maxChannelError', 'resourcesCreated', 'resourcesReleased',
        'privateBoundDraws', 'finalPrivateBoundResources', 'queryIssues', 'queryPolls', 'checkpoints',
        'cpuHelperChecks', 'vertexShaderBytes', 'pixelShaderBytes', 'vertexFloatConstants', 'pixelFloatConstants',
        'vertexIntConstants', 'pixelIntConstants', 'vertexBoolConstants', 'pixelBoolConstants', 'hostFloatConstantBytes',
        'parentPid', 'parentThread', 'parentCreation', 'initialProcess', 'finalProcess', 'fixtureCleanupComplete',
        'parentVulkanAbsent', 'desktop', 'inputBefore', 'inputAfter', 'globalInputUsed', 'desktopSwitched',
        'gameTouched', 'productIntegrated', 'memorySavingsProven', 'gpuPhysicalMeasured', 'opaqueCpuMeasured',
        'synchronizedInstant', 'windowPixelsVerified', 'playerVisualAccepted')
    fields(summary, required, 'M0 summary')
    for name in ('ok', 'publicD3D9Api', 'fixtureCleanupComplete', 'parentVulkanAbsent'):
        require(summary[name] is True, 'summary failed ' + name)
    for name in ('globalInputUsed', 'desktopSwitched', 'gameTouched', 'productIntegrated', 'memorySavingsProven',
                 'gpuPhysicalMeasured', 'opaqueCpuMeasured', 'synchronizedInstant', 'windowPixelsVerified', 'playerVisualAccepted'):
        require(summary[name] is False, 'summary unsupported claim ' + name)
    uints(summary, dict(bits=32, rounds=96, draws=96, readbacks=96, pixelsChecked=319488, pixelFailures=0,
        resourcesCreated=576, resourcesReleased=576, privateBoundDraws=96, finalPrivateBoundResources=6,
        queryIssues=256, queryPolls=256, checkpoints=8, cpuHelperChecks=54, hostFloatConstantBytes=20,
        vertexIntConstants=0, pixelIntConstants=0, vertexBoolConstants=0, pixelBoolConstants=0,
        parentPid=launch['pid'], parentCreation=launch['creation100ns'], parentThread=launch['initialThreadId']))
    unsigned(summary, ('maxChannelError', 'cpuHelperChecks', 'vertexShaderBytes', 'pixelShaderBytes',
                       'vertexFloatConstants', 'pixelFloatConstants'), 32)
    require(summary['maxChannelError'] <= 1, 'pixel tolerance')
    for prefix in ('vertex', 'pixel'):
        size = summary[prefix+'ShaderBytes']
        require(16 < size <= 16384 and size % 4 == 0, 'independent original shader token size')
        require(summary[prefix+'FloatConstants'] <= size//24, 'independent DEF count exceeds token storage')
    require(summary['mode'] == 'com' and summary['desktop'] == desktop
            and summary['inputBefore'] == summary['inputAfter'] == input_desktop, 'summary mode/desktop')
    for name in ('initialProcess', 'finalProcess'):
        os_snapshot(summary[name], 32)


def check_case(reports, host_reports, launch, desktop, input_desktop):
    check_launch(launch, desktop, input_desktop)
    rows, hosts, checkpoints, mirrors = read_records(reports, host_reports)
    check_lifetime(rows, hosts, launch, desktop, input_desktop)
    summary = rows[SUMMARY]; check_summary(summary, launch, desktop, input_desktop)
    proxy = rows[PROXY]; ds = hosts[DESKTOP]
    samples = []; old_end = None; nonce = None; stable = None; live_accounts = None
    parent_fields = ('version', 'phase', 'ordinal', 'pid', 'thread', 'bits', 'logicalHandles', 'reservedHandles',
        'privateBindings', 'publicZeroBindings', 'expectedHostPid', 'expectedHostCreation', 'expectedNonceLow',
        'expectedNonceHigh', 'expectedSequence', 'creation', 'tickBegin', 'beforeEnd', 'afterBegin', 'tickEnd',
        'connectionInlineBytes', 'fixedPacketPayloadBytes', 'workerBatchPayloadBytes', 'before', 'after', 'cpu')
    checkpoint_fields = ('schema', 'phase', 'ordinal', 'parent', 'hostWireHex', 'synchronizedInstant',
        'gpuPhysicalMeasured', 'opaqueCpuMeasured', 'gameTouched', 'productIntegrated', 'memorySavingsProven')
    u32 = ('version', 'phase', 'ordinal', 'pid', 'thread', 'bits', 'logicalHandles', 'reservedHandles',
           'privateBindings', 'publicZeroBindings', 'expectedHostPid')
    u64 = tuple(name for name in parent_fields if name not in (*u32, 'before', 'after', 'cpu'))
    for index, ((phase, ordinal), checkpoint, mirror) in enumerate(zip(SAMPLES, checkpoints, mirrors)):
        fields(checkpoint, checkpoint_fields, 'M0 checkpoint')
        fields(mirror, ('schema', 'phase', 'ordinal', 'sequence', 'wireHex'), 'host independent mirror')
        uints(checkpoint, dict(phase=phase, ordinal=ordinal))
        uints(mirror, dict(phase=phase, ordinal=ordinal, sequence=SEQUENCES[index]))
        for name in checkpoint_fields[5:]:
            require(checkpoint[name] is False, 'checkpoint unsupported claim ' + name)
        parent = checkpoint['parent']; fields(parent, parent_fields, 'checkpoint parent')
        unsigned(parent, u32, 32); unsigned(parent, u64)
        live = phase in (2, 3, 5); private = phase in (3, 5)
        uints(parent, dict(version=1, phase=phase, ordinal=ordinal, pid=launch['pid'], bits=32,
            thread=launch['initialThreadId'], creation=launch['creation100ns'],
            logicalHandles=7 if live else 1, reservedHandles=0, privateBindings=6 if private else 0,
            publicZeroBindings=6 if private else 0, expectedHostPid=proxy['childPid'],
            expectedHostCreation=proxy['childCreation'], expectedSequence=SEQUENCES[index],
            fixedPacketPayloadBytes=393216, workerBatchPayloadBytes=65536))
        # These packet payload bytes are a SUBSET of Connection's sizeof.
        require(parent['connectionInlineBytes'] >= parent['fixedPacketPayloadBytes'], 'connection inline subset accounting')
        for name in ('before', 'after'): os_snapshot(parent[name], 32)
        cpu_account(parent['cpu'])
        host = decode_host(checkpoint['hostWireHex']); independent = decode_host(mirror['wireHex'])
        require(checkpoint['hostWireHex'] == mirror['wireHex'] and host == independent, 'independent host wire mismatch')
        uints(host, dict(phase=phase, ordinal=ordinal, pid=proxy['childPid'], creation=proxy['childCreation'],
            thread=ds['threadId'], bits=64, sequence=SEQUENCES[index], nonceLow=parent['expectedNonceLow'],
            nonceHigh=parent['expectedNonceHigh'], logicalResources=6 if live else 0,
            submitted=SERIALS[index], completed=SERIALS[index], retired=SERIALS[index],
            draws=DRAWS[index], readbacks=DRAWS[index], uploads=UPLOADS[index], fixedPacketPayloadBytes=131072))
        observed_nonce = (host['nonceLow'], host['nonceHigh'])
        if nonce is None: nonce = observed_nonce
        require(observed_nonce == nonce, 'cross-session nonce changed')
        times = (parent['tickBegin'], parent['beforeEnd'], host['tickBegin'], host['tickEnd'],
                 parent['afterBegin'], parent['tickEnd'])
        require(all(a <= b for a, b in zip(times, times[1:])), 'before/host/after sample window order')
        require(old_end is None or old_end <= times[0], 'overlapping/reordered checkpoint windows')
        old_end = times[-1]
        require(host['ownerInlineBytes'] > 0, 'missing host owned inline byte observation')
        footprint = (parent['connectionInlineBytes'], parent['fixedPacketPayloadBytes'],
                     parent['workerBatchPayloadBytes'], host['ownerInlineBytes'], host['fixedPacketPayloadBytes'])
        if stable is None: stable = footprint
        require(stable == footprint, 'fixed inline/payload footprint changed')
        check_owned_account(parent, host, summary)
        accounts = (parent['cpu'], host['cpu'])
        if not live:
            if index == 0: baseline = accounts
            require(accounts == baseline, 'drained payload account did not return to ready baseline')
        else:
            if live_accounts is None: live_accounts = accounts
            require(accounts == live_accounts, 'same-shape owned-resource capacities grew between rounds')
        samples.append(dict(phase=phase, ordinal=ordinal, sequence=host['sequence'],
            windowBegin=times[0], windowEnd=times[-1], windowMilliseconds=times[-1]-times[0],
            parent=parent, host=host))
    peak = 160+12+4+24+summary['vertexShaderBytes']+summary['pixelShaderBytes']
    uints(hosts[HOST], dict(peakLogicalBytes=peak))
    parent_os = [summary['initialProcess']]
    for sample in samples: parent_os.extend((sample['parent']['before'], sample['parent']['after']))
    parent_os.append(summary['finalProcess'])
    host_os = [sample['host']['process'] for sample in samples]
    for series in (parent_os, host_os):
        require(all(a['peakCommitBytes'] <= b['peakCommitBytes'] for a, b in zip(series, series[1:])),
                'process lifetime commit peak decreased')
    drained = [sample for sample in samples if sample['phase'] == 4]
    return dict(schema='warvk-d3d9-memory-account-analysis/v1', ok=True, checkpoints=len(samples),
        scope='explicit-owned-payloads-and-self-process-OS-observations',
        sampleWindows=samples, parentOS=metric_ranges(parent_os), hostOS=metric_ranges(host_os),
        drainedParentBeforeOS=metric_ranges([s['parent']['before'] for s in drained]),
        drainedParentAfterOS=metric_ranges([s['parent']['after'] for s in drained]),
        drainedHostOS=metric_ranges([s['host']['process'] for s in drained]),
        drainedOrdinals=[s['ordinal'] for s in drained], sameShapeOwnedAccountsStable=True,
        publicZeroPrivateResourcesStayedLive=True, exactOwnedBaselineRecovered=True,
        finalLogicalHandles=hosts[HOST]['liveLogicalHandles'], finalLogicalResources=hosts[HOST]['liveResources'],
        accountingNotes=[
            'peakCommitBytes is PROCESS_MEMORY_COUNTERS_EX.PeakPagefileUsage, not peak private residency',
            'parent fixedPacketPayloadBytes is a subset of connectionInlineBytes; never add them',
            'workerBatchPayloadBytes is worker-local payload storage, not worker stack commitment',
            'logical GPU resource bytes are not physical GPU residency; never sum peer accounts as VRAM',
            'explicit payload vectors exclude object overhead and opaque compiler/driver allocations',
            'OS before/host/after windows are paired observations, not simultaneous global measurements',
            'drained-cycle OS ranges and endpoint deltas are observations, not a memory-saving or plateau gate'],
        synchronizedInstant=False, gpuPhysicalMeasured=False, opaqueCpuMeasured=False,
        osPlateauAcceptanceThreshold=None, memorySavingsProven=False, gameTouched=False, productIntegrated=False)


def mutations(reports, host_reports, launch, desktop, input_desktop):
    """Alter actual successful records in memory; no repeated native/GPU work."""
    check_case(reports, host_reports, launch, desktop, input_desktop)
    tested = killed = 0
    def attempt(parents, hosts, process):
        nonlocal tested, killed
        tested += 1
        try: check_case(parents, hosts, process, desktop, input_desktop)
        except (ValueError, KeyError, TypeError): killed += 1
    for stream, records in enumerate((reports, host_reports)):
        for index, row in enumerate(records):
            lists = copy.deepcopy([reports, host_reports]); lists[stream].append(copy.deepcopy(row))
            attempt(*lists, copy.deepcopy(launch))
            lists = copy.deepcopy([reports, host_reports]); del lists[stream][index]
            attempt(*lists, copy.deepcopy(launch))
            for field in row:
                lists = copy.deepcopy([reports, host_reports]); del lists[stream][index][field]
                attempt(*lists, copy.deepcopy(launch))
    for field in ('jobEmpty', 'processSettled', 'forcedTermination', 'timedOut', 'exitCode', 'inputAfter'):
        process = copy.deepcopy(launch); value = process[field]
        process[field] = not value if type(value) is bool else value+1 if type(value) is int else value+'-changed'
        attempt(copy.deepcopy(reports), copy.deepcopy(host_reports), process)
    for index, row in enumerate(reports):
        if row['schema'] != CHECKPOINT: continue
        for field, value in row['parent'].items():
            lists = copy.deepcopy([reports, host_reports]); del lists[0][index]['parent'][field]
            attempt(*lists, copy.deepcopy(launch))
            if type(value) is int:
                for bad in (False, -1, 1.0, 1 << 64):
                    lists = copy.deepcopy([reports, host_reports]); lists[0][index]['parent'][field] = bad
                    attempt(*lists, copy.deepcopy(launch))
        for byte in (0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 80, 88, 96, 104, 112, 120,
                     128, 136, 152, 156, 216, 220, 224, 232, 240, 248, 480, 511):
            lists = copy.deepcopy([reports, host_reports]); raw = bytearray.fromhex(row['hostWireHex']); raw[byte] ^= 1
            lists[0][index]['hostWireHex'] = raw.hex()
            attempt(*lists, copy.deepcopy(launch))
    require(tested > 0 and tested == killed, 'memory reader mutation survived')
    return dict(tested=tested, killed=killed, source='actual-passing-runtime-records', nativeRerun=False, gpuRerun=False)


def verify_raw_receipt(receipt, root):
    """Current producer-time M0 evidence only; never retrofit an old receipt."""
    require(receipt.get('rawArtifactsPreserved') is True, 'M0 initial raw evidence was not frozen and rechecked')
    root = Path(root).resolve(strict=True)
    verify_raw_artifacts(receipt.get('probeRawArtifacts'), root,
                         ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False)
    steps = receipt.get('stepRawArtifacts')
    expected = [(name, bits) for bits in (32, 64) for name in ('resource_registry', 'wire')]
    require(type(steps) is list and len(steps) == len(expected), 'M0 initial CPU raw manifest population')
    for manifest, (name, bits) in zip(steps, expected):
        verify_raw_artifacts(manifest, root, [f'{name}-{bits}.stdout', f'{name}-{bits}.stderr'], False)
    require(type(receipt.get('cases')) is list and len(receipt['cases']) == 1, 'one M0 initial raw case required')
    case = receipt['cases'][0]; directory = root/'memory-account'
    require(case.get('label') == 'memory-account'
            and Path(case['process']['stdout']) == directory/'case.stdout'
            and Path(case['process']['stderr']) == directory/'case.stderr', 'M0 raw case redirection identity')
    verify_raw_artifacts(case.get('initialRawArtifacts'), directory,
                         ['case.stdout', 'case.stderr', 'host.log'], True)
    return list(raw_manifests(receipt))


def source_set():
    return set(sources()) | {ROOT/'AutoTest'/name for name in (
        'test_render_host_d3d9_memory.cpp', 'test_render_host_d3d9_resource_lifetime.cpp',
        'run_render_host_memory_gate.py', 'test_render_host_memory_gate.py',
        'run_render_host_presenter_gate.py', 'run_render_host_protocol_gate.py',
        'run_render_host_transport_gate.py', 'run_render_host_core_gate.py',
        'isolated_render_desktop.py', 'test_isolated_render_desktop_contract.py',
        'render_lab_token.py', 'test_render_lab_token.py',
        'render_lab_default_dacl.py', 'test_render_lab_default_dacl.py',
        'render_lab_raw_artifacts.py', 'test_render_lab_raw_artifacts.py',
        'test_render_host_desktop_probe.cpp')}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('toolchain', 'build', 'reference-dll', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--restricted-medium', action='store_true',
                    help='opt-in restricted medium-integrity children; the accounting workload is unchanged')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT', ok=False,
        gameTouched=False, playerDllChanged=None, productIntegrated=False,
        memorySavingsProven=False, gpuPhysicalMemoryMeasured=False,
        simultaneousSamples=False, osPlateauAcceptanceThreshold=None,
        restrictedMediumRequested=args.restricted_medium,
        windowPixelsVerified=False, playerVisualAccepted=False,
        priority='BelowNormal', parallelism=2, steps=[], cases=[], caseCopies=[],
        stepRawArtifacts=[], rawArtifactsPreserved=False)
    def copy_artifact(source, target):
        require(not target.exists(), 'new executable copy required')
        original = identity(source); shutil.copy2(source, target); copied = identity(target)
        require(all(original[key] == copied[key] for key in ('bytes', 'sha256')), 'executed copy differs')
        receipt['caseCopies'].append(dict(source=original, copy=copied))
        return target
    try:
        require(os.name == 'nt', 'Windows isolated native gate required')
        tc = args.toolchain.resolve(strict=True); build = args.build.resolve(strict=True)
        reference = args.reference_dll.resolve(strict=True); player = Path('E:/Work/Warcraft III/d3d9.dll')
        receipt['referenceBefore'] = identity(reference); receipt['playerBefore'] = identity(player)
        paths = source_set(); before = [identity(path) for path in sorted(paths)]; receipt['sources'] = before
        boundary = check_host_compile_boundary(build); receipt['allowedSharedD3D9Inputs'] = boundary
        cc = {bits: tc/(arch+'-w64-mingw32-clang++.exe') for bits, arch in ((32, 'i686'), (64, 'x86_64'))}
        receipt['compilers'] = [identity(path) for path in cc.values()]
        env = {key: value for key, value in os.environ.items() if key.upper() in ENV_ALLOW}
        controls = dict(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0')
        env.update(controls); receipt['processControls'] = controls
        receipt['inheritedEnvironmentNames'] = sorted(key for key in env if key.upper() in ENV_ALLOW)
        for script, prefix in (('test_isolated_render_desktop_contract.py', 'desktop-reader-tests'),
                               ('test_render_host_memory_gate.py', 'memory-reader-tests')):
            receipt['steps'].append(execute([sys.executable, '-B', ROOT/'AutoTest'/script],
                                           ROOT, out/prefix, 30, env))
        common = [ROOT/'tools/render_host'/name for name in
                  ('protocol.cpp', 'win32_transport.cpp', 'win32_security.cpp', 'owned_child.cpp')]
        memory_source = ROOT/'tools/render_host/process_memory_probe.cpp'
        probes = {}; cpu = []
        for bits in (32, 64):
            probes[bits] = out/f'desktop-probe-{bits}.exe'
            receipt['steps'].append(execute([cc[bits], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                '-municode', '-D_WIN32_WINNT=0x0a00', ROOT/'AutoTest/test_render_host_desktop_probe.cpp', *common,
                '-luser32', '-lbcrypt', '-ladvapi32', '-o', probes[bits]], ROOT, out/f'probe-build-{bits}', 180, env))
            for name in ('resource_registry', 'wire'):
                exe = out/f'{name}-{bits}.exe'
                extra = [ROOT/'tools/render_host/protocol.cpp'] if name == 'wire' else []
                receipt['steps'].append(execute([cc[bits], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                    ROOT/f'AutoTest/test_render_host_d3d9_{name}.cpp', *extra, '-o', exe],
                    ROOT, out/f'{name}-build-{bits}', 180, env))
                cpu.append((name, bits, exe))
        target = 'tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe'; host = build/target
        receipt['steps'].append(execute(['ninja', '-C', build, '-j2', target], ROOT, out/'host-build', 600, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-n', target], ROOT, out/'host-no-work', 30, env))
        require('no work to do' in (out/'host-no-work.stdout').read_text('utf-8'), 'host not frozen')
        require(boundary == check_host_compile_boundary(build), 'host compile boundary changed')
        receipt['compileCommands'] = identity(build/'compile_commands.json')
        proxy = out/'warvk-d3d9-proxy-dev.dll'; fixture = out/'d3d9-memory32.exe'
        receipt['steps'].append(execute([cc[32], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-Wno-microsoft-exception-spec', '-static', '-pthread', '-shared',
            '-Wl,--kill-at', '-DNOMINMAX', '-D_WIN32_WINNT=0x0a00', ROOT/'tools/render_host/d3d9_proxy.cpp',
            ROOT/'tools/render_host/d3d9_proxy.def', *common, memory_source,
            '-luser32', '-lbcrypt', '-ladvapi32', '-luuid', '-lpsapi', '-o', proxy],
            ROOT, out/'proxy-build', 240, env))
        receipt['steps'].append(execute([cc[32], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
            '-pthread', '-municode', '-D_WIN32_WINNT=0x0a00', ROOT/'AutoTest/test_render_host_d3d9_memory.cpp',
            *common, memory_source, '-luser32', '-lbcrypt', '-ladvapi32', '-lpsapi', '-o', fixture],
            ROOT, out/'fixture-build', 180, env))
        require(pe_identity(proxy) == pe_identity(fixture) == pe_identity(reference) == (0x14c, 0x10b), 'actual x86 inputs')
        require(pe_identity(host) == (0x8664, 0x20b), 'actual x64 host')
        for bits, width in ((32, (0x14c, 0x10b)), (64, (0x8664, 0x20b))):
            require(pe_identity(probes[bits]) == width, 'actual probe width')
        receipt['binaries'] = [identity(path) for path in (proxy, fixture, host, *probes.values(), *(row[2] for row in cpu))]
        with IsolatedRenderDesktop(restricted_medium=args.restricted_medium) as desktop:
            receipt['desktopName'] = desktop.desktop_name; receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name, '--expect-station', 'WinSta0',
                '--input-desktop', desktop.input_before, '--child', probes[64], '--child-log', out/'desktop-child.log'],
                cwd=out, stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['desktopProbe'] = probe
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'desktop-child.log'])
            check_launch(probe, desktop.desktop_name, desktop.input_before)
            rows = json_lines(out/'probe.stdout') + json_lines(out/'desktop-child.log')
            receipt['probeReports'] = rows; require(len(rows) == 2, 'actual CPU probe population')
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station='WinSta0', bits=32, child=rows[1], launch=probe)
            for name, bits, exe in cpu:
                launch = desktop.run([exe], cwd=out, stdout_path=out/f'{name}-{bits}.stdout',
                    stderr_path=out/f'{name}-{bits}.stderr', timeout_s=30, env=env)
                receipt['stepRawArtifacts'].append(capture_raw_artifacts(out, [f'{name}-{bits}.stdout', f'{name}-{bits}.stderr']))
                receipt['steps'].append(launch); check_launch(launch, desktop.desktop_name, desktop.input_before)
                text = (out/f'{name}-{bits}.stdout').read_text('utf-8')
                require(re.search(rf'checks=\d+ failures=0 bits={bits}\b', text) is not None, 'native CPU registry/wire failed')
            directory = out/'memory-account'; directory.mkdir()
            case = dict(label='memory-account', ok=False, deadlineSeconds=45); receipt['cases'].append(case)
            local_fixture = copy_artifact(fixture, directory/fixture.name)
            local_proxy = copy_artifact(proxy, directory/'proxy.dll')
            local_host = copy_artifact(host, directory/host.name)
            case_controls = dict(WARVK_D3D9_HOST_EXE=str(local_host), WARVK_D3D9_HOST_LOG=str(directory/'host.log'),
                                 DXVK_LOG_PATH=str(directory), DXVK_CONFIG_FILE=str(directory/'absent.conf'))
            case['processControls'] = controls | case_controls
            launch = desktop.run([local_fixture, '--mode', 'com', '--dll', local_proxy,
                '--expect-desktop', desktop.desktop_name, '--input-desktop', desktop.input_before],
                cwd=directory, stdout_path=directory/'case.stdout', stderr_path=directory/'case.stderr',
                timeout_s=45, env=env | case_controls)
            case['process'] = launch
            case['initialRawArtifacts'] = capture_raw_artifacts(directory, ['case.stdout', 'case.stderr', 'host.log'], True)
            case['artifacts'] = [identity(path) for path in sorted(directory.iterdir()) if path.is_file()]
            reports = json_lines(directory/'case.stdout'); hosts = json_lines(directory/'host.log')
            case['reports'] = reports; case['hostReports'] = hosts
            require(launch['ok'] is True, 'native memory case failed')
            case['memoryAccount'] = check_case(reports, hosts, launch, desktop.desktop_name, desktop.input_before)
            case['readerMutations'] = mutations(reports, hosts, launch, desktop.desktop_name, desktop.input_before)
            proxy_row = next(row for row in reports if row['schema'] == 'warvk-d3d9-proxy/v1')
            case['childSettlement'] = child_settled(proxy_row['childPid'], proxy_row['childCreation'])
            case['ok'] = True
        require(receipt['desktopSafety']['ok'] is True, 'isolated desktop cleanup failed')
        require(receipt['compileCommands'] == identity(build/'compile_commands.json'), 'compile database changed')
        require(receipt['binaries'] == [identity(Path(row['path'])) for row in receipt['binaries']], 'binaries changed')
        require(all(row['copy'] == identity(Path(row['copy']['path'])) for row in receipt['caseCopies']), 'executed copies changed')
        require(len(receipt['cases']) == 1 and receipt['cases'][0]['ok'] is True, 'memory case population')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            if 'referenceBefore' in receipt:
                receipt['referenceAfter'] = identity(Path(receipt['referenceBefore']['path']))
                receipt['playerAfter'] = identity(Path(receipt['playerBefore']['path']))
                receipt['playerDllChanged'] = receipt['playerBefore'] != receipt['playerAfter']
                require(receipt['referenceBefore'] == receipt['referenceAfter'] and not receipt['playerDllChanged'],
                        'reference/player changed')
            if 'sources' in receipt:
                receipt['sourcesPreserved'] = paths == source_set() and before == [identity(path) for path in sorted(paths)]
                require(receipt['sourcesPreserved'], 'source set changed during gate')
            receipt['sourceAndReferencePreserved'] = receipt.get('sourcesPreserved', False) and receipt['playerDllChanged'] is False
            captured = list(raw_manifests(receipt))
            for manifest in captured: verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(captured)
            if receipt['ok']: verify_raw_receipt(receipt, out)
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'error', 'scope', 'sourceAndReferencePreserved', 'memorySavingsProven')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
