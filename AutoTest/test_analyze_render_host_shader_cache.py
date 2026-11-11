"""Synthetic reader regression only: no native process, build, device or GPU.

Use the existing M0 reader's explicit synthetic wire fixture, then independently
construct cache observations. Temporary manifests/logs are fabricated test
inputs, never actual memory measurements or native acceptance artifacts.
"""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import analyze_render_host_shader_cache as reader
from test_render_host_memory_gate import records, DESKTOP, INPUT


def sample():
    reports, hosts, launch = records()
    windows = reader.check_case(reports, hosts, launch, DESKTOP, INPUT)['sampleWindows']
    rows = []
    for index, window in enumerate(windows):
        host = window['host']; phase = host['phase']; ordinal = host['ordinal']
        populated = index != 0; live = phase in (2, 3, 5)
        rows.append(dict(schema='warvk-d3d9-shader-cache/v1',
            pid=202, thread=404, creation=20002, nonceLow=0x1122334455667788,
            nonceHigh=0x8877665544332211, sequence=host['sequence'], phase=phase, ordinal=ordinal,
            requests=2*ordinal if populated else 0, hits=max(0, 2*ordinal-2) if populated else 0,
            compileAttempts=2 if populated else 0, compilesSucceeded=2 if populated else 0,
            registrations=2 if populated else 0, raceLosers=0, validationFailures=0,
            registrationFailures=0, readyEntries=2 if populated else 0, failedEntries=0,
            keyBytes=320 if populated else 0, keyCapacityBytes=384 if populated else 0,
            constantBytes=20 if populated else 0, constantCapacityBytes=40 if populated else 0,
            vertexCookies=[0, 0x100000001 if live else 0, 0, 0],
            pixelCookies=[0, 0, 0x100000002 if live else 0, 0],
            includesPipelineOrDriverBytes=False))
    return rows, windows, 320


class CacheReaderTests(unittest.TestCase):
    def reject(self, state):
        with self.assertRaises((ValueError, KeyError, TypeError, IndexError)):
            reader.check_cache(*state)

    def test_valid_repeated_workload_and_scope(self):
        result = reader.check_cache(*sample())
        self.assertTrue(result['ok'])
        self.assertEqual((result['createRequests'], result['cacheHits']), (192, 190))
        self.assertEqual((result['compilesSucceeded'], result['registrations']), (2, 2))
        self.assertEqual(result['canonicalCookies'], [0x100000001, 0x100000002])
        self.assertFalse(result['wholeProcessPlateauProven'])
        self.assertFalse(result['uniqueShaderWorkingSetBoundProven'])
        self.assertFalse(result['gameMemorySavingsProven'])
        self.assertFalse(result['includesPipelineOrDriverBytes'])

    def test_stable_larger_capacity_is_legal_not_exact_prediction(self):
        for field, unit in (('keyCapacityBytes', 4), ('constantCapacityBytes', 20)):
            state = sample()
            for row in state[0][1:]: row[field] += unit * 17
            with self.subTest(field=field): self.assertTrue(reader.check_cache(*state)['ok'])

    def test_capacity_equal_size_is_legal(self):
        state = sample()
        for row in state[0]:
            row['keyCapacityBytes'] = row['keyBytes']
            row['constantCapacityBytes'] = row['constantBytes']
        self.assertTrue(reader.check_cache(*state)['ok'])

    def test_single_checkpoint_capacity_growth_or_shrink_is_rejected(self):
        for field, unit in (('keyCapacityBytes', 4), ('constantCapacityBytes', 20)):
            for index in range(1, 8):
                for delta in (-unit, unit):
                    state = sample(); state[0][index][field] += delta
                    with self.subTest(field=field, index=index, delta=delta): self.reject(state)

    def test_uniform_non_element_capacity_is_not_production_reachable(self):
        for field in ('keyCapacityBytes', 'constantCapacityBytes'):
            state = sample()
            for row in state[0][1:]: row[field] += 1
            with self.subTest(field=field): self.reject(state)

    def test_capacity_below_size(self):
        for field, bad in (('keyCapacityBytes', 316), ('constantCapacityBytes', 0)):
            state = sample()
            for row in state[0][1:]: row[field] = bad
            with self.subTest(field=field): self.reject(state)

    def test_empty_short_extra_duplicate_reversed_rows(self):
        for changed in ([], sample()[0][:-1], sample()[0]+sample()[0][-1:],
                        [sample()[0][0]]*8, list(reversed(sample()[0]))):
            self.reject((changed, sample()[1], 320))

    def test_standalone_rejects_paired_wrong_phase_ordinal_sequence(self):
        for field in ('phase', 'ordinal', 'sequence'):
            state = sample()
            delta = 2 if field == 'phase' else 1
            state[0][4][field] += delta
            state[1][4]['host'][field] += delta
            state[1][4][field] += delta
            if field == 'ordinal':
                state[0][4]['requests'] += 2; state[0][4]['hits'] += 2
            with self.subTest(field=field): self.reject(state)

    def test_standalone_rejects_reordered_pairs(self):
        state = sample()
        state[0][3], state[0][4] = state[0][4], state[0][3]
        state[1][3], state[1][4] = state[1][4], state[1][3]
        self.reject(state)

    def test_each_identity_field_is_bound(self):
        for field in ('pid','thread','creation','nonceLow','nonceHigh','sequence','phase','ordinal'):
            for index in range(8):
                state = sample(); state[0][index][field] += 1
                with self.subTest(field=field, index=index): self.reject(state)

    def test_paired_cross_session_identity_drift_rejected(self):
        for field in ('pid','thread','creation','nonceLow','nonceHigh'):
            state = sample()
            state[0][4][field] += 1; state[1][4]['host'][field] += 1
            with self.subTest(field=field): self.reject(state)

    def test_zero_identity_and_zero_nonce_pair(self):
        for fields in (('pid',), ('thread',), ('creation',), ('nonceLow','nonceHigh')):
            state = sample()
            for row, window in zip(state[0], state[1]):
                for field in fields: row[field] = window['host'][field] = 0
            with self.subTest(fields=fields): self.reject(state)

    def test_numeric_types_ranges_and_widths(self):
        for field in (*reader.IDENTITY, *reader.COUNTERS):
            for bad in (True, False, -1, 1.0, '1', None, 1 << 64):
                state = sample(); state[0][2][field] = bad
                with self.subTest(field=field, bad=bad): self.reject(state)
        for field in ('pid', 'thread'):
            state = sample()
            for row, window in zip(state[0], state[1]):
                row[field] = window['host'][field] = 1 << 32
            with self.subTest(field=field): self.reject(state)

    def test_token_byte_input_not_float_bool_or_nonword(self):
        for bad in (320.0, True, 0, -1, 321, 1 << 64):
            state = sample(); self.reject((state[0], state[1], bad))

    def test_standalone_host_types_cannot_alias_integer_identity(self):
        state=sample(); state[1][2]['host']['pid']=202.0; self.reject(state)
        state=sample(); state[1][2]['host']['cpu']['valid']=1; self.reject(state)
        state=sample(); state[1][2]['host']['cpu']['kinds']['vertexShader']['cpuSizeBytes']=20.0
        self.reject(state)

    def test_every_field_required_and_unknown_field_rejected(self):
        for field in sample()[0][1]:
            state = sample(); del state[0][1][field]
            with self.subTest(field=field): self.reject(state)
        state = sample(); state[0][1]['unknown'] = 0; self.reject(state)

    def test_wrong_schema_and_scope_booleans(self):
        for field, values in (('schema', ('warvk-d3d9-shader-cache/v2', None)),
                              ('includesPipelineOrDriverBytes', (True, 0, 'false', None))):
            for bad in values:
                state = sample(); state[0][2][field] = bad
                with self.subTest(field=field,bad=bad): self.reject(state)

    def test_wrong_counter_populations(self):
        for field in reader.COUNTERS:
            for index in range(8):
                state = sample(); state[0][index][field] += 1
                with self.subTest(field=field,index=index): self.reject(state)

    def test_metadata_size_must_equal_consumed_host_size(self):
        state = sample()
        for row in state[0][1:]: row['constantBytes'] = 40
        self.reject(state)

    def test_cookie_shape_types_population(self):
        for name in ('vertexCookies','pixelCookies'):
            for bad in (None, (), [], [1]*3, [1]*5, [True,0,0,0], [-1,0,0,0],
                        [1 << 64,0,0,0], [1.0,0,0,0], [1,2,0,0], [0]*4):
                state=sample(); state[0][2][name]=bad
                with self.subTest(name=name,bad=bad): self.reject(state)

    def test_cookies_may_occupy_another_typed_slot(self):
        state=sample()
        for row in state[0]:
            row['vertexCookies'].reverse(); row['pixelCookies'].reverse()
        self.assertTrue(reader.check_cache(*state)['ok'])

    def test_cookie_stage_alias_and_recreation_change(self):
        state=sample(); state[0][1]['pixelCookies']=[0,0,0x100000001,0]; self.reject(state)
        state=sample(); state[0][-1]['vertexCookies']=[0,0x100000003,0,0]; self.reject(state)

    def test_drained_cookie_population_and_empty_baseline(self):
        state=sample(); state[0][3]['vertexCookies']=[1,0,0,0]; self.reject(state)
        for field in ('keyBytes','keyCapacityBytes','constantBytes','constantCapacityBytes'):
            state=sample(); state[0][0][field]=4
            with self.subTest(field=field): self.reject(state)

    def test_mutations_kill_579_with_valid_baseline_and_preserve_inputs(self):
        state=sample(); before=copy.deepcopy(state)
        result=reader.mutations(*state)
        self.assertEqual(result['tested'],579); self.assertEqual(result['killed'],579)
        self.assertFalse(result['gpuRerun']); self.assertEqual(state,before)
        self.assertEqual(result['source'],'supplied-cache-observations')

    def test_mutations_refuse_invalid_original(self):
        state=sample(); state[0][2]['requests']+=1
        with self.assertRaises(ValueError): reader.mutations(*state)

    def test_cache_text_parse_preserves_large_integers_and_ignores_other_logs(self):
        rows=sample()[0]
        text='ordinary log\n'+ '\n'.join(reader.PREFIX+json.dumps(row) for row in rows)
        self.assertEqual(reader.cache_rows(text),rows)

    def test_cache_text_malformed_prefix_duplicate_key_nonfinite(self):
        for text in ('x'+reader.PREFIX+'{}', ' '+reader.PREFIX+'{}',
                     'D3D9 shader-cache {}',reader.PREFIX+'{"a":0,"a":1}',
                     reader.PREFIX+'{"a":NaN}', reader.PREFIX+'{"a":Infinity}',reader.PREFIX+'{'):
            with self.subTest(text=text), self.assertRaises(ValueError): reader.cache_rows(text)


class MainBindingTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='warvk-synthetic-cache-reader-')
        self.addCleanup(self.temporary.cleanup)
        self.directory=Path(self.temporary.name).resolve()
        self.case_directory=self.directory/'memory-account'; self.case_directory.mkdir()
        self.stdout=self.case_directory/'case.stdout'; self.hostlog=self.case_directory/'host.log'
        self.stderr=self.case_directory/'case.stderr'; self.stderr.write_bytes(b'')
        self.source=self.directory/'synthetic-source.txt'; self.binary=self.directory/'not-an-executable.txt'
        self.source.write_text('fabricated source marker, not production',encoding='utf-8')
        self.binary.write_text('fabricated binary marker; never executable',encoding='utf-8')
        self.dependency=self.directory/'synthetic-dependency.txt'
        self.dependency.write_text('independently expected source dependency',encoding='utf-8')
        self.compiler=self.directory/'synthetic-compiler.txt'
        self.compiler.write_text('fake compiler marker; never executable',encoding='utf-8')
        self.compile_commands=self.directory/'synthetic-compile-commands.json'
        self.compile_commands.write_text('[]',encoding='utf-8')
        self.reference=self.directory/'synthetic-reference.txt'; self.reference.write_bytes(b'reference')
        self.player=self.directory/'synthetic-player.txt'; self.player.write_bytes(b'player')
        self.executed_copy=self.directory/'synthetic-executed-copy.txt'
        self.executed_copy.write_bytes(self.binary.read_bytes())
        self.probe_names=['probe.stdout','probe.stderr','desktop-child.log']
        for name in self.probe_names: (self.directory/name).write_bytes(b'synthetic probe bytes\n')
        probe_raw=reader.memory.capture_raw_artifacts(self.directory,self.probe_names)
        step_raw=[]
        for bits in (32,64):
            for name in ('resource_registry','wire'):
                names=[f'{name}-{bits}.stdout',f'{name}-{bits}.stderr']
                for raw_name in names: (self.directory/raw_name).write_bytes(b'synthetic CPU step bytes\n')
                step_raw.append(reader.memory.capture_raw_artifacts(self.directory,names))
        reports,hosts,launch=records(); launch.update(stdout=str(self.stdout),stderr=str(self.stderr))
        self.stdout.write_text('\n'.join(json.dumps(row) for row in reports)+'\n',encoding='utf-8')
        self.hostlog.write_text('\n'.join(json.dumps(row) for row in hosts)+'\n'+
            '\n'.join(reader.PREFIX+json.dumps(row) for row in sample()[0])+'\n',encoding='utf-8')
        self.baseline=dict(schema=1,scope='M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT',ok=True,
            sourcesPreserved=True,sourceAndReferencePreserved=True,playerDllChanged=False,
            desktopName=DESKTOP,desktopSafety=dict(ok=True,inputBefore=INPUT),
            sources=[reader.identity(self.source),reader.identity(self.dependency)],
            binaries=[reader.identity(self.binary)],compilers=[reader.identity(self.compiler)],
            compileCommands=reader.identity(self.compile_commands),
            referenceBefore=reader.identity(self.reference),referenceAfter=reader.identity(self.reference),
            playerBefore=reader.identity(self.player),playerAfter=reader.identity(self.player),
            caseCopies=[dict(source=reader.identity(self.binary),copy=reader.identity(self.executed_copy))],
            rawArtifactsPreserved=True,probeRawArtifacts=probe_raw,stepRawArtifacts=step_raw,
            cases=[dict(ok=True,label='memory-account',process=launch,
                artifacts=[reader.identity(self.stdout),reader.identity(self.hostlog),reader.identity(self.stderr)],
                initialRawArtifacts=reader.memory.capture_raw_artifacts(self.case_directory,
                    ['case.stdout','case.stderr','host.log'],True))])
        self.receipt=self.directory/'synthetic-receipt.json'; self.output=self.directory/'synthetic-analysis.json'

    def run_main(self, expected=0, hook=None):
        self.receipt.write_text(json.dumps(self.baseline),encoding='utf-8')
        with mock.patch('sys.argv',['reader','--receipt',str(self.receipt),'--output',str(self.output)]), \
             mock.patch.object(reader.memory,'source_set',return_value={self.source,self.dependency}), \
             contextlib.redirect_stdout(io.StringIO()):
            if hook:
                with hook: result=reader.main()
            else: result=reader.main()
        self.assertEqual(result,expected)
        report=json.loads(self.output.read_text('utf-8'))
        self.assertEqual(report['ok'],expected==0)
        return report

    def test_synthetic_end_to_end_reader_only(self):
        result=self.run_main()
        self.assertFalse(result['gpuRun'])
        self.assertTrue(result['completeCurrentM0SourceSetVerified'])
        self.assertTrue(result['initialRawArtifactsPreserved'])
        self.assertTrue(result['analysis']['sameBytecodeReuseProven'])
        self.assertFalse(result['analysis']['gameMemorySavingsProven'])

    def test_stdout_must_be_frozen(self):
        self.baseline['cases'][0]['artifacts']=self.baseline['cases'][0]['artifacts'][1:]
        self.run_main(1)

    def test_hostlog_must_be_frozen(self):
        self.baseline['cases'][0]['artifacts']=self.baseline['cases'][0]['artifacts'][:1]
        self.run_main(1)

    def test_supplied_source_subset_cannot_define_expected_source_set(self):
        self.baseline['sources']=self.baseline['sources'][:1]
        report=self.run_main(1)
        self.assertIn('current source set differs',report['error'])

    def test_missing_full_baseline_dependencies_rejected(self):
        original=copy.deepcopy(self.baseline)
        for name in ('sourcesPreserved','compilers','compileCommands','referenceBefore',
                     'referenceAfter','playerBefore','playerAfter','caseCopies'):
            self.baseline=copy.deepcopy(original); del self.baseline[name]
            with self.subTest(name=name): self.run_main(1)
            self.output.unlink()

    def test_original_build_and_copy_drift_rejected(self):
        for path in (self.compiler,self.compile_commands,self.reference,self.player,self.executed_copy):
            original=path.read_bytes(); path.write_bytes(original+b'changed')
            with self.subTest(path=path.name): self.run_main(1)
            self.output.unlink(); path.write_bytes(original)

    def test_rehashed_copy_with_different_bytes_rejected(self):
        self.executed_copy.write_bytes(b'new bytes are not the frozen build binary')
        self.baseline['caseCopies'][0]['copy']=reader.identity(self.executed_copy)
        report=self.run_main(1)
        self.assertIn('executed copy mismatch',report['error'])

    def test_old_receipt_without_initial_raw_manifests_rejected(self):
        original=copy.deepcopy(self.baseline)
        for name in ('rawArtifactsPreserved','probeRawArtifacts','stepRawArtifacts','initialRawArtifacts'):
            self.baseline=copy.deepcopy(original)
            target=self.baseline['cases'][0] if name == 'initialRawArtifacts' else self.baseline
            del target[name]
            with self.subTest(name=name): self.run_main(1)
            self.output.unlink()

    def test_initial_probe_and_cpu_step_drift_rejected(self):
        names=self.probe_names+[f'{name}-{bits}.{suffix}' for bits in (32,64)
            for name in ('resource_registry','wire') for suffix in ('stdout','stderr')]
        for name in names:
            path=self.directory/name; original=path.read_bytes()
            path.write_bytes(original+b'ERROR: appended non-JSON diagnostic\n')
            with self.subTest(name=name): self.run_main(1)
            self.output.unlink(); path.write_bytes(original)

    def test_rehashing_case_artifacts_cannot_bless_late_raw_drift(self):
        self.stderr.write_bytes(b'ERROR: added after launch return\n')
        self.baseline['cases'][0]['artifacts'][-1]=reader.identity(self.stderr)
        report=self.run_main(1)
        self.assertIn('changed since launch return',report['error'])

    def test_initial_cpu_step_population_and_order_rejected(self):
        original=copy.deepcopy(self.baseline['stepRawArtifacts'])
        for value in (original[:-1],list(reversed(original)),[original[0]]*4):
            self.baseline['stepRawArtifacts']=value
            with self.subTest(value=value[0]['requiredNames']): self.run_main(1)
            self.output.unlink()

    def test_late_case_log_population_rejected(self):
        (self.case_directory/'late.log').write_bytes(b'late non-JSON diagnostic')
        report=self.run_main(1)
        self.assertIn('population changed',report['error'])

    def test_pre_read_source_drift_rejected(self):
        self.source.write_text('changed',encoding='utf-8'); self.run_main(1)

    def test_drift_during_parse_rejected(self):
        original=reader.cache_rows
        def during(text):
            self.source.write_text('changed after identity check',encoding='utf-8')
            return original(text)
        self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))

    def test_stdout_drift_during_parse_rejected(self):
        original=reader.cache_rows
        def during(text):
            with self.stdout.open('a',encoding='utf-8') as stream: stream.write('late change\n')
            return original(text)
        self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))

    def test_hostlog_drift_during_parse_rejected(self):
        original=reader.cache_rows
        def during(text):
            with self.hostlog.open('a',encoding='utf-8') as stream: stream.write('late change\n')
            return original(text)
        self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))

    def test_binary_drift_during_parse_rejected(self):
        original=reader.cache_rows
        def during(text):
            self.binary.write_text('changed fake binary',encoding='utf-8')
            return original(text)
        self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))

    def test_build_lineage_rechecked_after_analysis(self):
        original=reader.cache_rows
        def during(text):
            self.compiler.write_bytes(b'changed compiler marker during cache analysis')
            return original(text)
        report=self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))
        self.assertIn('file changed',report['error'])

    def test_initial_raw_rechecked_after_analysis(self):
        original=reader.cache_rows
        for path in (self.directory/'probe.stderr',self.directory/'wire-64.stderr',self.stderr):
            before=path.read_bytes()
            def during(text):
                path.write_bytes(before+b'ERROR: late non-JSON diagnostic\n')
                return original(text)
            with self.subTest(path=path.name):
                self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))
            self.output.unlink(); path.write_bytes(before)

    def test_current_source_population_rechecked_after_analysis(self):
        original=reader.cache_rows
        def during(text):
            reader.memory.source_set.return_value={self.source}
            return original(text)
        report=self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))
        self.assertIn('current source set differs',report['error'])

    def test_baseline_change_during_parse_rejected(self):
        original=reader.cache_rows
        def during(text):
            self.receipt.write_text('changed baseline',encoding='utf-8'); return original(text)
        self.run_main(1,mock.patch.object(reader,'cache_rows',side_effect=during))

    def test_invalid_root_and_unsafe_run_rejected(self):
        original=copy.deepcopy(self.baseline)
        for key,bad in (('ok',False),('sourceAndReferencePreserved',False),('playerDllChanged',True),
                        ('sourcesPreserved',False),('schema',True),('scope','unrelated')):
            self.baseline=copy.deepcopy(original); self.baseline[key]=bad
            with self.subTest(key=key): self.run_main(1)
            self.output.unlink()

    def test_upper_M0_checker_still_rejects_real_wire_mismatch(self):
        reports= [json.loads(line) for line in self.stdout.read_text('utf-8').splitlines()]
        reports[4]['parent']['expectedSequence'] += 1
        self.stdout.write_text('\n'.join(json.dumps(row) for row in reports)+'\n',encoding='utf-8')
        self.baseline['cases'][0]['artifacts'][0]=reader.identity(self.stdout)
        # Deliberately recapture this fabricated fixture so rejection proves
        # the full M0 wire reader ran, not only the raw-byte preservation gate.
        self.baseline['cases'][0]['initialRawArtifacts']=reader.memory.capture_raw_artifacts(
            self.case_directory,['case.stdout','case.stderr','host.log'],True)
        report=self.run_main(1)
        self.assertNotIn('changed since launch return',report['error'])

    def test_output_is_never_overwritten(self):
        self.output.write_text('preserve',encoding='utf-8')
        with self.assertRaises(ValueError): self.run_main()
        self.assertEqual(self.output.read_text('utf-8'),'preserve')

    def test_late_output_creation_is_never_overwritten(self):
        original=reader.cache_rows
        def during(text):
            self.output.write_text('created by another actor',encoding='utf-8')
            return original(text)
        with self.assertRaises(FileExistsError):
            self.run_main(hook=mock.patch.object(reader,'cache_rows',side_effect=during))
        self.assertEqual(self.output.read_text('utf-8'),'created by another actor')


if __name__ == '__main__':
    unittest.main()
