"""Pure instrumentation/reader/recipe tests; no compiler, GPU or Desktop.

Run this adjacent draft directly with python -B. The pair can later be moved
to AutoTest unchanged; imports of production helpers use the actual repo root.
"""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import diagnose_render_host_initialization as gate


def trace(events=None, pid=42, tid=43, creation=12345):
    events = gate.expected_events() if events is None else events
    return '\n'.join(f'{gate.PREFIX} pid={pid} tid={tid} creation100ns={creation} index={i} phase={phase} event={event} '
        f'elapsedNs={i*1000} priorLogNs={(i-1)*10}' for i, (phase, event) in enumerate(events, 1))


def old_receipt():
    directory = gate.ROOT/'AutoTest/artifacts/fake-r10/clear-soak'
    process = dict(argv=[str(directory/'d3d9-presenter-fault32.exe'), '--dll', str(directory/'proxy.dll'),
        '--scenario', 'clear-soak', '--diagnostic-readback', '1', '--expect-desktop', 'PrivateOld',
        '--input-desktop', 'Default'], desktop='PrivateOld', inputBefore='Default', timeoutSeconds=45.0,
        restrictedToken=dict(mode='restricted-medium-per-child/v1', childTokenVerifiedBeforeResume=True))
    return dict(scope='D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP', ok=False, restrictedMediumRequested=True,
        cases=[dict(label='clear-soak', process=process)])


def settled():
    return dict(exitCode=2, jobActiveProcesses=0, processSettled=True, jobEmpty=True, jobAtomic=True,
        handleListApplied=True, timedOut=False, forcedTermination=False, desktopHandleInherited=False,
        cleanupErrors=[], desktop='PrivateNew', inputBefore='Default', inputAfter='Default',
        pid=41, initialThreadId=42, creation100ns=1234, argv=['fixture.exe'],
        actualExecutable='fixture.exe', timeoutSeconds=45,
        restrictedToken=dict(mode='restricted-medium-per-child/v1', childTokenVerifiedBeforeResume=True,
            derivedTokenClosed=True, cleanupErrors=[], resumedAfterVerification=True, resumePreviousCount=1,
            parentTokenUnchanged=True, noHighIntegrityFallback=True, globalSecurityChanged=False,
            childBeforeResume=dict(integrityRid=8192, integritySid='S-1-16-8192', hasRestrictions=True)))


class InstrumentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = gate.BACKEND.read_text('utf-8')
        cls.changed, cls.edits = gate.instrument_source(cls.original)

    def test_imports_use_real_autotest_root(self):
        self.assertEqual(gate.copied.ROOT.resolve(), gate.ROOT.resolve())
        self.assertEqual(gate.BACKEND, gate.ROOT/'tools/render_host/dxvk_core/d3d9_backend.cpp')
        self.assertEqual(Path(gate.copied.__file__).parent.resolve(), (gate.ROOT/'AutoTest').resolve())

    def test_insertions_reconstruct_original_byte_for_byte(self):
        reverted = self.changed
        for row in reversed(self.edits):
            self.assertEqual(reverted.count(row['insertion']), 1)
            reverted = reverted.replace(row['insertion'], '', 1)
        self.assertEqual(reverted, self.original)

    def test_production_timeouts_and_submission_statements_preserved(self):
        for anchor in ('constexpr auto CpuWait = std::chrono::seconds(5);',
            "constexpr uint64_t GpuWaitNs = 5'000'000'000ull;", 'owner.submit(m_stats, true);',
            'device->submitCommandList(commands, nullptr, serial, &submission);',
            'device->vkd()->vkWaitSemaphores(device->handle(), &wait, GpuWaitNs);'):
            self.assertEqual(self.original.count(anchor), self.changed.count(anchor))
        self.assertNotIn('std::this_thread::sleep', gate.TRACE_CPP)
        self.assertNotIn('malloc', gate.TRACE_CPP)
        self.assertNotIn('new ', gate.TRACE_CPP)

    def test_fixed_scope_and_pre_submit_retirement_not_gpu_completion(self):
        self.assertIn('InitDiagnosticTrace initTiming;', self.changed)
        self.assertEqual(self.changed.count('InitDiagnosticTrace initTiming;'), 1)
        init = self.changed[self.changed.index('void D3D9Backend::initialize('):
                            self.changed.index('void D3D9Backend::createBuffer(')]
        for phase in gate.OUTER_PHASES:
            self.assertIn(f'("{phase}")', init)
        self.assertIn('? "resource-retire" : "resource-retire-pre-submit"', self.changed)
        self.assertIn('initDiagnosticTrace = previous;', gate.TRACE_CPP)
        self.assertIn('if (trace) trace->inSubmit = previous;', gate.TRACE_CPP)

    def test_exception_markers_and_measured_logging_cost(self):
        self.assertIn('std::uncaught_exceptions()', gate.TRACE_CPP)
        self.assertIn('? "exception" : "end"', gate.TRACE_CPP)
        self.assertIn('std::fflush(stderr);', gate.TRACE_CPP)
        self.assertIn('priorLogNs +=', gate.TRACE_CPP)
        self.assertNotIn('sequence=', gate.TRACE_CPP)  # Not available in backend.

    def test_instrumented_input_refused(self):
        with self.assertRaises(ValueError):
            gate.instrument_source(self.changed)

    def test_changed_or_duplicate_anchor_refused(self):
        anchor = '    owner.instance = new DxvkInstance(DxvkInstanceFlag::ClientApiIsD3D9);\n'
        for changed in (self.original.replace(anchor, ''), self.original.replace(anchor, anchor+anchor)):
            with self.subTest(delta=len(changed)-len(self.original)), self.assertRaises(ValueError):
                gate.instrument_source(changed)

    def test_crlf_must_be_explicitly_normalized(self):
        with self.assertRaises(ValueError):
            gate.instrument_source(self.original.replace('\n', '\r\n'))


class ReaderTests(unittest.TestCase):
    def test_complete_trace_is_diagnostic_not_acceptance(self):
        row = gate.read_stages('existing host diagnostic\n'+trace()+'\nother log', 42, 43)
        self.assertTrue(row['traceComplete'])
        self.assertTrue(row['initializationSucceeded'])
        self.assertFalse(row['acceptancePassed'])
        self.assertEqual(row['markerCount'], 28)
        self.assertEqual({x['phase'] for x in row['intervals']},
            {'initialize', 'resource-retire-pre-submit', *gate.OUTER_PHASES, *gate.SUBMIT_PHASES})
        self.assertTrue(row['finalMarkerLoggingNotMeasured'])
        self.assertTrue(row['timerCallOverheadNotSeparated'])

    def test_all_possible_exception_unwinds(self):
        events = gate.expected_events(); stack = []
        for index, (phase, event) in enumerate(events[:-1], 1):
            if event == 'begin': stack.append(phase)
            else: stack.pop()
            failed = events[:index]+[(name, 'exception') for name in reversed(stack)]
            with self.subTest(index=index):
                row = gate.read_stages(trace(failed))
                self.assertFalse(row['initializationSucceeded'])
                self.assertFalse(row['acceptancePassed'])

    def test_empty_and_partial_cannot_be_complete(self):
        for payload in ('', 'nothing\n', trace().splitlines()[0], '\n'.join(trace().splitlines()[:-1])):
            with self.subTest(size=len(payload)), self.assertRaises(ValueError):
                gate.read_stages(payload)

    def test_missing_duplicate_and_reordered_stages(self):
        events = gate.expected_events()
        populations = (events[:3]+events[5:], events[:3]+events[1:3]+events[3:],
                       events[:1]+events[3:5]+events[1:3]+events[5:])
        for records in populations:
            with self.subTest(records=records[:5]), self.assertRaises(ValueError):
                gate.read_stages(trace(records))

    def test_native_identity_mismatch_and_bool_refused(self):
        for pid, tid in ((41, 43), (42, 44), (True, 43), (42, False), (0, 43), (2**32, 43)):
            with self.subTest(pid=pid, tid=tid), self.assertRaises(ValueError):
                gate.read_stages(trace(), pid, tid)
        with self.assertRaises(ValueError):
            gate.read_stages(trace().replace('pid=42 tid=43 creation100ns=12345 index=3',
                                            'pid=999 tid=43 creation100ns=12345 index=3'))

    def test_creation_identity_not_just_pid(self):
        gate.read_stages(trace(), 42, 43, 12345)
        for value in (12346, True, 0, 2**64):
            with self.assertRaises(ValueError): gate.read_stages(trace(), 42, 43, value)
        with self.assertRaises(ValueError):
            gate.read_stages(trace().replace('creation100ns=12345 index=3', 'creation100ns=99999 index=3'))

    def test_actual_host_proxy_creation_bound(self):
        witness = dict(schema='warvk-d3d9-desktop/v1', beforeGpu=True, pid=42, threadId=43, peerPid=41,
            desktop='PrivateNew', inputDesktop='Default', windowStation='WinSta0', windowThreadId=44)
        proxy = dict(schema='warvk-d3d9-proxy/v1', childPid=42, childCreation=12345,
                     peerVerified=True, x86VulkanAbsent=True)
        gate.read_bound_stages(trace(), [witness], [proxy], settled(), 'PrivateNew', 'Default')
        for key, value in (('childCreation', 9999), ('childPid', 999), ('childCreation', True), ('peerVerified', False)):
            bad = proxy | {key: value}
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.read_bound_stages(trace(), [witness], [bad], settled(), 'PrivateNew', 'Default')
        with self.assertRaises(ValueError):
            gate.read_bound_stages(trace(), [witness], [proxy, proxy], settled(), 'PrivateNew', 'Default')

    def test_duplicate_missing_sequence_refused(self):
        for replacement in ('index=1', 'index=3', 'index=0'):
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                gate.read_stages(trace().replace('index=2 ', replacement+' '))

    def test_clock_inversions_and_impossible_overhead(self):
        for text in (trace().replace('elapsedNs=2000', 'elapsedNs=0'),
                     trace().replace('priorLogNs=20', 'priorLogNs=0'),
                     trace().replace('priorLogNs=10', 'priorLogNs=3000'),
                     trace().replace('elapsedNs=2000 priorLogNs=10', 'elapsedNs=2000 priorLogNs=1500')):
            with self.subTest(text=text[:200]), self.assertRaises(ValueError):
                gate.read_stages(text)

    def test_malformed_marker_never_silently_skipped(self):
        for malformed in (' '+trace(), trace().replace('event=begin', 'event=success', 1),
                          trace().replace('phase=instance', 'phase=unknown'),
                          trace()+'\n'+gate.PREFIX+' damaged', trace().replace('index=2', 'index=02', 1)):
            with self.subTest(prefix=malformed[:80]), self.assertRaises(ValueError):
                gate.read_stages(malformed)

    def test_exception_must_unwind_stack_without_resuming(self):
        for events in ([('initialize', 'begin'), ('device', 'exception'), ('initialize', 'exception')],
                       [('initialize', 'begin'), ('instance', 'begin'), ('instance', 'exception'),
                        ('adapter-check', 'begin'), ('initialize', 'exception')]):
            with self.assertRaises(ValueError): gate.read_stages(trace(events))

    def test_empty_success_and_root_early_end_refused(self):
        with self.assertRaises(ValueError):
            gate.read_stages(trace([('initialize', 'begin'), ('initialize', 'end')]))

    def test_excessive_trace_and_after_init_events_refused(self):
        with self.assertRaises(ValueError):
            gate.read_stages(trace()+ '\n'+trace())


class ReadbackProfileTests(unittest.TestCase):
    def test_profiles_only_remove_the_two_readback_markers(self):
        full = gate.expected_events()
        short = gate.expected_events(readback=False)
        self.assertEqual(full, gate.expected_events(readback=True))
        self.assertEqual(short, [event for event in full if event[0] != 'presented-readback'])
        self.assertEqual(len(full), 28)
        self.assertEqual(len(short), 26)
        for enabled, events in ((True, full), (False, short)):
            result = gate.read_stages(trace(events), readback=enabled)
            self.assertTrue(result['traceComplete'])
            self.assertTrue(result['initializationSucceeded'])
            self.assertFalse(result['acceptancePassed'])
            self.assertEqual(result['markerCount'], len(events))

    def test_readback_is_keyword_only_and_strict_bool(self):
        for value in (None, 0, 1, 'false', [], {}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.expected_events(readback=value)
            with self.subTest(reader=value), self.assertRaises(ValueError):
                gate.read_stages(trace(), readback=value)
        with self.assertRaises(TypeError): gate.expected_events(False)
        with self.assertRaises(TypeError): gate.read_stages(trace(), None, None, None, False)

    def test_profiles_cannot_be_inferred_from_missing_events(self):
        short = trace(gate.expected_events(readback=False))
        with self.assertRaises(ValueError): gate.read_stages(short)
        with self.assertRaises(ValueError): gate.read_stages(trace(), readback=False)

    def test_false_profile_retains_order_population_and_clock_guards(self):
        events = gate.expected_events(readback=False)
        for index in range(1, len(events)-1):
            bad = events[:index]+events[index+1:]
            with self.subTest(removed=index), self.assertRaises(ValueError):
                gate.read_stages(trace(bad), readback=False)
        bad = list(events); bad[2], bad[3] = bad[3], bad[2]
        with self.assertRaises(ValueError): gate.read_stages(trace(bad), readback=False)
        raw = trace(events)
        for bad in (raw.replace('index=2 ', 'index=1 '),
                    raw.replace('elapsedNs=2000', 'elapsedNs=0'),
                    raw.replace('priorLogNs=10', 'priorLogNs=3000')):
            with self.assertRaises(ValueError): gate.read_stages(bad, readback=False)

    def test_false_profile_keeps_exact_host_proxy_binding(self):
        witness = dict(schema='warvk-d3d9-desktop/v1', beforeGpu=True, pid=42, threadId=43, peerPid=41,
            desktop='PrivateNew', inputDesktop='Default', windowStation='WinSta0', windowThreadId=44)
        proxy = dict(schema='warvk-d3d9-proxy/v1', childPid=42, childCreation=12345,
                     peerVerified=True, x86VulkanAbsent=True)
        raw = trace(gate.expected_events(readback=False))
        gate.read_bound_stages(raw, [witness], [proxy], settled(), 'PrivateNew', 'Default', readback=False)
        for key, value in (('childCreation', 999), ('childPid', 999), ('peerVerified', False)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.read_bound_stages(raw, [witness], [proxy | {key: value}], settled(),
                                      'PrivateNew', 'Default', readback=False)

    def test_false_profile_keeps_exception_unwind_semantics(self):
        events = [('initialize', 'begin'), ('instance', 'begin'),
                  ('instance', 'exception'), ('initialize', 'exception')]
        result = gate.read_stages(trace(events), readback=False)
        self.assertFalse(result['initializationSucceeded'])
        self.assertFalse(result['acceptancePassed'])
        bad = events[:-1]+[('adapter-check', 'begin'), events[-1]]
        with self.assertRaises(ValueError): gate.read_stages(trace(bad), readback=False)


class RecipeTests(unittest.TestCase):
    def test_historical_actual_argv_and_deadline(self):
        record = old_receipt()
        case = gate.baseline_case(record)
        self.assertEqual(case['process']['timeoutSeconds'], 45)

    def test_mutated_historical_case_not_accepted(self):
        mutations = (
            lambda r: r.update(ok=True),
            lambda r: r.update(restrictedMediumRequested=False),
            lambda r: r['cases'].append(copy.deepcopy(r['cases'][0])),
            lambda r: r['cases'][0]['process'].update(timeoutSeconds=90),
            lambda r: r['cases'][0]['process']['argv'].__setitem__(6, '0'),
            lambda r: r['cases'][0]['process']['argv'].__setitem__(4, 'present'),
            lambda r: r['cases'][0]['process']['restrictedToken'].update(childTokenVerifiedBeforeResume=False),
            lambda r: r['cases'][0]['process']['argv'].append('--extra'),
        )
        for mutate in mutations:
            r = old_receipt(); mutate(r)
            with self.assertRaises(ValueError): gate.baseline_case(r)

    def test_only_expected_path_and_desktop_substitutions(self):
        original = old_receipt()['cases'][0]['process']['argv']
        changed = gate.replay_argv(original, Path('new-fixture.exe'), Path('new-proxy.dll'), 'PrivateNew', 'Default')
        self.assertEqual(changed[3:8], original[3:8])
        self.assertEqual(changed[0], 'new-fixture.exe')
        self.assertEqual(changed[8], 'PrivateNew')
        self.assertNotEqual(original[0], changed[0])

    def test_allowlist_does_not_inherit_credentials_or_layer_overrides(self):
        env = gate.recipe_environment(dict(PATH='toolchain', SystemRoot='C:/Windows',
            DSH_TOKEN='secret', OPENAI_API_KEY='secret', VK_LOADER_LAYERS_DISABLE='*',
            VK_LOADER_DRIVERS_SELECT='driver', DXVK_SHADER_CACHE='1'), Path('new-case'), Path('diagnostic.exe'))
        self.assertEqual(env['PATH'], 'toolchain')
        self.assertEqual(env['DXVK_SHADER_CACHE'], '0')
        self.assertEqual(env['DXVK_LOG_LEVEL'], 'info')
        self.assertEqual(env['WARVK_D3D9_PROFILE'], 'presenter-readback-isolated-v1')
        self.assertFalse(any(k.startswith('VK_') for k in env))
        self.assertNotIn('secret', env.values())
        self.assertNotIn('OPENAI_API_KEY', env)

    def test_case_duplicate_environment_refused(self):
        with self.assertRaises(ValueError):
            gate.recipe_environment({'PATH': 'one', 'Path': 'two'}, Path('case'), Path('host'))

    def test_copied_tu_compile_tokens_use_original_helper(self):
        cwd = gate.ROOT/'fake-build'
        source = gate.BACKEND
        entry = dict(directory=str(cwd), file=str(source), output='original.obj', arguments=[
            'clang++.exe', '-I../include', '-O2', '-MD', '-MF', 'original.obj.d', '-MQ', 'original.obj',
            '-o', 'original.obj', '-c', str(source)])
        row = gate.copied.compile_plan(entry, cwd/'copied.cpp', cwd/'new.obj', cwd/'new.obj.d')
        self.assertEqual(row['cwd'], str(cwd.resolve()))
        self.assertEqual(row['argv'][1:4], row['originalArgv'][1:4])
        self.assertEqual(row['addedInclude'], ['-iquote', str(source.parent)])

    def test_false_result_keeps_valid_diagnostic_settlement(self):
        gate.check_diagnostic_settlement(settled(), 'PrivateNew', 'Default')
        row = settled(); row['exitCode'] = 0
        gate.check_diagnostic_settlement(row, 'PrivateNew', 'Default')

    def test_uncertain_settlement_refused(self):
        mutations = dict(forcedTermination=True, timedOut=True, jobActiveProcesses=1,
            jobEmpty=False, processSettled=False, handleListApplied=False, desktopHandleInherited=True,
            cleanupErrors=['error'], inputAfter='Other', exitCode=4, pid=True,
            creation100ns=0, actualExecutable='different.exe', timeoutSeconds=90)
        for key, value in mutations.items():
            row = settled(); row[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.check_diagnostic_settlement(row, 'PrivateNew', 'Default')

    def test_actual_medium_resume_fields_required(self):
        for key, value in (('resumedAfterVerification', False), ('resumePreviousCount', 2),
            ('resumePreviousCount', True), ('parentTokenUnchanged', False), ('noHighIntegrityFallback', False),
            ('globalSecurityChanged', True)):
            row = settled(); row['restrictedToken'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.check_diagnostic_settlement(row, 'PrivateNew', 'Default')
        row = settled(); row['restrictedToken']['childBeforeResume']['integrityRid'] = 12288
        with self.assertRaises(ValueError): gate.check_diagnostic_settlement(row, 'PrivateNew', 'Default')

    def test_step_failure_still_captures_raw_before_propagating(self):
        receipt = dict(steps=[], stepRawArtifacts=[])
        prefix = gate.ROOT/'AutoTest/artifacts/not-created-for-pure-test/compile'
        with patch.object(gate, 'execute', side_effect=ValueError('failed compile')), \
             patch.object(gate, 'capture_raw_artifacts', return_value={'raw': 'kept'}) as capture:
            with self.assertRaisesRegex(ValueError, 'failed compile'):
                gate.run_step(['compiler'], gate.ROOT, prefix, 30, {}, receipt)
        self.assertEqual(receipt['stepRawArtifacts'], [{'raw': 'kept'}])
        capture.assert_called_once_with(prefix.parent, ['compile.stdout', 'compile.stderr'])

    def test_source_never_runs_mutation_main_or_changes_acceptance(self):
        text = Path(gate.__file__).read_text('utf-8')
        self.assertNotIn('copied.main(', text)
        self.assertNotIn('copied.mutate_source(', text)
        self.assertNotIn('acceptancePassed=True', text)
        self.assertNotIn("['acceptancePassed'] = True", text)
        self.assertIn("historicalEnvironmentUnrecorded=True", text)
        self.assertIn("historicalEnvironmentExactlyReproduced=False", text)
        self.assertIn("timeout_s=case['process']['timeoutSeconds']", text)


if __name__ == '__main__':
    unittest.main()
