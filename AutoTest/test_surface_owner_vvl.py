"""Pure B2 VVL recipe/reader/sequence tests; never run native, build or GPU.

Synthetic strings are parser inputs only. Existing installed files may be read
for PE/hash inspection; no DLL is loaded. Formal inputs and old receipts remain
unchanged, and neither the C2 gate nor any global controls are monkeypatched.
"""
import ast
import copy
from dataclasses import asdict, replace
import inspect
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import diagnose_surface_owner_vvl as m
from render_lab_layer_source import NO_SETTINGS
from test_render_host_validation_gate import vvl_record, DXVK_LOG

ARTIFACT = (m.ROOT/'AutoTest/artifacts/b2-vvl-pure-model-not-created').resolve()
MANIFEST = ARTIFACT/'sdk/VkLayer_khronos_validation.json'
LIBRARY = MANIFEST.parent/'VkLayer_khronos_validation.dll'
SOURCE = ARTIFACT/'empty-implicit-layers'
LOADER = Path('C:/Windows/System32/vulkan-1.dll')


def loader_text():
    return '\n'.join((
        'INFO: '+NO_SETTINGS,
        'INFO: Using Vulkan Loader '+str(LOADER),
        'INFO: Vulkan Loader Version 1.4.341',
        'LAYER: Searching for implicit layer manifest files',
        'LAYER: In following locations:', 'LAYER: '+str(SOURCE), 'LAYER: Found no files',
        'LAYER: Loading layer library '+str(LIBRARY),
        f'LAYER: Insert instance layer "{m.validation.LAYER}" ({LIBRARY})',
        f'LAYER: Inserted device layer "{m.validation.LAYER}" ({LIBRARY})'))+'\n'


def result_row(case, safe=True, passed=True):
    directory = ARTIFACT/case.label
    controls = m.controls_for(case, directory, MANIFEST, SOURCE)
    return dict(**asdict(case), acceptancePassed=False, settlementCertified=safe,
        childSettlement='exact-process-handle-signaled', workloadPassed=passed,
        diagnosticCollected=True, requestedDiagnosticControls=controls,
        actualSafeEnvironment=m.continuation.environment(case, {'PATH': 'synthetic'}, directory,
            directory/'warvk-d3d9-host-dev.exe', controls), extraRawNames=list(m.EXTRA_RAW_NAMES),
        vvlDiagnostic=dict(checksPassed=passed, caseChecksPassed=passed))


class RecipeTests(unittest.TestCase):
    def test_exact_two_host_cases_original_deadlines(self):
        self.assertEqual(m.CASES, (m.continuation.CASES[1], m.continuation.CASES[11]))
        self.assertEqual([(c.index, c.mode, c.kind, c.seconds) for c in m.CASES],
                         [(2, 'host', 'soak', 45), (12, 'host', 'shader', 90)])
        for case in m.continuation.CASES:
            if case not in m.CASES:
                with self.subTest(index=case.index), self.assertRaises(ValueError): m.checked_case(case)
        with self.assertRaises(ValueError): m.checked_case(replace(m.CASES[0], seconds=90))

    def test_controls_are_exact_original_recipe(self):
        for case in m.CASES:
            directory = ARTIFACT/case.label
            actual = m.controls_for(case, directory, MANIFEST, SOURCE)
            self.assertEqual(actual, m.validation.validation_env(directory, MANIFEST, SOURCE))
            self.assertEqual(actual['VK_IMPLICIT_LAYER_PATH'], str(SOURCE))
            self.assertFalse(m.FORBIDDEN_CONTROLS.intersection(actual))

    def test_every_omitted_control_is_rejected(self):
        case = m.CASES[0]; directory = ARTIFACT/case.label
        controls = m.controls_for(case, directory, MANIFEST, SOURCE)
        for key in controls:
            changed = dict(controls); del changed[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                m.check_controls(changed, directory, MANIFEST, SOURCE)

    def test_arbitrary_overrides_and_icd_redirects_rejected(self):
        case = m.CASES[0]; directory = ARTIFACT/case.label
        controls = m.controls_for(case, directory, MANIFEST, SOURCE)
        for name in m.FORBIDDEN_CONTROLS | {'VK_OTHER', 'WARVK_D3D9_HOST_EXE', 'SECRET_TOKEN'}:
            with self.subTest(name=name), self.assertRaises(ValueError):
                m.check_controls(controls | {name: 'anything'}, directory, MANIFEST, SOURCE)
        for key in controls:
            with self.subTest(key=key), self.assertRaises(ValueError):
                m.check_controls(controls | {key: controls[key]+'-different'}, directory, MANIFEST, SOURCE)

    def test_control_types_are_not_coerced(self):
        case = m.CASES[0]; directory = ARTIFACT/case.label
        controls = m.controls_for(case, directory, MANIFEST, SOURCE)
        for value in (None, [], tuple(controls.items()), controls | {'DXVK_SHADER_CACHE': 0}):
            with self.subTest(value=type(value)), self.assertRaises(ValueError):
                m.check_controls(value, directory, MANIFEST, SOURCE)

    def test_actual_environment_not_just_requested_controls(self):
        for case in m.CASES:
            row = result_row(case); directory = ARTIFACT/case.label
            before = copy.deepcopy(row)
            m.check_actual_environment(case, row, directory, MANIFEST, SOURCE)
            self.assertEqual(row, before)
            for key, value in (('VK_DRIVER_FILES', 'other.json'), ('VK_LAYER_PATH', 'other'),
                ('WARVK_D3D9_PROFILE', 'other'), ('WARVK_DIAGNOSTIC_SURFACE_OWNER', 'peer'),
                ('WARVK_D3D9_HOST_EXE', 'other.exe'), ('TOKEN', 'not-allowed')):
                changed = copy.deepcopy(row); changed['actualSafeEnvironment'][key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    m.check_actual_environment(case, changed, directory, MANIFEST, SOURCE)

    def test_extra_logs_must_be_initially_required(self):
        case = m.CASES[0]; directory = ARTIFACT/case.label
        for names in ([], ['validation.raw.log'], [m.DXVK_LOG], ['validation.raw.log', m.DXVK_LOG, 'other.log']):
            row = result_row(case); row['extraRawNames'] = names
            with self.subTest(names=names), self.assertRaises(ValueError):
                m.check_actual_environment(case, row, directory, MANIFEST, SOURCE)

    def test_existing_layer_inspection_is_read_only_and_x64(self):
        manifest, library, loader, rows = m.inspect_frozen_layer()
        self.assertEqual(manifest, m.LAYER_MANIFEST.resolve(strict=True))
        self.assertEqual(library.name, 'VkLayer_khronos_validation.dll')
        self.assertEqual(loader.name, 'vulkan-1.dll')
        self.assertEqual(rows, [m.identity(p) for p in (manifest, library, loader)])
        self.assertEqual(m.pe_identity(library), (0x8664, 0x20b))
        self.assertEqual(m.pe_identity(loader), (0x8664, 0x20b))


class ReaderTests(unittest.TestCase):
    def parse(self, host=None, dxvk=DXVK_LOG, raw=None):
        return m.parse_evidence(loader_text() if host is None else host, dxvk,
            vvl_record() if raw is None else raw, LIBRARY, SOURCE, LOADER)

    def test_clean_model_does_not_claim_c2_or_filter_limit(self):
        result = self.parse()
        self.assertTrue(result['checksPassed'])
        self.assertTrue(result['loader']['implicitDiscoverySource']['actualDiscoveryProven'])
        self.assertTrue(result['validation']['synchronizationValidationProven'])
        self.assertFalse(result['acceptancePassed'])
        self.assertFalse(result['completeC2AcceptancePassed'])
        self.assertFalse(result['runtimeFilterLimitProven'])

    def test_nonstatus_warning_error_and_dxvk_ignored_ids_remain_failures(self):
        for severity in ('Warning', 'Performance Warning', 'Error'):
            for code in ('23259a0d', '4b9d1597', '534c50ad', '9750b479', '11b37e31',
                         '151f5e5a', '6c16bfb4', 'd6d77e1e', 'a5625282'):
                raw = vvl_record()+vvl_record(severity, 'VUID-not-filtered', code, 'actual diagnostic')
                with self.subTest(severity=severity, code=code):
                    result = self.parse(raw=raw)
                    self.assertFalse(result['checksPassed'])
                    self.assertTrue(result['errors'])
                    self.assertIn('loader', result)

    def test_loader_failure_does_not_hide_independent_activation_parse(self):
        result = self.parse(host=loader_text()+'ERROR: loader failed\n')
        self.assertFalse(result['checksPassed'])
        self.assertNotIn('loader', result)
        self.assertIn('validation', result)

    def test_both_errors_retained(self):
        result = self.parse(host='ERROR: bad\n', raw='broken')
        self.assertEqual(len(result['errors']), 2)
        self.assertTrue(result['errors'][0].startswith('loader:'))
        self.assertTrue(result['errors'][1].startswith('validation:'))

    def test_missing_changed_or_extra_loader_proof_fails(self):
        original = loader_text()
        alternatives = ('', str(SOURCE), original.replace(str(SOURCE), str(SOURCE)+'-different'),
            original.replace('Found no files', 'Found another file'), original.replace('1.4.341', '1.4.300'),
            original.replace(str(LOADER), 'C:/other/vulkan-1.dll'),
            original+f'LAYER: Insert instance layer "VK_LAYER_FOREIGN" ({LIBRARY})\n',
            original+'INFO: environment ignored because elevated\n')
        for text in alternatives:
            with self.subTest(text=text[:60]): self.assertFalse(self.parse(host=text)['checksPassed'])

    def test_no_activation_duplicate_or_sync_disabled_fails(self):
        for text in ('', vvl_record().rstrip('\n'), vvl_record()+vvl_record(),
                     vvl_record().replace(m.validation.SYNC_ENABLE, 'None'),
                     vvl_record().replace(m.validation.CACHE_DISABLE, 'OTHER_DISABLE')):
            with self.subTest(text=text[:50]): self.assertFalse(self.parse(raw=text)['checksPassed'])

    def test_original_readers_not_reimplemented(self):
        source = inspect.getsource(m.parse_evidence)
        self.assertIn('validation.check_loader(', source)
        self.assertIn('validation.parse_validation(', source)
        self.assertNotIn('replace(', source)
        self.assertNotIn('filter(', source)


class SequenceTests(unittest.TestCase):
    def test_first_failed_but_settled_case_retained_and_second_runs_once(self):
        seen = []
        def run(case):
            seen.append(case.index)
            return result_row(case, passed=case.kind == 'shader')
        rows = m.execute_two_cases(run)
        self.assertEqual(seen, [2, 12])
        self.assertFalse(rows[0]['workloadPassed'])
        self.assertTrue(rows[1]['workloadPassed'])
        self.assertTrue(all(r['acceptancePassed'] is False for r in rows))

    def test_uncertain_settlement_stops_second_case(self):
        seen = []
        def run(case):
            seen.append(case.index)
            return result_row(case, safe=False)
        with self.assertRaises(ValueError): m.execute_two_cases(run)
        self.assertEqual(seen, [2])

    def test_unknown_child_settlement_cannot_pass(self):
        for field, value in (('childSettlement', 'not-found-maybe'), ('settlementCertified', 1),
                             ('acceptancePassed', True), ('seconds', 46), ('index', True), ('mode', 'peer')):
            row = result_row(m.CASES[0]); row[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): m.check_result_scope(m.CASES[0], row)

    def test_native_exception_stops_without_retry(self):
        seen = []
        def run(case):
            seen.append(case.index)
            raise RuntimeError('synthetic unsettled process')
        with self.assertRaises(RuntimeError): m.execute_two_cases(run)
        self.assertEqual(seen, [2])

    def test_empty_sample_or_only_one_case_never_all_pass(self):
        for count in (0, 1, 2):
            receipt = dict(cases=[result_row(c) for c in m.CASES[:count]], prepared=True,
                nativeRequested=True, frozenInputsPreserved=True, rawArtifactsPreserved=True,
                implicitSourcePreserved=True)
            m.summarize(receipt)
            self.assertEqual(receipt['vvlChecksPassed'], count == 2)
            for key in ('acceptancePassed', 'completeC2AcceptancePassed', 'peerWindowAcceptancePassed'):
                self.assertIs(receipt[key], False)

    def test_missing_preservation_or_failed_case_keeps_final_red(self):
        base = dict(cases=[result_row(c) for c in m.CASES], prepared=True, nativeRequested=True,
                    frozenInputsPreserved=True, rawArtifactsPreserved=True, implicitSourcePreserved=True)
        for key in ('frozenInputsPreserved', 'rawArtifactsPreserved', 'implicitSourcePreserved'):
            receipt = copy.deepcopy(base); receipt[key] = False
            m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])
        receipt = copy.deepcopy(base); receipt['cases'][0]['vvlDiagnostic']['caseChecksPassed'] = False
        m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])
        for key in ('error', 'preservationError', 'rawPreservationError'):
            receipt = copy.deepcopy(base); receipt[key] = 'synthetic failure'
            m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])

    def test_duplicate_case_or_stale_positive_flag_cannot_certify(self):
        base = dict(cases=[result_row(c) for c in m.CASES], prepared=True, nativeRequested=True,
                    frozenInputsPreserved=True, rawArtifactsPreserved=True, implicitSourcePreserved=True)
        receipt = copy.deepcopy(base); receipt['cases'][1] = copy.deepcopy(receipt['cases'][0])
        m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])
        receipt = copy.deepcopy(base); receipt['cases'][0]['workloadPassed'] = False
        m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])
        receipt = copy.deepcopy(base); receipt['cases'][0]['vvlDiagnostic']['checksPassed'] = False
        m.summarize(receipt); self.assertFalse(receipt['vvlChecksPassed'])


class StructuralTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(m.__file__).read_text('utf-8')
        cls.tree = ast.parse(cls.source)

    def test_native_only_explicit_root_flag_no_passthrough_cli(self):
        args = [n.args[0].value for n in ast.walk(self.tree) if isinstance(n, ast.Call)
                and isinstance(n.func, ast.Attribute) and n.func.attr == 'add_argument'
                and n.args and isinstance(n.args[0], ast.Constant)]
        self.assertEqual(args, ['--output', '--execute-native'])
        self.assertIn('if args.execute_native:\n            run_native(', self.source)

    def test_no_original_c2_bypass_build_or_global_mutation(self):
        for token in ('validation.main(', 'validation.load_baseline(', 'build_diagnostic(', 'subprocess.',
                      'PRESETS[', 'shutil.', 'unlink(', 'rmtree(', 'SetEnvironmentVariable'):
            self.assertFalse(token in self.source, token)
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Subscript) and isinstance(node.ctx, ast.Store):
                self.assertNotEqual(ast.unparse(node.value), 'os.environ')
        self.assertIn('continuation.bind_inputs()', self.source)
        self.assertIn('continuation.reader_revisions()', self.source)

    def test_existing_single_case_and_initial_raw_binding(self):
        self.assertIn('continuation.run_one_case(', self.source)
        self.assertIn('controls=controls, extra_raw_names=list(EXTRA_RAW_NAMES)', self.source)
        block = inspect.getsource(m.analyze_case)
        self.assertLess(block.index('verify_raw_artifacts('), block.index("read_text('utf-8')"))
        self.assertIn("row['vvlDiagnostic'] = result", block)

    def test_medium_probe_and_private_source_are_required(self):
        self.assertIn('IsolatedRenderDesktop(restricted_medium=True)', self.source)
        self.assertIn('validate_probe_receipt(', self.source)
        self.assertIn('create_empty_source(out)', self.source)
        self.assertIn('empty_directory_witness(empty_source)', self.source)
        self.assertIn("receipt['implicitSourceBefore'] == receipt['implicitSourceAfter']", self.source)


if __name__ == '__main__':
    unittest.main()
