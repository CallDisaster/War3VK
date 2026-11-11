"""M0 live destroy mutation, using a frozen successful NEW M0 receipt.

Root-owned native/build lease only. Never edits a live source/object/executable.
Compile one copied backend TU with the original flags/cwd and relink the frozen
host inputs. The negative must be the first drained owned-account refusal; a
crash, timeout, early failure, or failed child exit is NOT a killed mutation.
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

import run_render_host_memory_gate as memory
from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, child_settled, strict_json
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt

TARGET = Path('tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe')
BACKEND = ROOT/'tools/render_host/dxvk_core/d3d9_backend.cpp'
ERROR = 'warvk-d3d9-memory-error/v1'
EXPECTED_ERROR = 'owned CPU account valid'
HOST_COUNTS = dict(accepted=26, rejected=0, backendCalls=20, submitted=8, gpuCompleted=8, retired=8,
    draws=1, clears=1, uploads=3, uploadedBytes=176, readbacks=1, queries=0, buffersCreated=2,
    texturesCreated=1, shadersCompiled=2, declarationsCreated=1, logicalHandlesCreated=7,
    logicalHandlesDestroyed=7, logicalHandlesReused=0, resourcesReused=0, queryHandlesCreated=1,
    queryHandlesDestroyed=1, queryHandlesReused=0, queryIssues=0, queryPolls=0)


def windows_argv(command):
    """MS CRT-style argv, not POSIX shlex (which corrupts Windows backslashes).

    All execution is shell=False. Shell/response-file recipes are refused, not
    approximated. Original command text AND unchanged tokens enter the receipt.
    """
    require(type(command) is str and command and not any(c in command for c in '\r\n\0'), 'single command required')
    result = []; pos = 0
    while pos < len(command):
        while pos < len(command) and command[pos] in ' \t': pos += 1
        if pos == len(command): break
        word = []; quoted = False
        while pos < len(command) and (quoted or command[pos] not in ' \t'):
            slashes = 0
            while pos < len(command) and command[pos] == '\\': slashes += 1; pos += 1
            if pos < len(command) and command[pos] == '"':
                word.extend('\\' * (slashes // 2))
                if slashes % 2: word.append('"'); pos += 1
                elif quoted and pos+1 < len(command) and command[pos+1] == '"': word.append('"'); pos += 2
                else: quoted = not quoted; pos += 1
            else:
                word.extend('\\' * slashes)
                if pos == len(command) or (not quoted and command[pos] in ' \t'): break
                word.append(command[pos]); pos += 1
        require(not quoted, 'unclosed command quote')
        result.append(''.join(word))
    require(result and all(x not in ('&', '&&', '|', '||', '>', '>>', '<', ';') and not x.startswith('@')
                           for x in result), 'shell/response-file command unsupported')
    return result


def one_option(argv, flag):
    places = [i for i, value in enumerate(argv) if value == flag]
    require(len(places) == 1 and places[0]+1 < len(argv), 'one exact '+flag+' option required')
    return places[0]+1


def path_arg(value, cwd):
    return (Path(cwd)/value).resolve()


def mutate_source(text):
    old = '        slot = Owner::Buffer{};'
    require(text.count(old) == 1, 'exact original Buffer destroy anchor required')
    start = text.index('void D3D9Backend::destroy(')
    require(text.index(old) > start, 'mutation must be in actual destroy')
    replacement = ('        // M0 MUTATION ONLY: deliberately keep dead IB allocation.\n'
        '        auto retainedIndexBytes = std::move(slot.indexBytes);\n'
        '        slot = Owner::Buffer{};\n'
        '        slot.indexBytes = std::move(retainedIndexBytes);\n'
        '        slot.indexBytes.clear();')
    return text.replace(old, replacement, 1)


def compile_plan(entry, copied_source, obj, dep):
    cwd = Path(entry['directory']).resolve()
    require(path_arg(entry['file'], cwd) == BACKEND.resolve(), 'exact backend compile entry')
    argv = list(entry['arguments']) if 'arguments' in entry else windows_argv(entry['command'])
    require(all(type(x) is str for x in argv), 'compile argv strings')
    require(all(not x.startswith('@') and x not in ('&', '&&', '|', '||', '>', '>>', '<', ';') for x in argv),
            'compile response-file/shell recipe unsupported')
    src = one_option(argv, '-c'); out = one_option(argv, '-o')
    mf = one_option(argv, '-MF'); mq = one_option(argv, '-MQ')
    require(path_arg(argv[src], cwd) == BACKEND.resolve(), 'compiler input differs from entry')
    original_obj = path_arg(entry['output'], cwd)
    require(path_arg(argv[out], cwd) == original_obj and path_arg(argv[mq], cwd) == original_obj,
            'compiler output/MQ identity')
    require(path_arg(argv[mf], cwd) == Path(str(original_obj)+'.d'), 'original dependency identity')
    changed = list(argv); changed[src] = str(copied_source); changed[out] = changed[mq] = str(obj); changed[mf] = str(dep)
    # The copied directory contains only this CPP. Relative quoted includes must
    # resolve against the original source directory, not an invented header tree.
    changed += ['-iquote', str(BACKEND.parent)]
    require(all(a == b for i, (a, b) in enumerate(zip(argv, changed)) if i not in (src, out, mf, mq)),
            'unexpected compile-token change')
    return dict(cwd=str(cwd), originalArgv=argv, argv=changed, originalObject=str(original_obj),
                substitutions=dict(source=src, output=out, dependency=mf, dependencyTarget=mq),
                addedInclude=['-iquote', str(BACKEND.parent)])


def link_plan(commands, cwd, original_obj, replacement_obj, output):
    candidates = []
    for text in commands.splitlines():
        if ' -o ' not in text: continue
        argv = windows_argv(text)
        if argv.count('-o') == 1 and path_arg(argv[one_option(argv, '-o')], cwd) == (Path(cwd)/TARGET).resolve():
            candidates.append((text, argv))
    require(len(candidates) == 1, 'one exact original Ninja host linker command')
    text, argv = candidates[0]; out = one_option(argv, '-o')
    matches = [i for i, value in enumerate(argv) if not value.startswith('-') and path_arg(value, cwd) == Path(original_obj)]
    require(len(matches) == 1, 'one original backend object in linker command')
    changed = list(argv); changed[out] = str(output); changed[matches[0]] = str(replacement_obj)
    require(all(a == b for i, (a, b) in enumerate(zip(argv, changed)) if i not in (out, matches[0])),
            'unexpected linker-token change')
    return dict(cwd=str(cwd), originalText=text, originalArgv=argv, argv=changed,
                substitutions=dict(output=out, backendObject=matches[0]))


def declared_link_inputs(query, cwd, link):
    lines = query.splitlines(); inside = False; inputs = []
    for line in lines:
        if line == '  input: cpp_LINKER': inside = True; continue
        if line.startswith('  outputs:'): inside = False
        if not inside: continue
        value = line.strip()
        if value.startswith('|| '): value = value[3:]
        elif value.startswith('| '): value = value[2:]
        if Path(value).suffix.lower() in ('.obj', '.o', '.a', '.lib'):
            inputs.append(path_arg(value, cwd))
    require(inputs and len(inputs) == len(set(inputs)), 'unique declared link inputs required')
    explicit = {path_arg(x, cwd) for x in link['originalArgv'] if not x.startswith('-')
                and Path(x).suffix.lower() in ('.obj', '.o', '.a', '.lib')}
    require(explicit <= set(inputs), 'Ninja query omitted explicit link input')
    for arg in link['originalArgv']:
        if arg.startswith('-l'):
            require(any(p.name == 'lib'+arg[2:]+'.a' for p in inputs), 'unresolved explicit system library '+arg)
    return sorted(inputs)


def thin_archive_members(query, listing, cwd, archive):
    """Bind Ninja's direct object inputs to llvm-ar's actual external members.

    A thin archive's own hash does not include its object bytes. This bounded
    gate supports the current object-only archives; nested/unmodelled members
    are refused rather than silently treated as embedded archive contents.
    """
    cwd = Path(cwd).resolve(); archive = Path(archive).resolve()
    lines = query.splitlines()
    require(lines and lines[0].endswith(':')
            and path_arg(lines[0][:-1], cwd) == archive, 'thin archive query target identity')
    require(lines.count('  input: STATIC_LINKER') == 1 and lines.count('  outputs:') == 1,
            'one exact thin archive static-link query')
    start = lines.index('  input: STATIC_LINKER')+1; end = lines.index('  outputs:')
    require(start <= end, 'thin archive query order')
    declared = []
    for line in lines[start:end]:
        require(line.startswith('    '), 'thin archive query member indentation')
        value = line.strip()
        # Implicit scripts/tool dependencies are not archive members. They are
        # already frozen by the baseline source/build identity, where applicable.
        if value.startswith('| '): continue
        require(value and not value.startswith('|'), 'unexpected thin archive dependency')
        declared.append(path_arg(value, cwd))
    names = listing.splitlines()
    require(names and all(n and n == n.strip() and '\0' not in n for n in names),
            'nonempty exact llvm-ar member listing')
    members = [path_arg(name, cwd) for name in names]
    require(len(declared) == len(set(declared)) and len(members) == len(set(members))
            and set(declared) == set(members), 'thin archive actual/declared members differ')
    require(all(p.is_relative_to(cwd) and p.suffix.lower() in ('.obj', '.o') for p in members),
            'thin archive external/nested/non-object member unsupported')
    return sorted(members)


def same_file(row):
    memory.fields(row, ('path', 'bytes', 'sha256'), 'file identity')
    require(row == identity(Path(row['path'])), 'file changed: '+row['path'])
    return row


def verify_baseline(path):
    baseline = strict_json(path.read_text('utf-8'))
    require(baseline['schema'] == 1 and baseline['scope'] == 'M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT'
        and baseline['ok'] is True and baseline['sourcesPreserved'] is True
        and baseline['sourceAndReferencePreserved'] is True and baseline['playerDllChanged'] is False,
        'successful frozen new M0 baseline required')
    require(len(baseline['cases']) == 1 and baseline['cases'][0]['ok'] is True, 'one baseline native case')
    require({str(p.resolve()) for p in memory.source_set()} == {str(Path(r['path']).resolve()) for r in baseline['sources']},
            'current source set differs from successful baseline')
    for group in ('sources', 'binaries', 'compilers'):
        for row in baseline[group]: same_file(row)
    for name in ('compileCommands', 'referenceBefore', 'referenceAfter', 'playerBefore', 'playerAfter'): same_file(baseline[name])
    for pair in baseline['caseCopies']:
        same_file(pair['source']); same_file(pair['copy'])
        require(all(pair['source'][n] == pair['copy'][n] for n in ('bytes', 'sha256')), 'baseline executed copy mismatch')
    case = baseline['cases'][0]
    for row in case['artifacts']: same_file(row)
    artifact = {Path(row['path']).name: Path(row['path']) for row in case['artifacts']}
    reports = json_lines(artifact['case.stdout']); hosts = json_lines(artifact['host.log'])
    memory.check_case(reports, hosts, case['process'], baseline['desktopName'], baseline['desktopSafety']['inputBefore'])
    summary = next(row for row in reports if row['schema'] == memory.SUMMARY)
    require(summary['cpuHelperChecks'] == 54, 'pre-dead-slot M0 baseline cannot authorize this mutation')
    active = memory.decode_host(reports[1]['hostWireHex'])
    require(active['cpu']['kinds']['indexBuffer']['cpuCapacityBytes'] == 12,
            'this bounded mutation requires the measured 12-byte IB capacity')
    return baseline, reports, hosts


def check_baseline_process_policy(baseline, restricted_medium):
    require(type(restricted_medium) is bool
            and baseline['restrictedMediumRequested'] is restricted_medium
            and baseline['desktopSafety']['restrictedMediumRequested'] is restricted_medium,
            'mutation token policy must match the passing baseline')


def check_negative_launch(row, desktop, input_desktop):
    # Do not rewrite a failed process to a synthetic success for check_launch.
    require(type(row) is dict and row['ok'] is False and row['exitCode'] == 1
            and native_uint(row['exitCode'], 32), 'expected fixture assertion exit 1 only')
    require(type(row['argv']) is list and row['argv'] and type(row['actualExecutable']) is str
            and Path(row['actualExecutable']).resolve() == Path(row['argv'][0]).resolve(), 'actual negative executable identity')
    for name in ('jobAtomic', 'handleListApplied', 'processSettled', 'jobEmpty'): require(row[name] is True, 'negative launch '+name)
    for name in ('timedOut', 'forcedTermination', 'desktopHandleInherited'): require(row[name] is False, 'negative launch '+name)
    require(row['cleanupErrors'] == [] and native_uint(row['jobActiveProcesses'], 32)
            and row['jobActiveProcesses'] == 0 and not row.get('error'), 'negative launch must settle normally')
    for name, bits in (('pid', 32), ('creation100ns', 64), ('initialThreadId', 32)):
        require(native_uint(row[name], bits) and row[name] > 0, 'negative launch identity '+name)
    require(row['desktop'] == desktop and row['inputBefore'] == row['inputAfter'] == input_desktop
            and desktop != input_desktop, 'negative isolated desktop identity')


def check_mutation_desktop_cleanup(receipt, baseline_reports, baseline_hosts):
    """Accept expected-negative execution separately from raw Desktop.ok.

    The launcher rightly reports ok=False for assertion exit 1. Do not change
    that observation. Its finalizers close the process/thread/Job/std handles;
    an empty settled Job plus no cleanup errors is their closure witness, not
    an independently sampled process-handle census.
    """
    raw = receipt['desktopSafety']; desktop = receipt['desktopName']; initial = raw['inputBefore']
    require(type(raw) is dict and type(raw['schema']) is int and raw['schema'] == 1
            and raw['scope'] == 'NON_INPUT_DESKTOP_LAUNCH_CONTEXT', 'exact Desktop context')
    require(raw['ok'] is False, 'raw aggregate must retain expected-negative false')
    for key in ('desktopCreated', 'desktopClosed', 'creatorExited'):
        require(raw[key] is True, 'Desktop cleanup '+key)
    for key in ('desktopHandleInheritable', 'desktopSwitchRequested', 'globalInputUsed', 'gameTouched'):
        require(raw[key] is False, 'Desktop cleanup forbidden '+key)
    require(type(desktop) is str and desktop and type(initial) is str and initial
            and desktop.casefold() != initial.casefold() and raw['desktop'] == desktop
            and raw['windowStation'] == 'WinSta0' and raw['desktopPath'] == 'WinSta0\\'+desktop,
            'exact non-input Desktop context identity')
    require(type(raw['launcherDesktop']) is str and raw['launcherDesktop']
            and raw['cleanupErrors'] == [] and not raw.get('bodyError') and not raw.get('error'),
            'Desktop context must finish without cleanup/body error')
    for key in ('inputAfterCreate', 'inputBeforeLaunch', 'inputAfterLaunch', 'inputAfterRun', 'inputAfterClose'):
        require(raw[key] == initial, 'Desktop context changed input at '+key)
    restricted = receipt['restrictedMediumRequested']
    require(type(restricted) is bool and raw['restrictedMediumRequested'] is restricted,
            'Desktop requested token policy')
    probe = receipt['probe']; launch = receipt['process']
    require(type(raw['launches']) is list and len(raw['launches']) == 2
            and raw['launches'] == [probe, launch], 'exactly probe then certified negative launches')
    check_launch(probe, desktop, initial)
    require(not probe.get('error'), 'probe launch exception')
    rows = receipt['probeReports']
    require(type(rows) is list and len(rows) == 2, 'two actual CPU probe witnesses')
    validate_probe_receipt(rows[0], desktop=desktop, input_desktop=initial,
                           station='WinSta0', bits=32, child=rows[1], launch=probe)
    require(rows[0]['main']['threadId'] == probe['initialThreadId']
            and (probe['pid'], probe['creation100ns']) != (launch['pid'], launch['creation100ns']),
            'distinct exact probe and mutation launch owners')
    observed = check_negative(receipt['reports'], receipt['hostReports'], launch,
                              desktop, initial, baseline_reports, baseline_hosts)
    require(receipt['mutation'] == observed, 'stored mutation verdict differs from raw evidence')
    require(receipt['childSettlement'] in ('process-object-absent', 'pid-reused-original-object-gone',
                                          'exact-process-handle-signaled'), 'independent child settlement required')
    for row in (probe, launch):
        if not restricted:
            require('restrictedToken' not in row, 'unexpected restricted token without requested policy')
            continue
        token = row['restrictedToken']
        require(type(token) is dict and type(token['schema']) is int and token['schema'] == 1
                and token['mode'] == 'restricted-medium-per-child/v1', 'restricted token receipt')
        for key in ('ready', 'parentTokenUnchanged', 'derivedTokenClosed', 'childTokenVerifiedBeforeResume',
                    'noHighIntegrityFallback', 'resumedAfterVerification'):
            require(token[key] is True, 'restricted token cleanup '+key)
        require(token['globalSecurityChanged'] is False and token['cleanupErrors'] == []
                and native_uint(token['resumePreviousCount'], 32) and token['resumePreviousCount'] == 1,
                'restricted token cleanup or resume failure')
    return dict(accepted=True, rawDesktopOk=False, rawNegativeLaunchOk=False, expectedFixtureExit=1,
        launchCount=2, cpuProbePassed=True, specificNegativeRevalidated=True, desktopClosed=True,
        allJobsEmpty=True, forcedTermination=False, inputDesktopUnchanged=True,
        handleClosureEvidence='owned-launch-finalizers-no-errors-and-desktop-close',
        independentHandleCensusMeasured=False, rawEvidenceRewritten=False)


def decode_dead_host(wire):
    """Decode the deliberate INVALID account without normalizing its bytes."""
    require(type(wire) is str and re.fullmatch('[0-9a-f]{1024}', wire), 'exact negative host wire')
    raw = bytes.fromhex(wire)
    names = ('magic', 'version', 'phase', 'ordinal', 'pid', 'thread', 'bits', 'logicalResources',
        'creation', 'nonceLow', 'nonceHigh', 'sequence', 'tickBegin', 'tickEnd', 'logicalBytes',
        'submitted', 'completed', 'retired', 'draws', 'uploads', 'readbacks', 'fixedPacketPayloadBytes', 'ownerInlineBytes')
    result = dict(zip(names, struct.unpack_from('<8I15Q', raw)))
    memory.uints(result, dict(magic=0x314d3944, version=1, phase=4, ordinal=1, bits=64, sequence=24,
        logicalResources=0, logicalBytes=0, submitted=8, completed=8, retired=8, draws=1, uploads=3,
        readbacks=1, fixedPacketPayloadBytes=131072))
    os_values = struct.unpack_from('<2I7Q', raw, 152)
    require(os_values[0] == 1, 'negative host OS measurement must still be valid')
    result['process'] = dict(zip(('valid', 'win32', *memory.OS_FIELDS), (True, *os_values[1:])))
    memory.os_snapshot(result['process'], 64)
    valid, reserved = struct.unpack_from('<2I', raw, 216)
    require(valid == 0 and reserved == 0 and raw[480:] == bytes(32), 'dead-slot invalid bit/reserved contract')
    result['cpu'] = dict(valid=False, kinds={})
    for index, kind in enumerate(memory.KINDS):
        values = struct.unpack_from('<4Q', raw, 224+index*32)
        # slot={} intentionally resets index16=false: the dead IB allocation is
        # visible in the now-empty Buffer slot classified as vertexBuffer.
        require(values == (0, 0, 0, 12 if index == 0 else 0), 'specific dead IB capacity not observed: '+kind)
        result['cpu']['kinds'][kind] = dict(zip(memory.ACCOUNT_FIELDS, values))
    require(result['tickBegin'] <= result['tickEnd'] and result['ownerInlineBytes'] > 0, 'negative host observation window')
    return result


def check_negative(reports, hosts, launch, desktop, input_desktop, baseline_reports, baseline_hosts):
    check_negative_launch(launch, desktop, input_desktop)
    require([r.get('schema') for r in reports] == [memory.CHECKPOINT]*3+[memory.PROXY, ERROR],
            'only first three passing checkpoints then normal proxy and exact error')
    require([r.get('schema') for r in hosts] == [memory.DESKTOP]+[memory.MIRROR]*4+[memory.HOST],
            'four actual host observations and normal host teardown required')
    error = reports[-1]; memory.fields(error, ('schema', 'ok', 'error'), 'exact native assertion')
    require(error['ok'] is False and error['error'] == EXPECTED_ERROR, 'unrelated error is not mutation detection')
    summary = next(r for r in baseline_reports if r['schema'] == memory.SUMMARY)
    baseline_proxy = next(r for r in baseline_reports if r['schema'] == memory.PROXY)
    baseline_host = next(r for r in baseline_hosts if r['schema'] == memory.HOST)
    proxy = reports[-2]; host = hosts[-1]; ds = hosts[0]
    memory.fields(proxy, baseline_proxy, 'negative proxy')
    for name, value in baseline_proxy.items():
        if name not in ('childPid', 'childCreation', 'primaryExitWaitMs', 'exchanges'):
            require(type(proxy[name]) is type(value) and proxy[name] == value, 'proxy final '+name)
    memory.uints(proxy, dict(exchanges=26)); memory.unsigned(proxy, ('childPid',), 32)
    memory.unsigned(proxy, ('childCreation', 'primaryExitWaitMs'))
    require(proxy['childPid'] > 0 and proxy['childCreation'] > 0 and proxy['primaryExitWaitMs'] <= 5000,
            'actual child identity/unchanged primary-exit deadline')
    memory.fields(host, baseline_host, 'negative host')
    for name, value in baseline_host.items():
        expected = HOST_COUNTS.get(name, value)
        require(type(host[name]) is type(expected) and host[name] == expected, 'host final '+name)
    memory.fields(ds, ('schema', 'pid', 'threadId', 'peerPid', 'windowThreadId',
                      'windowStation', 'desktop', 'inputDesktop', 'beforeGpu'), 'desktop record')
    memory.uints(ds, dict(pid=proxy['childPid'], peerPid=launch['pid'], windowThreadId=0))
    memory.unsigned(ds, ('threadId',), 32)
    require(ds['threadId'] > 0 and ds['pid'] != launch['pid'] and ds['beforeGpu'] is True
        and ds['windowStation'] == 'WinSta0' and ds['desktop'] == desktop and ds['inputDesktop'] == input_desktop,
        'independent host desktop/owner identity')
    nonce = None; end = None; parent_peak = 0; host_peak = 0
    for index in range(3):
        cp = reports[index]; mirror = hosts[index+1]; original = baseline_reports[index]
        memory.fields(cp, original, 'passing-prefix checkpoint'); memory.fields(cp['parent'], original['parent'], 'passing-prefix parent')
        memory.fields(mirror, ('schema', 'phase', 'ordinal', 'sequence', 'wireHex'), 'prefix mirror')
        phase, ordinal = memory.SAMPLES[index]; sequence = memory.SEQUENCES[index]
        memory.uints(cp, dict(phase=phase, ordinal=ordinal)); memory.uints(mirror, dict(phase=phase, ordinal=ordinal, sequence=sequence))
        for name in ('synchronizedInstant', 'gpuPhysicalMeasured', 'opaqueCpuMeasured', 'gameTouched', 'productIntegrated', 'memorySavingsProven'):
            require(cp[name] is False, 'prefix scope '+name)
        p = cp['parent']; h = memory.decode_host(cp['hostWireHex'])
        require(cp['hostWireHex'] == mirror['wireHex'], 'prefix independent host mirror')
        for name, value in p.items():
            if name not in ('before', 'after', 'cpu'): require(native_uint(value), 'prefix native integer '+name)
        memory.uints(p, dict(version=1, phase=phase, ordinal=ordinal, pid=launch['pid'], thread=launch['initialThreadId'],
            creation=launch['creation100ns'], bits=32, expectedHostPid=proxy['childPid'], expectedHostCreation=proxy['childCreation'],
            expectedSequence=sequence, logicalHandles=1 if index == 0 else 7, reservedHandles=0,
            privateBindings=6 if index == 2 else 0, publicZeroBindings=6 if index == 2 else 0,
            connectionInlineBytes=original['parent']['connectionInlineBytes'], fixedPacketPayloadBytes=393216, workerBatchPayloadBytes=65536))
        memory.uints(h, dict(pid=proxy['childPid'], creation=proxy['childCreation'], thread=ds['threadId'], phase=phase,
            ordinal=ordinal, sequence=sequence, nonceLow=p['expectedNonceLow'], nonceHigh=p['expectedNonceHigh'],
            submitted=memory.SERIALS[index], completed=memory.SERIALS[index], retired=memory.SERIALS[index],
            draws=memory.DRAWS[index], readbacks=memory.DRAWS[index], uploads=memory.UPLOADS[index], fixedPacketPayloadBytes=131072))
        for name in ('before', 'after'): memory.os_snapshot(p[name], 32)
        require(parent_peak <= p['before']['peakCommitBytes'] <= p['after']['peakCommitBytes']
            and host_peak <= h['process']['peakCommitBytes'], 'prefix lifetime OS peaks decreased')
        parent_peak = p['after']['peakCommitBytes']; host_peak = h['process']['peakCommitBytes']
        memory.cpu_account(p['cpu']); memory.check_owned_account(p, h, summary)
        original_h = memory.decode_host(original['hostWireHex'])
        require(p['cpu'] == original['parent']['cpu'] and h['cpu'] == original_h['cpu']
            and h['ownerInlineBytes'] == original_h['ownerInlineBytes'], 'prefix exact known same-workload accounts')
        observed_nonce = (h['nonceLow'], h['nonceHigh'])
        if nonce is None: nonce = observed_nonce
        require(nonce == observed_nonce, 'prefix cross-session identity')
        times = (p['tickBegin'], p['beforeEnd'], h['tickBegin'], h['tickEnd'], p['afterBegin'], p['tickEnd'])
        require(all(a <= b for a, b in zip(times, times[1:])) and (end is None or end <= times[0]), 'prefix time windows')
        end = times[-1]
    mirror = hosts[4]; memory.fields(mirror, ('schema', 'phase', 'ordinal', 'sequence', 'wireHex'), 'dead mirror')
    memory.uints(mirror, dict(phase=4, ordinal=1, sequence=24)); dead = decode_dead_host(mirror['wireHex'])
    memory.uints(dead, dict(pid=proxy['childPid'], creation=proxy['childCreation'], thread=ds['threadId'],
        nonceLow=nonce[0], nonceHigh=nonce[1], ownerInlineBytes=memory.decode_host(reports[0]['hostWireHex'])['ownerInlineBytes']))
    require(end <= dead['tickBegin'], 'dead checkpoint was not after successful private draw')
    require(host_peak <= dead['process']['peakCommitBytes'], 'dead checkpoint lifetime OS peak decreased')
    return dict(killed=True, mechanism='real-Buffer-destroy-retains-dead-IB-vector-capacity',
        firstRefusedPhase=4, firstRefusedOrdinal=1, firstRefusedSequence=24,
        earlierPassingCheckpoints=3, hostInvalidObservation=dead, fixtureExpectedExit=1,
        childExit=proxy['childExit'], childPrimaryExitWaitMs=proxy['primaryExitWaitMs'],
        genericFailureAccepted=False, baselineRewritten=False, gameTouched=False, productIntegrated=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--restricted-medium', action='store_true')
    args = parser.parse_args(); out = args.output.resolve()
    require(out.is_relative_to((ROOT/'AutoTest/artifacts').resolve()), 'new project artifact directory required')
    out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='M0_NATIVE_DEAD_IB_DESTROY_MUTATION', ok=False,
        priority='BelowNormal', parallelism=1, maximumParallelism=2, steps=[], copies=[],
        restrictedMediumRequested=args.restricted_medium,
        liveSourceEdited=False, playerDllChanged=None, systemChanged=False, gameTouched=False,
        productIntegrated=False, memorySavingsProven=False, rootBuildNativeLeaseRequired=True)
    retained = []; identities = []; baseline = None
    def copy_artifact(source, target):
        original = identity(source); require(not target.exists(), 'new artifact copy only')
        shutil.copy2(source, target); copied = identity(target)
        require(all(original[n] == copied[n] for n in ('bytes', 'sha256')), 'copied artifact differs')
        receipt['copies'].append(dict(source=original, copy=copied)); return target
    try:
        require(os.name == 'nt', 'Windows native mutation only')
        baseline_path = args.baseline.resolve(strict=True); receipt['baselineReceipt'] = identity(baseline_path)
        baseline, passing, passing_hosts = verify_baseline(baseline_path)
        check_baseline_process_policy(baseline, args.restricted_medium)
        receipt['baselineRevalidated'] = True
        build = Path(baseline['compileCommands']['path']).parent; host = build/TARGET
        require(any(row == identity(host) for row in baseline['binaries']), 'current frozen host not in baseline')
        receipt['originalHost'] = identity(host); receipt['compileCommands'] = identity(build/'compile_commands.json')
        receipt['allowedSharedD3D9Inputs'] = check_host_compile_boundary(build)
        identities = baseline['sources']+baseline['binaries']+baseline['compilers']+[baseline['compileCommands'],
            baseline['referenceBefore'], baseline['playerBefore'], receipt['baselineReceipt'],
            identity(Path(__file__).resolve()), identity(ROOT/'AutoTest/test_render_host_memory_destroy_mutation.py')]
        env = {k: v for k, v in os.environ.items() if k.upper() in memory.ENV_ALLOW}
        controls = baseline['processControls']; require(type(controls) is dict, 'baseline process controls')
        require(set(controls) <= {'DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE', 'VK_LOADER_LAYERS_DISABLE',
            'VK_LOADER_DRIVERS_DISABLE', 'VK_LOADER_DRIVERS_SELECT'}, 'unsupported baseline process controls')
        env.update(controls); receipt['processControls'] = controls
        receipt['steps'].append(execute([sys.executable, '-B', ROOT/'AutoTest/test_render_host_memory_destroy_mutation.py'],
            ROOT, out/'reader-tests', 30, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-n', TARGET.as_posix()], ROOT, out/'frozen-no-work', 30, env))
        require('no work to do' in (out/'frozen-no-work.stdout').read_text('utf-8'), 'host build is not frozen')
        receipt['steps'].append(execute(['ninja', '-C', build, '-t', 'commands', TARGET.as_posix()], ROOT, out/'ninja-commands', 30, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-t', 'query', TARGET.as_posix()], ROOT, out/'ninja-query', 30, env))
        entries = strict_json((build/'compile_commands.json').read_text('utf-8'))
        entries = [e for e in entries if path_arg(e['file'], e['directory']) == BACKEND.resolve()]
        require(len(entries) == 1, 'single actual backend compile entry')
        variant = out/'substituted'; variant.mkdir(); source = variant/'d3d9_backend.cpp'
        original = BACKEND.read_bytes(); require(b'\r' not in original.replace(b'\r\n', b''), 'source newline form')
        changed = mutate_source(original.decode('utf-8').replace('\r\n', '\n'))
        source.write_bytes(changed.replace('\n', '\r\n').encode('utf-8') if b'\r\n' in original else changed.encode('utf-8'))
        receipt['originalSource'] = identity(BACKEND); receipt['mutatedSource'] = identity(source)
        obj = variant/'d3d9_backend.cpp.obj'; dep = variant/'d3d9_backend.cpp.obj.d'
        compile = compile_plan(entries[0], source, obj, dep)
        require(Path(compile['cwd']) == build.resolve(), 'original compiler working directory changed')
        compiler = path_arg(compile['argv'][0], build)
        require(any(row == identity(compiler) for row in baseline['compilers']), 'compiler not frozen by baseline')
        mutated_host = out/'warvk-d3d9-host-dev.exe'
        link = link_plan((out/'ninja-commands.stdout').read_text('utf-8'), build,
            Path(compile['originalObject']), obj, mutated_host)
        require(path_arg(link['argv'][0], build) == compiler, 'compiler/linker driver identity changed')
        retained = [identity(p) for p in declared_link_inputs((out/'ninja-query.stdout').read_text('utf-8'), build, link)]
        receipt.update(compilePlan=compile, linkPlan=link, retainedDeclaredLinkInputs=retained,
            implicitToolchainLibrariesIndividuallyMeasured=False)
        # llvm-ar only lists existing archive members. No archive is rewritten,
        # and every externally stored object is rehashed before/after execution.
        archiver = compiler.parent/'llvm-ar.exe'
        receipt['archiver'] = identity(archiver); identities.append(receipt['archiver'])
        identities.append(identity(build/'build.ninja'))
        thin = []; member_paths = set()
        for row in retained:
            archive = Path(row['path'])
            if archive.suffix.lower() not in ('.a', '.lib'): continue
            with archive.open('rb') as stream: magic = stream.read(8)
            require(magic in (b'!<arch>\n', b'!<thin>\n'), 'unmodelled archive format: '+str(archive))
            if magic != b'!<thin>\n': continue
            require(archive.is_relative_to(build), 'thin archive outside frozen build')
            prefix = out/f'thin-archive-{len(thin):02d}'; target = archive.relative_to(build).as_posix()
            receipt['steps'].append(execute(['ninja', '-C', build, '-t', 'query', target], ROOT, prefix.with_name(prefix.name+'-query'), 30, env))
            receipt['steps'].append(execute([archiver, 't', target], build, prefix.with_name(prefix.name+'-members'), 30, env))
            members = thin_archive_members(prefix.with_name(prefix.name+'-query.stdout').read_text('utf-8'),
                prefix.with_name(prefix.name+'-members.stdout').read_text('utf-8'), build, archive)
            member_paths.update(members); same_file(row)
            thin.append(dict(archive=row, members=[str(p) for p in members]))
        member_rows = [identity(p) for p in sorted(member_paths)]
        receipt['thinArchives'] = thin; receipt['retainedThinArchiveMemberInputs'] = member_rows
        retained = retained+member_rows
        for row in identities+retained: same_file(row)
        receipt['steps'].append(execute(compile['argv'], build, out/'variant-compile', 180, env))
        receipt['steps'].append(execute(link['argv'], build, out/'variant-link', 180, env))
        receipt['mutatedObject'] = identity(obj); receipt['mutatedHost'] = identity(mutated_host)
        require(pe_identity(mutated_host) == (0x8664, 0x20b), 'real x64 mutated host required')
        for row in identities+retained: same_file(row)
        by_name = {Path(row['path']).name: Path(row['path']) for row in baseline['binaries']}
        fixture = copy_artifact(by_name['d3d9-memory32.exe'], out/'d3d9-memory32.exe')
        proxy = copy_artifact(by_name['warvk-d3d9-proxy-dev.dll'], out/'proxy.dll')
        probes = {bits: copy_artifact(by_name[f'desktop-probe-{bits}.exe'], out/f'desktop-probe-{bits}.exe') for bits in (32, 64)}
        with IsolatedRenderDesktop(restricted_medium=args.restricted_medium) as desktop:
            receipt['desktopSafety'] = desktop.receipt; receipt['desktopName'] = desktop.desktop_name
            probe = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name, '--expect-station', 'WinSta0',
                '--input-desktop', desktop.input_before, '--child', probes[64], '--child-log', out/'probe-child.log'],
                cwd=out, stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['probe'] = probe; check_launch(probe, desktop.desktop_name, desktop.input_before)
            rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log'); require(len(rows) == 2, 'two actual desktop probes')
            receipt['probeReports'] = rows
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                station='WinSta0', bits=32, child=rows[1], launch=probe)
            case_controls = dict(WARVK_D3D9_HOST_EXE=str(mutated_host), WARVK_D3D9_HOST_LOG=str(out/'host.log'),
                DXVK_LOG_PATH=str(out), DXVK_CONFIG_FILE=str(out/'absent.conf'))
            case_env = env | case_controls; receipt['caseProcessControls'] = controls | case_controls
            receipt['nativeDeadlines'] = dict(fixtureSeconds=45, childPrimaryExitMilliseconds=5000)
            launch = desktop.run([fixture, '--mode', 'com', '--dll', proxy, '--expect-desktop', desktop.desktop_name,
                '--input-desktop', desktop.input_before], cwd=out, stdout_path=out/'case.stdout',
                stderr_path=out/'case.stderr', timeout_s=45, env=case_env)
            receipt['process'] = launch; reports = json_lines(out/'case.stdout'); hosts = json_lines(out/'host.log')
            receipt['reports'] = reports; receipt['hostReports'] = hosts
            receipt['mutation'] = check_negative(reports, hosts, launch, desktop.desktop_name, desktop.input_before, passing, passing_hosts)
            final_proxy = reports[-2]
            receipt['childSettlement'] = child_settled(final_proxy['childPid'], final_proxy['childCreation'])
        receipt['desktopCleanupAcceptance'] = check_mutation_desktop_cleanup(receipt, passing, passing_hosts)
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            for row in identities+retained: same_file(row)
            for pair in receipt['copies']: same_file(pair['source']); same_file(pair['copy'])
            for name in ('mutatedSource', 'mutatedObject', 'mutatedHost'):
                if name in receipt: same_file(receipt[name])
            if baseline is not None:
                receipt['playerAfter'] = identity(Path(baseline['playerBefore']['path']))
                receipt['playerDllChanged'] = receipt['playerAfter'] != baseline['playerBefore']
                require(not receipt['playerDllChanged'], 'player DLL changed')
            receipt['frozenInputsPreserved'] = bool(identities and retained)
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        receipt['artifacts'] = [identity(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'receipt.json']
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: receipt.get(k) for k in ('ok', 'error', 'scope', 'frozenInputsPreserved', 'playerDllChanged')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
