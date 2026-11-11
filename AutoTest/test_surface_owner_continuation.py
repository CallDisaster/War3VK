"""Pure candidate tests. No compilation, native execution, process query or GPU.

Frozen r1 logs are re-read, never regenerated; state fixtures are synthetic.
No global PRESETS, timeouts, source files or actual readers are monkeypatched.
"""
import ast
import copy
from dataclasses import replace
import inspect
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_surface_owner_continuation as m
import run_render_host_presenter_gate as presenter
from test_render_host_initialization import settled


def launch(case):
    result = settled()
    result['timeoutSeconds'] = case.seconds
    return result


def outcome(case, passed=True, safe=True):
    return dict(index=case.index, kind=case.kind, mode=case.mode, scenario=case.scenario,
                workloadPassed=passed, settlementCertified=safe, acceptancePassed=False)


class PlanTests(unittest.TestCase):
    def test_exact_fixed_twelve_including_second_abba(self):
        self.assertEqual(len(m.CASES), 12)
        self.assertEqual([c.index for c in m.CASES], list(range(1, 13)))
        self.assertEqual([c.mode for c in m.CASES[:4]], ['peer', 'host', 'host', 'peer'])
        self.assertEqual([(c.scenario, c.mode) for c in m.CASES[4:10]],
            [(s, arm) for s in ('marker', 'extent', 'destroy') for arm in ('peer', 'host')])
        self.assertEqual([(c.kind, c.mode) for c in m.CASES[10:]], [('shader', 'peer'), ('shader', 'host')])
        self.assertEqual(len({c.label for c in m.CASES}), 12)

    def test_all_plan_fields_cannot_be_changed(self):
        for case in m.CASES:
            for field, value in (('index', 0), ('kind', 'other'), ('mode', 'other'),
                                 ('scenario', 'control'), ('seconds', case.seconds+1)):
                with self.subTest(case=case.index, field=field), self.assertRaises(ValueError):
                    m.checked_case(replace(case, **{field: value}))
        with self.assertRaises(ValueError): m.checked_case(replace(m.CASES[0], index=True))

    def test_arguments_preserve_original_workloads(self):
        for case in m.CASES:
            args = m.arguments(case, Path('parent.exe'), Path('proxy.dll'), 'Private', 'Default')
            self.assertEqual(args[:5], ['parent.exe', '--dll', 'proxy.dll', '--scenario', case.scenario])
            self.assertNotIn('--live-resource', args)
            if case.kind == 'shader':
                self.assertEqual(args[5:], ['--wsi-readback'])
                self.assertEqual(case.seconds, 90)
                self.assertEqual(case.monitor_preset, 'r9-readback')
            else:
                self.assertEqual(args[-4:], ['--expect-desktop', 'Private', '--input-desktop', 'Default'])
                self.assertEqual(case.seconds, 45)
                self.assertEqual(case.monitor_preset, 'r8-clear')
                self.assertEqual('--diagnostic-readback' in args, case.kind == 'soak')

    def test_failed_workloads_are_retained_and_not_retried(self):
        seen = []
        def run(case):
            seen.append(case.index)
            return outcome(case, passed=case.index not in (1, 5, 11))
        rows = m.execute_matrix(run)
        self.assertEqual(seen, list(range(1, 13)))
        self.assertEqual([r['index'] for r in rows if not r['workloadPassed']], [1, 5, 11])
        self.assertTrue(all(r['acceptancePassed'] is False for r in rows))

    def test_uncertain_cleanup_stops_before_next_case(self):
        for stop in (1, 5, 12):
            seen = []
            def run(case):
                seen.append(case.index)
                return outcome(case, safe=case.index != stop)
            with self.assertRaises(ValueError): m.execute_matrix(run)
            self.assertEqual(seen, list(range(1, stop+1)))

    def test_launch_exception_cannot_start_another_case(self):
        seen = []
        def run(case):
            seen.append(case.index)
            if case.index == 4: raise RuntimeError('synthetic launch interrupted')
            return outcome(case)
        with self.assertRaises(RuntimeError): m.execute_matrix(run)
        self.assertEqual(seen, [1, 2, 3, 4])

    def test_wrong_scope_or_truthy_settlement_cannot_advance(self):
        for field, value in (('index', True), ('mode', 'host'), ('acceptancePassed', True),
                             ('settlementCertified', 1), ('scenario', 'normal')):
            def run(case):
                row = outcome(case); row[field] = value
                return row
            with self.subTest(field=field), self.assertRaises(ValueError): m.execute_matrix(run)


class EnvironmentTests(unittest.TestCase):
    def test_allowlist_and_default_case_profiles(self):
        original = {'Path': 'tools', 'SECRET_TOKEN': 'not-inherited',
            'VK_INSTANCE_LAYERS': 'not-inherited', 'WARVK_DIAGNOSTIC_SURFACE_OWNER': 'not-inherited'}
        before = dict(original)
        for case in m.CASES:
            env = m.environment(case, original, Path('case'), Path('host.exe'))
            self.assertEqual(env['PATH'], 'tools')
            self.assertNotIn('SECRET_TOKEN', env)
            self.assertNotIn('VK_INSTANCE_LAYERS', env)
            self.assertEqual(env['WARVK_DIAGNOSTIC_SURFACE_OWNER'], case.mode)
            self.assertEqual(env['WARVK_D3D9_PROFILE'],
                'presenter-isolated-v1' if case.kind == 'fault' else 'presenter-readback-isolated-v1')
        self.assertEqual(original, before)

    def test_optional_controls_are_explicit_and_do_not_modify_input(self):
        controls = {'VK_LOADER_DEBUG': 'error,warn,info,layer', 'DXVK_DEBUG': 'validation'}
        before = dict(controls)
        env = m.environment(m.CASES[0], {}, Path('case'), Path('host.exe'), controls)
        self.assertEqual({k: env[k] for k in controls}, controls)
        self.assertEqual(controls, before)
        self.assertNotIn('DXVK_DEBUG', m.environment(m.CASES[0], {}, Path('case'), Path('host.exe')))

    def test_real_validation_controls_fit_without_enabling_vvl(self):
        from run_render_host_validation_gate import validation_env
        controls = validation_env(Path('case'), Path('sdk/VkLayer_khronos_validation.json'))
        env = m.environment(m.CASES[0], {}, Path('case'), Path('host.exe'), controls)
        self.assertEqual(env['VK_KHRONOS_VALIDATION_LOG_FILENAME'], str(Path('case/validation.raw.log')))
        self.assertEqual(env['WARVK_D3D9_HOST_EXE'], 'host.exe')

    def test_controls_cannot_override_identity_or_inherit_arbitrary_inputs(self):
        for controls in ([], [('VK_X', '1')], {'VK_X': 1}, {'vk_x': '1'}, {'VK_X': 'x\x00'},
                         {'PATH': 'replacement'}, {'WARVK_D3D9_HOST_EXE': 'other.exe'},
                         {'WARVK_D3D9_PROFILE': 'other'}, {'WARVK_DIAGNOSTIC_SURFACE_OWNER': 'host'},
                         {'DXVK_CONFIG_FILE': 'existing.conf'}, {'UNRELATED_SECRET': 'x'}):
            with self.subTest(controls=controls), self.assertRaises(ValueError):
                m.environment(m.CASES[0], {}, Path('case'), Path('host.exe'), controls)

    def test_extra_raw_names_have_no_paths_reuse_or_duplicates(self):
        self.assertEqual(m.checked_extra_raw_names(None), [])
        self.assertEqual(m.checked_extra_raw_names(['validation.raw.log']), ['validation.raw.log'])
        for value in ('a.log', ('a.log',), ['a.log', 'a.log'], ['host.log'], ['../x'], ['a/b'],
                      ['a\\b'], ['C:x'], ['.'], ['..'], ['x\x00'], ['exit-snapshot-1.json'], [1]):
            with self.subTest(value=value), self.assertRaises(ValueError): m.checked_extra_raw_names(value)


class SettlementTests(unittest.TestCase):
    def test_original_45_and_90_receipts_not_normalized(self):
        for case in (m.CASES[0], m.CASES[4], m.CASES[10]):
            for code in (0, 2):
                record = launch(case); record['exitCode'] = code
                before = copy.deepcopy(record)
                m.check_settlement(case, record, 'PrivateNew', 'Default')
                self.assertEqual(record, before)

    def test_all_unsafe_cleanup_bits_refuse(self):
        for case in (m.CASES[0], m.CASES[10]):
            mutations = [('jobActiveProcesses', 1), ('jobActiveProcesses', False), ('exitCode', True),
                ('exitCode', 4), ('exitCode', 259), ('timeoutSeconds', case.seconds+1),
                ('processSettled', False), ('jobEmpty', False), ('jobAtomic', False), ('handleListApplied', False),
                ('timedOut', True), ('forcedTermination', True), ('desktopHandleInherited', True),
                ('cleanupErrors', ['unsettled']), ('actualExecutable', 'wrong.exe'), ('pid', False),
                ('initialThreadId', 0), ('creation100ns', 0), ('inputAfter', 'changed'), ('desktop', 'Default')]
            for key, value in mutations:
                changed = launch(case); changed[key] = value
                with self.subTest(case=case.index, key=key), self.assertRaises((ValueError, KeyError, TypeError)):
                    m.check_settlement(case, changed, 'PrivateNew', 'Default')

    def test_medium_token_contract_cannot_be_relaxed(self):
        for case in (m.CASES[0], m.CASES[10]):
            changes = [('mode', 'high'), ('childTokenVerifiedBeforeResume', False), ('derivedTokenClosed', False),
                ('resumedAfterVerification', False), ('resumePreviousCount', True), ('parentTokenUnchanged', False),
                ('noHighIntegrityFallback', False), ('globalSecurityChanged', True), ('cleanupErrors', ['bad'])]
            for key, value in changes:
                changed = launch(case); changed['restrictedToken'][key] = value
                with self.subTest(case=case.index, key=key), self.assertRaises(ValueError):
                    m.check_settlement(case, changed, 'PrivateNew', 'Default')
            for key, value in (('integrityRid', 12288), ('integrityRid', True),
                               ('integritySid', 'S-1-16-12288'), ('hasRestrictions', False)):
                changed = launch(case); changed['restrictedToken']['childBeforeResume'][key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    m.check_settlement(case, changed, 'PrivateNew', 'Default')


class FrozenReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old = m.strict_json(m.BASELINE.read_text('utf-8'))

    def test_frozen_r1_hash_and_strict_actual_raw_reread(self):
        self.assertEqual(m.identity(m.BASELINE)['sha256'], m.BASELINE_SHA)
        self.assertEqual(self.old['diagnosticHost']['sha256'], m.HOST_SHA)
        m.validate_baseline(self.old)

    def test_all_frozen_r10_r1_inputs_rehash_without_execution(self):
        _, old, binaries, frozen = m.bind_inputs()
        self.assertFalse(old['ok'])
        self.assertEqual(set(binaries), set(m.BINARY_PINS)|{'host'})
        self.assertGreater(len(frozen), 100)
        self.assertEqual(m.identity(binaries['host'])['sha256'], m.HOST_SHA)

    def test_stale_or_edited_baseline_receipt_refused(self):
        for key, value in (('acceptancePassed', True), ('diagnosticComplete', False),
                           ('sameExeRequired', False), ('arms', ['host']*4)):
            changed = copy.deepcopy(self.old); changed[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): m.validate_baseline(changed)
        changed = copy.deepcopy(self.old); changed['diagnosticHost']['sha256'] = '0'*64
        with self.assertRaises(ValueError): m.validate_baseline(changed)

    def test_record_population_rejects_duplicate_and_wrong_workload(self):
        row = self.old['cases'][0]
        m.record_population(m.CASES[0], row['reports'], row['hostReports'])
        with self.assertRaises(ValueError):
            m.record_population(m.CASES[0], row['reports']+[row['reports'][0]], row['hostReports'])
        for case in (m.CASES[4], m.CASES[10]):
            with self.assertRaises(ValueError): m.record_population(case, row['reports'], row['hostReports'])

    def test_original_workload_readers_are_called_not_reimplemented(self):
        self.assertIs(m.check_soak, presenter.check_soak)
        self.assertIs(m.check_fault, presenter.check_fault)
        self.assertIs(m.check_wsi, presenter.check_wsi)
        source = inspect.getsource(m.check_workload)
        self.assertIn('readback=False, live_resource=False', source)
        self.assertIn('check_wsi(f, proxy, host, witness, desktop, True, launch, input_desktop)', source)

    def test_real_soak_predicate_mutations_and_original_exit_limit(self):
        row = self.old['cases'][0]; case = m.CASES[0]; process = row['process']
        records = m.record_population(case, row['reports'], row['hostReports'])
        m.check_workload(case, records, process, process['desktop'], process['inputBefore'])
        for index, key, value in ((0, 'frames', 255), (0, 'pixelFailures', 1), (1, 'primaryExitWaitMs', 5001),
                                 (1, 'primaryExitWaitMs', True), (1, 'childNaturalExit', False),
                                 (2, 'retired', 512), (2, 'liveResources', 1), (3, 'peerPid', 0)):
            changed = copy.deepcopy(records); changed[index][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                m.check_workload(case, changed, process, process['desktop'], process['inputBefore'])

    def test_monitor_must_bind_the_actual_child(self):
        row = self.old['cases'][0]; process = row['process']; directory = Path(process['argv'][0]).parent
        args = (process, directory/'d3d9-presenter-fault32.exe', directory/'warvk-d3d9-host-dev.exe',
                process['desktop'], process['inputBefore'])
        m.bind_monitor(row['monitor'], *args)
        for role, key, value in (('parent', 'pid', 1), ('host', 'creation100ns', 1),
                                 ('host', 'image', 'other.exe'), ('parent', 'active', False)):
            changed = copy.deepcopy(row['monitor']); changed['binding'][role][key] = value
            with self.subTest(role=role, key=key), self.assertRaises(ValueError): m.bind_monitor(changed, *args)
        changed = copy.deepcopy(row['monitor']); changed['errors'] = ['query-failed']
        with self.assertRaises(ValueError): m.bind_monitor(changed, *args)

    def test_conflicting_file_identity_never_silently_refreshed(self):
        item = dict(path=str(m.BASELINE), bytes=1, sha256='A'*64)
        self.assertEqual(m.unique_inputs([item, dict(item)]), [item])
        with self.assertRaises(ValueError): m.unique_inputs([item, item|{'sha256': 'B'*64}])

    def test_only_explicit_reader_preimages_can_be_revised(self):
        revisions = m.reader_revisions()
        self.assertEqual(len(revisions), 2)
        for row in revisions:
            before = row['archivedPreimage'] | {'path': row['originalPath']}
            self.assertEqual(m.revised_inputs([before]), [row['archivedPreimage'], row['current']])
            with self.assertRaises(ValueError): m.revised_inputs([before | {'sha256': '0'*64}])
            with self.assertRaises(ValueError): m.revised_inputs([row['current']])
        ordinary = dict(path=str(m.ROOT/'tools/render_host/dxvk_core/d3d9_backend.cpp'), bytes=1, sha256='F'*64)
        self.assertEqual(m.revised_inputs([ordinary]), [ordinary])


class StructuralSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(m.__file__).read_text('utf-8')
        cls.tree = ast.parse(cls.source)

    def test_no_build_or_global_preset_timeout_mutation(self):
        for forbidden in ('build_diagnostic(', 'instrument_source(', 'subprocess.', 'os.environ[',
                          'PRESETS[', 'os.system(', 'unlink(', 'rmtree('):
            self.assertFalse(forbidden in self.source, forbidden)
        writes = [n.attr for n in ast.walk(self.tree) if isinstance(n, ast.Attribute)
                  and isinstance(n.ctx, ast.Store)]
        self.assertNotIn('launcher_timeout', writes)
        self.assertIn("if args.execute_native:\n            run_native(", self.source)
        self.assertIn('buildsAllowed=False', self.source)

    def test_default_matrix_does_not_enable_optional_controls(self):
        calls = [n for n in ast.walk(self.tree) if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Name) and n.func.id == 'run_one_case']
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].keywords, [])
        self.assertNotIn("add_argument('--controls", self.source)
        self.assertNotIn("add_argument('--case", self.source)

    def test_raw_captured_before_settlement_and_parser(self):
        block = inspect.getsource(m.run_one_case)
        positions = [block.index(name) for name in ('desktop.run(', 'capture_raw_artifacts(',
            'check_settlement(', 'child_settled(', "row['settlementCertified'] = True", 'json_lines(')]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('init.RAW_NAMES+extras+', block)
        self.assertIn("row['actualSafeEnvironment'] = env", block)
        self.assertIn("row['settlementError'] = str(error)\n        raise", block)


if __name__ == '__main__':
    unittest.main()
