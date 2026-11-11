"""Pure adversarial reader tests: no native process, compiler, Desktop or GPU.

The synthetic population/schema and literal hashes below are independent of
the reader's tables. They are test inputs, never native equivalence evidence.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_render_host_shader_cache_equivalence as gate
from isolated_render_desktop import DesktopSafetyError


PHASES = ('original-a', 'same-token-distinct-live-objects', 'original-a-restored',
    'recreated-while-peer-live', 'recreated-after-all-public-shaders-released',
    'only-immediate-constant-bytes-changed', 'original-constant-bytes-restored',
    'changed-token-distinct-live-objects', 'original-recreated-after-changed-shaders-destroyed')
TOKENS = ('b69b1c52fc6bdc95', 'dfe21ee2dc664ded', '7f95f2f6422c5e3d')
PIXELS = ('4550bdbe6ebe5fc5', '72a3f3156b4dc2e5')
ERRORS = (ValueError, KeyError, TypeError, DesktopSafetyError)


def medium():
    return dict(schema=1, mode='restricted-medium-per-child/v1', ready=True,
        parentTokenUnchanged=True, derivedTokenClosed=True, childTokenVerifiedBeforeResume=True,
        noHighIntegrityFallback=True, resumedAfterVerification=True, resumePreviousCount=1,
        globalSecurityChanged=False, cleanupErrors=[])


def launch(pid, desktop, argv):
    return dict(ok=True, argv=argv, actualExecutable=argv[0], jobActiveProcesses=0,
        jobAtomic=True, handleListApplied=True, processSettled=True, jobEmpty=True,
        timedOut=False, forcedTermination=False, desktopHandleInherited=False,
        pid=pid, creation100ns=pid * 100, initialThreadId=pid + 1000, exitCode=0,
        cleanupErrors=[], desktop=desktop, inputBefore='Default', inputAfter='Default',
        restrictedToken=medium())


def cpu_probe(bits, pid, desktop):
    main = dict(threadId=pid + 1000, station='WinSta0', desktop=desktop,
        inputBefore='Default', inputAfter='Default')
    return dict(schema=1, scope='NON_INPUT_DESKTOP_PROBE', bits=bits, ok=True,
        pid=pid, creation100ns=pid * 100, main=main,
        pump=dict(main, threadId=pid + 1001, windowCreated=True, windowHidden=True,
            windowDestroyed=True, classUnregistered=True, width=64, height=64,
            pings=1, messagesDispatched=2), childRequested=False, childDesktopExplicit=False,
        childPid=0, childCreation=0, childExit=0, gpuModulesAbsent=True, gameTouched=False,
        globalInputUsed=False, desktopSwitchUsed=False, normalTeardown=True)


def file_identity(path, empty=False):
    return dict(path=str(path.resolve()), bytes=0 if empty else 1, sha256='A' * 64)


def case(label='remote', pid=300, polls=79):
    reference = label != 'remote'; desktop = 'WarVK_Shader_Synthetic_' + label
    directory = gate.ROOT/'AutoTest'/'synthetic-reader-only'/label
    renderer = str((directory/'renderer.dll').resolve())
    argv = [str(directory/'shader-cache-equivalence32.exe'), '--dll', renderer,
        '--expect-desktop', desktop, '--input-desktop', 'Default']
    if reference: argv.append('--reference')
    process = launch(pid, desktop, argv)
    probe = launch(pid + 10, desktop, [str(directory/'desktop-probe-32.exe')])
    probes = [cpu_probe(32, pid + 10, desktop), cpu_probe(64, pid + 11, desktop)]
    probes[0].update(childRequested=True, childDesktopExplicit=True,
        childPid=probes[1]['pid'], childCreation=probes[1]['creation100ns'])
    frames = []
    for at in range(72):
        phase = at % 9; green = phase in (5, 7)
        frames.append(dict(schema='warvk-d3d9-shader-cache-equivalence-frame/v1',
            cycle=at // 9 + 1, phase=PHASES[phase], ordinal=at + 1,
            expectedBGRA=0xff00ff00 if green else 0xffff0000, pixelsChecked=3328,
            inner=1936, clear=1392, failures=0, maxChannelError=0,
            sampledPixelFnv=PIXELS[int(green)], sameColorStable=True))
    summary = dict(schema='warvk-d3d9-shader-cache-equivalence/v1', ok=True, bits=32,
        reference=reference, adapter='shipping-d3d9' if reference else 'x64-development-host',
        stage='complete', error='', publicD3D9Api=True, cycles=8, phasesPerCycle=9,
        frames=72, redFrames=56, greenFrames=16, pixelsChecked=239616, pixelFailures=0,
        maxChannelError=0, draws=72, vertexCreates=32, pixelCreates=56,
        vertexPublicZero=32, pixelPublicZero=56, shaderByteRoundtrips=88,
        simultaneousDistinctCOMPairs=24, allPublicShaderRefsReleased=8,
        queryIssues=72, queryPolls=polls, desktopChecks=77,
        vertexTokenFnv=TOKENS[0], redTokenFnv=TOKENS[1], greenTokenFnv=TOKENS[2],
        redSampledPixelFnv=PIXELS[0], greenSampledPixelFnv=PIXELS[1],
        signatureConsumedByDraw=True, definedConstantOutputVerified=True,
        needsConstantCopiesCovered=False,
        needsConstantCopiesReason='relative-operands-not-admitted-by-current-host-profile',
        cacheReuseCertifiedByFixture=False, cacheIdentityEvidence='separate-host-cache-observation-required',
        normalTeardown=True, deviceFinalRefs=0, factoryFinalRefs=0, resourceHeldDevice=True,
        deviceReleaseMs=2, deviceReleaseLimitMs=5000, parentVulkanAbsent=not reference,
        remoteChildExitVerifiedByFixture=False, windowOwnerClosed=True, windowClassUnregistered=True,
        desktop=desktop, inputBefore='Default', inputAfter='Default', globalInputUsed=False,
        desktopSwitched=False, gameTouched=False, productIntegrated=False, memorySavingsProven=False,
        windowPixelsVerified=False, playerVisualAccepted=False)
    proxy = dict(schema='warvk-d3d9-proxy/v1', childPid=pid + 1, childCreation=(pid + 1) * 100,
        childExit=0, exchanges=479 + polls, peerVerified=True, transportFailed=False,
        x86VulkanAbsent=True, deviceLost=False, remoteTerminal=False, childNaturalExit=True,
        childProcessSettled=True, childSelfFailStop=False, primaryExitWaitMs=2,
        productIntegrated=False, memorySavingsProven=False)
    host = dict(schema='warvk-d3d9-host/v1', ok=True, bits=64, actualDxvkCore=True,
        actualDxso=True, accepted=479 + polls, rejected=0, backendCalls=477 + polls,
        submitted=221, gpuCompleted=221, retired=221, draws=72, clears=72, uploads=3,
        uploadedBytes=176, readbacks=72, queries=72, buffersCreated=2, texturesCreated=1,
        shadersCompiled=88, declarationsCreated=1, liveResources=0, liveLogicalBytes=0,
        peakLogicalBytes=376, vendorId=4318, deviceId=1234, normalTeardown=True,
        presentImplemented=False, presentAttempts=0, acquiredImages=0, presentQueued=0,
        presentApiSucceeded=0, presentGpuCompleted=0, presentRetired=0,
        lastAcquireVkResult=1, lastPresentVkResult=1, windowProcessId=0, windowThreadId=0,
        gpuInitialized=True, shutdownQueueDrained=True, presentedReadbacks=0,
        presentedFrame=0, logicalHandlesCreated=93, logicalHandlesDestroyed=93,
        logicalHandlesReused=84, resourcesReused=84, queryHandlesCreated=1,
        queryHandlesDestroyed=1, queryHandlesReused=0, queryIssues=72, queryPolls=polls,
        liveLogicalHandles=0, liveQueries=0, productIntegrated=False,
        gameTouched=False, memorySavingsProven=False)
    host_desktop = dict(schema='warvk-d3d9-desktop/v1', pid=pid + 1, threadId=pid + 2000,
        peerPid=pid, windowThreadId=0, windowStation='WinSta0', desktop=desktop,
        inputDesktop='Default', beforeGpu=True)
    safety = dict(schema=1, scope='NON_INPUT_DESKTOP_LAUNCH_CONTEXT', ok=True, desktop=desktop,
        desktopCreated=True, desktopClosed=True, creatorExited=True, desktopHandleInheritable=False,
        desktopSwitchRequested=False, restrictedMediumRequested=True, globalInputUsed=False,
        gameTouched=False, launches=[probe, process], cleanupErrors=[], windowStation='WinSta0',
        launcherDesktop='Default', desktopPath='WinSta0\\' + desktop, inputBefore='Default',
        inputAfterCreate='Default', inputBeforeLaunch='Default', inputAfterLaunch='Default',
        inputAfterRun='Default', inputAfterClose='Default')
    return dict(label=label, ok=True, reports=frames + ([] if reference else [proxy]) + [summary],
        hostReports=[] if reference else [host_desktop, host], process=process,
        desktopName=desktop, desktopSafety=safety, probe=probe, probeReports=probes,
        childSettlement='exact-process-handle-signaled', loaderInvocation=dict(
            renderer=file_identity(Path(renderer)), absoluteDllArgument=renderer,
            modulePathIndependentlyReported=False))


def comparison():
    return [case('old-reference', 100), case('new-reference', 200), case('remote', 300)]


def check(row):
    return gate.check_case(row['reports'], row['hostReports'], row['process'], row['desktopName'],
        row['desktopSafety']['inputBefore'], row['label'] != 'remote')


class EquivalenceReaderTests(unittest.TestCase):
    def reject(self, row):
        with self.assertRaises(ERRORS): check(row)

    def reject_comparison(self, rows):
        with self.assertRaises(ERRORS): gate.check_comparison(rows)

    def test_literal_oracle_and_all_three_positive_cases(self):
        self.assertEqual(gate.token_hashes(), TOKENS)
        self.assertEqual((gate.pixel_hash(False), gate.pixel_hash(True)), PIXELS)
        rows = comparison(); before = copy.deepcopy(rows)
        result = gate.check_comparison(rows)
        self.assertEqual(result['frames'], 216)
        self.assertEqual(result['pixelsChecked'], 718848)
        self.assertEqual(result['independentProcesses'], 3)
        self.assertFalse(result['cacheReuseCertified'])
        self.assertFalse(result['loadedModulePathIndependentlyObserved'])
        self.assertFalse(result['memorySavingsProven'])
        self.assertEqual(rows, before)

    def test_all_summary_fields_required_and_unknown_rejected(self):
        for field in case()['reports'][-1]:
            row = case(); del row['reports'][-1][field]
            with self.subTest(field=field): self.reject(row)
        row = case(); row['reports'][-1]['unknown'] = False; self.reject(row)

    def test_summary_exact_numbers_not_booleans_or_floats(self):
        for name, value in case()['reports'][-1].items():
            if type(value) is int:
                for bad in (True, float(value), -1, None):
                    row = case(); row['reports'][-1][name] = bad
                    with self.subTest(field=name, bad=bad): self.reject(row)

    def test_summary_claims_and_scope_are_exact(self):
        for name, value in case()['reports'][-1].items():
            if type(value) is bool:
                row = case(); row['reports'][-1][name] = not value
                with self.subTest(field=name): self.reject(row)
            elif type(value) is str:
                row = case(); row['reports'][-1][name] = value + '-changed'
                with self.subTest(field=name): self.reject(row)

    def test_exact_summary_populations(self):
        for name, value in case()['reports'][-1].items():
            if type(value) is int and name not in ('deviceReleaseMs', 'queryPolls'):
                row = case(); row['reports'][-1][name] = value + 1
                with self.subTest(field=name): self.reject(row)

    def test_all_frame_fields_required(self):
        for field in case()['reports'][0]:
            row = case(); del row['reports'][0][field]
            with self.subTest(field=field): self.reject(row)
        row = case(); row['reports'][0]['extra'] = 0; self.reject(row)

    def test_frame_oracle_counts_hashes_and_phase_are_independent(self):
        for name, value in case()['reports'][0].items():
            row = case()
            row['reports'][0][name] = not value if type(value) is bool else value + 1 if type(value) is int else value + '-bad'
            with self.subTest(field=name): self.reject(row)
        row = case(); row['reports'][5]['expectedBGRA'] = 0xffff0000; self.reject(row)
        row = case(); row['reports'][7]['sampledPixelFnv'] = PIXELS[0]; self.reject(row)
        row = case(); row['reports'][0]['inner'] = 1392; row['reports'][0]['clear'] = 1936; self.reject(row)

    def test_every_frame_ordinal_cycle_and_color_can_fail(self):
        for at in range(72):
            row = case(); row['reports'][at]['ordinal'] += 1
            with self.subTest(at=at): self.reject(row)
        row = case(); row['reports'][9]['cycle'] = 1; self.reject(row)
        row = case(); row['reports'][1] = copy.deepcopy(row['reports'][0]); self.reject(row)

    def test_frame_types_and_tolerance_aggregate(self):
        for field in ('cycle', 'ordinal', 'expectedBGRA', 'pixelsChecked', 'inner', 'clear', 'failures', 'maxChannelError'):
            row = case(); row['reports'][0][field] = True
            with self.subTest(field=field): self.reject(row)
        row = case(); row['reports'][0]['maxChannelError'] = 2; row['reports'][-1]['maxChannelError'] = 2; self.reject(row)
        row = case(); row['reports'][0]['maxChannelError'] = 1; self.reject(row)
        row['reports'][-1]['maxChannelError'] = 1
        self.assertTrue(check(row)['ok'])

    def test_stream_population_unknown_duplicate_and_order(self):
        for label in ('old-reference', 'remote'):
            original = case(label)
            for stream in ('reports', 'hostReports'):
                for at in range(len(original[stream])):
                    row = copy.deepcopy(original); del row[stream][at]
                    with self.subTest(label=label, stream=stream, at=at): self.reject(row)
                row = copy.deepcopy(original); row[stream].append(dict(schema='unknown/v1')); self.reject(row)
                if original[stream]:
                    row = copy.deepcopy(original); row[stream].append(copy.deepcopy(row[stream][-1])); self.reject(row)
                    row = copy.deepcopy(original); row[stream][0], row[stream][-1] = row[stream][-1], row[stream][0]; self.reject(row)

    def test_reference_cannot_carry_remote_or_false_mode(self):
        row = case('old-reference'); row['hostReports'] = case()['hostReports']; self.reject(row)
        row = case('old-reference'); row['reports'].insert(-1, case()['reports'][-2]); self.reject(row)
        row = case('old-reference'); row['reports'][-1]['reference'] = 1; self.reject(row)
        row = case(); row['reports'][-1]['parentVulkanAbsent'] = False; self.reject(row)
        row = case('new-reference'); row['reports'][-1]['parentVulkanAbsent'] = True
        self.assertTrue(check(row)['ok'])

    def test_proxy_all_fields_required_and_unknown(self):
        for name in case()['reports'][-2]:
            row = case(); del row['reports'][-2][name]
            with self.subTest(field=name): self.reject(row)
        row = case(); row['reports'][-2]['unknown'] = True; self.reject(row)

    def test_proxy_normal_exit_and_five_second_boundary(self):
        for name, bad in (('childExit', 2), ('childExit', True), ('childNaturalExit', False),
            ('childProcessSettled', False), ('childSelfFailStop', True), ('deviceLost', True),
            ('remoteTerminal', True), ('transportFailed', True), ('peerVerified', False),
            ('x86VulkanAbsent', False), ('primaryExitWaitMs', 5001), ('primaryExitWaitMs', True),
            ('primaryExitWaitMs', -1), ('exchanges', 1), ('productIntegrated', True), ('memorySavingsProven', True)):
            row = case(); row['reports'][-2][name] = bad
            with self.subTest(field=name, bad=bad): self.reject(row)
        row = case(); row['reports'][-2]['primaryExitWaitMs'] = 5000
        row['reports'][-1]['deviceReleaseMs'] = 5000
        self.assertTrue(check(row)['ok'])
        row['reports'][-1]['deviceReleaseMs'] = 5001; self.reject(row)

    def test_remote_exact_identity_not_bool_zero_or_overflow(self):
        for name, bits in (('childPid', 32), ('childCreation', 64)):
            for bad in (True, 0, -1, 1 << bits, 2.0):
                row = case(); row['reports'][-2][name] = bad
                with self.subTest(field=name, bad=bad): self.reject(row)
        row = case(); row['hostReports'][0]['pid'] += 1; self.reject(row)
        row = case(); row['hostReports'][0]['peerPid'] += 1; self.reject(row)
        row = case(); row['reports'][-2]['childPid'] = row['process']['pid']
        row['hostReports'][0]['pid'] = row['process']['pid']; self.reject(row)

    def test_all_host_fields_required_unknown_and_count_mutations(self):
        for name, value in case()['hostReports'][-1].items():
            row = case(); del row['hostReports'][-1][name]
            with self.subTest(missing=name): self.reject(row)
            row = case()
            row['hostReports'][-1][name] = not value if type(value) is bool else (0 if name in ('vendorId', 'deviceId') else value + 1) if type(value) is int else value + '-bad'
            with self.subTest(changed=name): self.reject(row)
        row = case(); row['hostReports'][-1]['unknown'] = True; self.reject(row)

    def test_host_and_summary_query_poll_relationship(self):
        self.assertTrue(check(case(polls=72))['ok'])
        self.assertTrue(check(case(polls=95))['ok'])
        self.reject(case(polls=71))
        row = case(); row['reports'][-1]['queryPolls'] += 1; self.reject(row)
        row = case(); row['hostReports'][-1]['backendCalls'] += 2; self.reject(row)
        # The legacy count remains CREATE requests; it must not be replaced
        # by the separate shared-cache actual compile observation (three).
        row = case(); row['hostReports'][-1]['shadersCompiled'] = 3; self.reject(row)

    def test_host_integer_type_strictness(self):
        for name, value in case()['hostReports'][-1].items():
            if type(value) is int:
                for bad in (True, float(value)):
                    row = case(); row['hostReports'][-1][name] = bad
                    with self.subTest(field=name, bad=bad): self.reject(row)

    def test_actual_host_desktop_fields(self):
        for name, value in case()['hostReports'][0].items():
            row = case(); del row['hostReports'][0][name]
            with self.subTest(missing=name): self.reject(row)
            row = case()
            row['hostReports'][0][name] = False if type(value) is bool else 0 if name == 'threadId' else value + 1 if type(value) is int else value + '-bad'
            with self.subTest(changed=name): self.reject(row)
        row = case(); row['hostReports'][0]['unknown'] = False; self.reject(row)

    def test_launch_cannot_hide_failure_or_input_use(self):
        for name, bad in (('ok', False), ('exitCode', 2), ('exitCode', True), ('jobActiveProcesses', 1),
            ('jobAtomic', False), ('handleListApplied', False), ('processSettled', False), ('jobEmpty', False),
            ('timedOut', True), ('forcedTermination', True), ('desktopHandleInherited', True),
            ('cleanupErrors', ['failure']), ('desktop', 'Default'), ('inputAfter', 'Other'),
            ('pid', True), ('creation100ns', 0), ('initialThreadId', 0),
            ('actualExecutable', 'wrong.exe'), ('error', 'post-run exception')):
            row = case(); row['process'][name] = bad
            with self.subTest(field=name): self.reject(row)

    def test_medium_contract_on_probe_and_fixture(self):
        for role in ('probe', 'process'):
            for name, value in medium().items():
                rows = comparison(); token = rows[2][role]['restrictedToken']
                token[name] = not value if type(value) is bool else value + 1 if type(value) is int else ['failed'] if type(value) is list else 'bad'
                with self.subTest(role=role, field=name): self.reject_comparison(rows)
            rows = comparison(); del rows[2][role]['restrictedToken']; self.reject_comparison(rows)

    def test_comparison_requires_all_independent_cases_in_order(self):
        for at in range(3):
            rows = comparison(); del rows[at]; self.reject_comparison(rows)
        rows = comparison(); rows.append(case()); self.reject_comparison(rows)
        rows = comparison(); rows[0], rows[1] = rows[1], rows[0]; self.reject_comparison(rows)
        rows = comparison(); rows[1] = copy.deepcopy(rows[0]); rows[1]['label'] = 'new-reference'; self.reject_comparison(rows)
        rows = comparison(); rows[1]['ok'] = 1; self.reject_comparison(rows)
        rows = comparison(); rows[2]['childSettlement'] = 'still-running'; self.reject_comparison(rows)

    def test_final_comparison_revalidates_not_trusts_embedded_verdict(self):
        rows = comparison(); rows[0]['acceptance'] = dict(ok=True)
        rows[0]['reports'][4]['failures'] = 1; self.reject_comparison(rows)
        rows = comparison(); rows[0]['reports'][-1]['redSampledPixelFnv'] = '0' * 16; self.reject_comparison(rows)
        rows = comparison(); rows[0]['reports'][-1]['cacheReuseCertifiedByFixture'] = True; self.reject_comparison(rows)

    def test_desktop_closure_input_and_body_error(self):
        for name, bad in (('schema', True), ('ok', False), ('desktopCreated', False), ('desktopClosed', False),
            ('creatorExited', False), ('desktopHandleInheritable', True), ('desktopSwitchRequested', True),
            ('restrictedMediumRequested', False), ('globalInputUsed', True), ('gameTouched', True),
            ('cleanupErrors', ['failure']), ('error', 'failed'), ('bodyError', 'failed'),
            ('desktopPath', 'WinSta0\\Default')):
            rows = comparison(); rows[2]['desktopSafety'][name] = bad
            with self.subTest(field=name): self.reject_comparison(rows)
        for name in ('inputAfterCreate', 'inputBeforeLaunch', 'inputAfterLaunch', 'inputAfterRun', 'inputAfterClose'):
            rows = comparison(); rows[2]['desktopSafety'][name] = 'Changed'
            with self.subTest(field=name): self.reject_comparison(rows)
        rows = comparison(); rows[2]['desktopSafety']['launches'].reverse(); self.reject_comparison(rows)

    def test_final_cpu_probe_revalidated_from_full_records(self):
        for at in (0, 1):
            for field in ('pid', 'bits', 'creation100ns', 'gpuModulesAbsent', 'normalTeardown'):
                rows = comparison(); row = rows[2]['probeReports'][at]; value = row[field]
                row[field] = not value if type(value) is bool else value + 1
                with self.subTest(at=at, field=field): self.reject_comparison(rows)
        rows = comparison(); rows[2]['probeReports'] = []; self.reject_comparison(rows)
        rows = comparison(); rows[2]['probeReports'].append(copy.deepcopy(rows[2]['probeReports'][1])); self.reject_comparison(rows)
        rows = comparison(); rows[2]['probeReports'][0]['main']['threadId'] += 3; self.reject_comparison(rows)
        rows = comparison(); rows[2]['probeReports'][1]['pump']['inputAfter'] = 'Other'; self.reject_comparison(rows)
        rows = comparison(); rows[2]['probeReports'][0]['childExit'] = 1; self.reject_comparison(rows)

    def test_loader_invocation_is_exact_but_not_module_path_claim(self):
        for field, bad in (('absoluteDllArgument', 'relative.dll'), ('modulePathIndependentlyReported', True)):
            rows = comparison(); rows[2]['loaderInvocation'][field] = bad
            with self.subTest(field=field): self.reject_comparison(rows)
        rows = comparison(); rows[2]['loaderInvocation']['renderer']['path'] += '.other'; self.reject_comparison(rows)
        rows = comparison(); rows[2]['loaderInvocation']['renderer']['bytes'] = True; self.reject_comparison(rows)
        rows = comparison(); rows[2]['loaderInvocation']['renderer']['sha256'] = 'A'; self.reject_comparison(rows)
        rows = comparison(); rows[2]['process']['argv'].append('--reference'); self.reject_comparison(rows)
        rows = comparison(); rows[0]['process']['argv'].pop(); self.reject_comparison(rows)
        rows = comparison(); rows[2]['process']['argv'][2] += '.other'; self.reject_comparison(rows)

    def test_unique_frozen_identity_types_and_paths(self):
        original = [file_identity(gate.ROOT/'source.cpp')]
        gate.unique_identities(original, 'test')
        for name, bad in (('path', 'relative.cpp'), ('path', ''), ('bytes', 0), ('bytes', True),
            ('sha256', 'a' * 64), ('sha256', 'A' * 63)):
            rows = copy.deepcopy(original); rows[0][name] = bad
            with self.subTest(field=name):
                with self.assertRaises(ERRORS): gate.unique_identities(rows, 'test')
        with self.assertRaises(ERRORS): gate.unique_identities(original * 2, 'test')
        with self.assertRaises(ERRORS): gate.unique_identities([], 'test')

    def test_strict_json_rejects_duplicate_keys_nonfinite_and_partial(self):
        for text in ('{"ok":true,"ok":false}', '{"x":{"bits":32,"bits":64}}',
            '{"count":NaN}', '{"count":Infinity}', '{"schema":1'):
            with self.subTest(text=text), self.assertRaises(ValueError): gate.strict_json(text)

    def test_raw_json_lines_do_not_turn_cache_diagnostics_into_pixel_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp)/'case.stdout'
            rows = case()['reports']
            raw.write_text('shader-cache-diagnostic {not a frozen pixel schema}\n' +
                '\n'.join(json.dumps(row) for row in rows), encoding='utf-8')
            self.assertEqual(gate.json_lines(raw), rows)
            raw.write_text('{"schema":"x","schema":"y"}\n', encoding='utf-8')
            with self.assertRaises(ValueError): gate.json_lines(raw)

    def test_raw_case_preservation_reopens_original_bytes_and_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp).resolve(); row = case()
            row['artifactDirectory'] = str(directory)
            row['loaderInvocation']['absoluteDllArgument'] = str(directory/'renderer.dll')
            data = {'case.stdout': row['reports'], 'host.log': row['hostReports'],
                'probe.stdout': row['probeReports'][:1], 'probe-child.log': row['probeReports'][1:]}
            for name, records in data.items():
                (directory/name).write_text('\n'.join(json.dumps(record) for record in records), encoding='utf-8')
            (directory/'case.stderr').write_bytes(b'')
            (directory/'probe.stderr').write_bytes(b'')
            row['probeRawArtifacts'] = gate.capture_raw_artifacts(directory,
                ['probe.stdout', 'probe.stderr', 'probe-child.log'])
            row['initialRawArtifacts'] = gate.capture_raw_artifacts(directory,
                ['case.stdout', 'case.stderr', 'host.log'], True)
            row['artifacts'] = [gate.identity(path) for path in sorted(directory.iterdir())]
            gate.verify_case_artifacts(row)
            duplicate = copy.deepcopy(row); duplicate['artifacts'].append(copy.deepcopy(duplicate['artifacts'][0]))
            with self.assertRaises(ValueError): gate.verify_case_artifacts(duplicate)
            wrong = copy.deepcopy(row); wrong['reports'][0]['ordinal'] += 1
            with self.assertRaises(ValueError): gate.verify_case_artifacts(wrong)
            for name in ('case.stderr', 'probe.stderr'):
                missing = copy.deepcopy(row)
                missing['artifacts'] = [item for item in missing['artifacts'] if Path(item['path']).name != name]
                (directory/name).write_bytes(b'ERROR: missing identity hides non-JSON rewrite\n')
                with self.subTest(missing=name), self.assertRaises(ValueError): gate.verify_case_artifacts(missing)
                (directory/name).write_bytes(b'')
            for key in ('probeRawArtifacts', 'initialRawArtifacts'):
                missing = copy.deepcopy(row); del missing[key]
                with self.subTest(missing=key), self.assertRaises(ValueError): gate.verify_case_artifacts(missing)
            missing = copy.deepcopy(row); missing['artifacts'] = []
            with self.assertRaises(ValueError): gate.verify_case_artifacts(missing)
            (directory/'case.stderr').write_bytes(b'late modification')
            with self.assertRaises(ValueError): gate.verify_case_artifacts(row)
            # Re-hashing the later files cannot retroactively replace the
            # producer-time initial raw identity.
            changed = copy.deepcopy(row); changed['artifacts'] = [gate.identity(path) for path in sorted(directory.iterdir())]
            with self.assertRaisesRegex(ValueError, 'changed since launch return'): gate.verify_case_artifacts(changed)
            (directory/'case.stderr').write_bytes(b'')
            (directory/'late.log').write_bytes(b'late log\n')
            changed = copy.deepcopy(row); changed['artifacts'] = [gate.identity(path) for path in sorted(directory.iterdir())]
            with self.assertRaisesRegex(ValueError, 'population changed'): gate.verify_case_artifacts(changed)


class BaselineInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve(); self.receipt_path = self.base/'receipt.json'
        run = case()
        (self.base/'probe.stdout').write_text(json.dumps(run['probeReports'][0]), encoding='utf-8')
        (self.base/'desktop-child.log').write_text(json.dumps(run['probeReports'][1]), encoding='utf-8')
        (self.base/'probe.stderr').write_bytes(b'')
        probe_raw = gate.capture_raw_artifacts(self.base, ['probe.stdout', 'probe.stderr', 'desktop-child.log'])
        step_raw = []
        for bits in (32, 64):
            for name in ('resource_registry', 'wire'):
                names = [f'{name}-{bits}.stdout', f'{name}-{bits}.stderr']
                for filename in names: (self.base/filename).write_bytes(b'CPU fixture\n')
                step_raw.append(gate.capture_raw_artifacts(self.base, names))
        directory = self.base/'memory-account'; directory.mkdir(); run['label'] = 'memory-account'
        run['process'].update(stdout=str(directory/'case.stdout'), stderr=str(directory/'case.stderr'))
        for name, rows in (('case.stdout', run['reports']), ('host.log', run['hostReports'])):
            (directory/name).write_text('\n'.join(json.dumps(row) for row in rows), encoding='utf-8')
        (directory/'case.stderr').write_bytes(b'')
        run['initialRawArtifacts'] = gate.capture_raw_artifacts(directory, ['case.stdout', 'case.stderr', 'host.log'], True)
        binaries = []
        for name in ('warvk-d3d9-proxy-dev.dll', 'warvk-d3d9-host-dev.exe', 'desktop-probe-32.exe', 'desktop-probe-64.exe'):
            path = self.base/name; path.write_bytes(b'1'); binaries.append(file_identity(path))
        self.baseline = dict(schema=1, scope='M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT', ok=True,
            sourcesPreserved=True, sourceAndReferencePreserved=True, playerDllChanged=False,
            restrictedMediumRequested=True, cases=[run], desktopName=run['desktopName'],
            desktopSafety=run['desktopSafety'], desktopProbe=run['probe'], probeReports=run['probeReports'],
            sources=[file_identity(self.base/'source.cpp')], binaries=binaries,
            compilers=[file_identity(self.base/'compiler.exe')],
            rawArtifactsPreserved=True, probeRawArtifacts=probe_raw, stepRawArtifacts=step_raw,
            processControls=dict(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0'))
        self.reports = copy.deepcopy(run['reports']); self.hosts = copy.deepcopy(run['hostReports'])

    def read(self):
        # Mock ONLY the earlier full M0 raw/source verifier. Its direct call is
        # asserted below; this suite independently exercises the new wrapper.
        with patch.object(gate.baseline_reader, 'verify_baseline',
            return_value=(self.baseline, self.reports, self.hosts)) as verify:
            result = gate.baseline_inputs(self.receipt_path)
            verify.assert_called_once_with(self.receipt_path)
            return result

    def reject(self):
        with self.assertRaises(ERRORS): self.read()

    def test_requires_full_baseline_verifier_and_reuses_only_frozen_names(self):
        _, selected = self.read()
        self.assertEqual(set(selected), {'proxy', 'host', 'probe32', 'probe64'})
        self.assertEqual(selected['host'], (self.base/'warvk-d3d9-host-dev.exe').resolve())
        with patch.object(gate.baseline_reader, 'verify_baseline', side_effect=ValueError('source hash changed')):
            with self.assertRaisesRegex(ValueError, 'source hash changed'): gate.baseline_inputs(self.receipt_path)

    def test_missing_false_or_mistyped_completion_flags(self):
        saved = copy.deepcopy(self.baseline)
        for name, bad in (('schema', True), ('scope', 'other'), ('ok', False), ('sourcesPreserved', False),
            ('sourceAndReferencePreserved', False), ('playerDllChanged', True), ('restrictedMediumRequested', False)):
            self.baseline = copy.deepcopy(saved); self.baseline[name] = bad
            with self.subTest(field=name): self.reject()
        for name in ('schema', 'sources', 'binaries', 'compilers', 'desktopProbe', 'probeReports'):
            self.baseline = copy.deepcopy(saved); del self.baseline[name]
            with self.subTest(missing=name): self.reject()

    def test_duplicate_frozen_sources_binaries_and_compilers(self):
        saved = copy.deepcopy(self.baseline)
        for name in ('sources', 'binaries', 'compilers'):
            self.baseline = copy.deepcopy(saved); self.baseline[name].append(copy.deepcopy(self.baseline[name][0]))
            with self.subTest(group=name): self.reject()
        self.baseline = copy.deepcopy(saved); self.baseline['binaries'].pop(); self.reject()

    def test_embedded_raw_parent_or_host_disagreement(self):
        self.baseline['cases'][0]['reports'][0]['ordinal'] += 1; self.reject()
        self.baseline['cases'][0]['reports'] = copy.deepcopy(self.reports)
        self.baseline['cases'][0]['hostReports'][0]['pid'] += 1; self.reject()

    def test_cpu_probe_raw_disagreement_duplicate_and_owner(self):
        raw = self.base/'probe.stdout'
        original = raw.read_text('utf-8')
        raw.write_text(original + '\n' + original, encoding='utf-8'); self.reject()
        raw.write_text(original, encoding='utf-8')
        self.baseline['probeReports'][0]['pid'] += 1; self.reject()

    def test_nonjson_probe_rewrite_and_historical_missing_manifest_rejected(self):
        for name in ('probe.stdout', 'probe.stderr', 'desktop-child.log'):
            path = self.base/name; original = path.read_bytes()
            path.write_bytes(original+b'\nERROR: late non-JSON diagnostic\n')
            with self.subTest(name=name): self.reject()
            path.write_bytes(original)
        for key in ('rawArtifactsPreserved', 'probeRawArtifacts', 'stepRawArtifacts'):
            original = self.baseline.pop(key)
            with self.subTest(missing=key): self.reject()
            self.baseline[key] = original

    def test_desktop_close_and_medium_before_any_native_launch(self):
        saved = copy.deepcopy(self.baseline)
        for name, bad in (('ok', False), ('desktopClosed', False), ('creatorExited', False),
            ('cleanupErrors', ['failed']), ('inputAfterClose', 'Other'), ('bodyError', 'failure')):
            self.baseline = copy.deepcopy(saved); self.baseline['desktopSafety'][name] = bad
            with self.subTest(field=name): self.reject()
        self.baseline = copy.deepcopy(saved)
        self.baseline['desktopProbe']['restrictedToken']['ready'] = False; self.reject()

    def test_no_new_driver_controls_or_hidden_shader_toggles(self):
        self.baseline['processControls']['DXVK_SHADER_CACHE'] = '1'; self.reject()
        self.baseline['processControls']['DXVK_SHADER_CACHE'] = '0'
        self.baseline['processControls']['NEW_DRIVER_OVERRIDE'] = '1'; self.reject()


if __name__ == '__main__':
    unittest.main()
