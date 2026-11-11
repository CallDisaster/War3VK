"""CPU-only counterexamples for the frozen-binary VVL gate.

No executable, Vulkan loader, DLL, desktop, process or service is launched here.
The synthetic strings test the reader contract, not runtime VVL activation.
"""
import copy
import json
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_render_host_validation_gate as gate


ARTIFACT = (Path(__file__).resolve().parent / 'artifacts/validation-reader-model').resolve()
LIBRARY = ARTIFACT / 'VkLayer_khronos_validation.dll'


def vvl_record(severity='Information', vuid=gate.STATUS_ID, message_id='23dfd876', body=None):
    if body is None:
        body = ('[AppName: warvk-d3d9-host-dev] vkCreateInstance(): Khronos Validation Layer Active:\n'
                f'    Current Enables: {gate.SYNC_ENABLE}.\n'
                f'    Current Disables: {gate.CACHE_DISABLE}.\n'
                'Objects: 1\n    [0] VkInstance 0x1234')
    return f'Validation {severity}: [ {vuid} ] | MessageID = 0x{message_id}\n{body}\n\n'


def loader_text():
    return ('WARNING | LAYER: Implicit layer "VK_LAYER_TEST_overlay" forced disabled because name matches filter '
            "of env var 'VK_LOADER_LAYERS_DISABLE'.\n"
            f'LAYER: Loading layer library {LIBRARY}\n'
            f'LAYER: Insert instance layer "{gate.LAYER}" ({LIBRARY})\n'
            f'LAYER: Inserted device layer "{gate.LAYER}" ({LIBRARY})\n')


DXVK_LOG = 'info: Enabled instance layers:\ninfo:   VK_LAYER_KHRONOS_validation\n'


def frozen(name):
    return dict(path=str(ARTIFACT/name), bytes=7, sha256='A'*64)


def baseline(kind='presenter'):
    return dict(ok=True, scope=('D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP' if kind == 'presenter'
            else 'R1_Q1_ACTUAL_X86_COM_RAW_TO_X64_DXVK'),
        sourceAndReferencePreserved=True, playerDllChanged=False,
        desktopSafety=dict(ok=True, inputBefore='Default'),
        cases=[dict(ok=True)], sources=[frozen('source.cpp')], binaries=[frozen('input.exe')],
        caseCopies=[dict(source=frozen('input.exe'), copy=frozen('case/input.exe'))],
        referenceBefore=frozen('reference.dll'), playerBefore=frozen('player.dll'),
        compileCommands=frozen('compile_commands.json'))


class ValidationReaderTests(unittest.TestCase):
    def test_malformed_loader_error_cannot_disappear(self):
        for bad in ('[Vulkan Loader] ERROR', '[Vulkan Loader] ERROR [X]: failed',
                    'ERROR [X]: failed', '[Vulkan Loader] malformed'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.check_loader(loader_text()+bad+'\n', DXVK_LOG, LIBRARY)

    def test_loader_request_text_is_not_execution_proof(self):
        text = loader_text().replace('LAYER: Loading', 'INFO: requested but not executed: Loading')
        with self.assertRaises(ValueError): gate.check_loader(text, DXVK_LOG, LIBRARY)
        for verb in ('Insert instance', 'Inserted device'):
            with self.subTest(verb=verb), self.assertRaises(ValueError):
                gate.check_loader(loader_text().replace('LAYER: '+verb, 'INFO: requested but not executed: '+verb), DXVK_LOG, LIBRARY)

    def test_indented_error_not_absorbed_by_activation(self):
        for padding in (' ', '  ', '\t'):
            text = vvl_record()+padding+'Validation Error: [ VUID-HIDDEN ] | MessageID = 0x01234567\nbad\n\n'
            with self.subTest(padding=padding), self.assertRaises(ValueError): gate.parse_validation(text)

    def test_private_empty_discovery_is_not_env_only(self):
        from render_lab_layer_source import NO_SETTINGS
        source = ARTIFACT/'private-empty'
        actual = '\n'.join(('INFO: '+NO_SETTINGS,
            'LAYER: Searching for implicit layer manifest files',
            'LAYER: In following locations:', 'LAYER: '+str(source), 'LAYER: Found no files'))+'\n'
        actual += 'INFO: Using Vulkan Loader C:/Windows/System32/vulkan-1.dll\nINFO: Vulkan Loader Version 1.4.341\n'
        layers = '\n'.join(loader_text().splitlines()[1:])+'\n'
        result = gate.check_loader(actual+layers, DXVK_LOG, LIBRARY, source, Path('C:/Windows/System32/vulkan-1.dll'))
        self.assertEqual(result['disabledImplicitLayerLines'], [])
        self.assertTrue(result['implicitDiscoverySource']['actualDiscoveryProven'])
        for bad in (layers, actual.replace(str(source),str(source)+'-other')+layers,
                    actual+layers+'ERROR: missing manifest\n',
                    actual+layers.replace('KHRONOS_validation','foreign')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.check_loader(bad, DXVK_LOG, LIBRARY, source, Path('C:/Windows/System32/vulkan-1.dll'))

    def test_private_source_environment_does_not_change_driver(self):
        source = ARTIFACT/'private-empty'
        env = gate.validation_env(ARTIFACT/'case', ARTIFACT/'VkLayer_khronos_validation.json', source)
        self.assertEqual(env['VK_IMPLICIT_LAYER_PATH'], str(source))
        self.assertEqual(env['VK_LOADER_LAYERS_DISABLE'], '~implicit~')
        self.assertEqual(env['VK_LOADER_DEBUG'], 'error,warn,info,layer')
        for name in ('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_ADD_IMPLICIT_LAYER_PATH'):
            self.assertNotIn(name,env)

    def test_actual_windows_callback_prefix(self):
        trace = '\n'.join('[Vulkan Loader] ' + line for line in loader_text().splitlines())
        result = gate.check_loader(trace, DXVK_LOG, LIBRARY)
        self.assertTrue(result['implicitLayersDisabledByProcessEnvironment'])
        for bad in ('ERROR: loader_get_json: Failed to open JSON file stale.json',
                    'WARNING | LAYER: an unclassified loader warning'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.check_loader(trace + '\n[Vulkan Loader] ' + bad, DXVK_LOG, LIBRARY)

    def test_information_activation_is_not_effective_filter_proof(self):
        result = gate.parse_validation(vvl_record())
        self.assertTrue(result['activationProven'])
        self.assertTrue(result['synchronizationValidationProven'])
        self.assertTrue(result['rawCallbackIndependentOfDxvkIgnoredIds'])
        self.assertFalse(result['runtimeFilterLimitProven'])
        self.assertEqual(result['nonStatusWarnings'], 0)

    def test_exact_status_warning_is_separately_classified(self):
        result = gate.parse_validation(vvl_record('Warning'))
        self.assertEqual(result['records'][0]['severity'], 'Warning')
        self.assertEqual(result['nonStatusWarnings'], 0)

    def test_crlf_and_app_name_are_preserved(self):
        result = gate.parse_validation(vvl_record().replace('\n', '\r\n'))
        self.assertIn('[AppName: warvk-d3d9-host-dev]', result['records'][0]['body'])

    def test_information_is_kept_not_discarded(self):
        result = gate.parse_validation(vvl_record() + vvl_record(vuid='INFORMATION-test', body='independent info'))
        self.assertEqual(len(result['records']), 2)
        self.assertEqual(result['records'][1]['body'], 'independent info')

    def test_every_nonstatus_warning_and_error_fails(self):
        for severity in ('Warning', 'Performance Warning', 'Error'):
            with self.subTest(severity=severity), self.assertRaises(ValueError):
                gate.parse_validation(vvl_record() + vvl_record(severity, 'VUID-test-00001', body='actual diagnostic'))

    def test_dxvk_ignored_ids_are_not_ignored_here(self):
        # Actual DXVK messenger ignore IDs; the independent file reader has no
        # matching suppression list. These are synthetic reader negatives.
        for message_id in ('23259a0d', '4b9d1597', '534c50ad', '9750b479', '11b37e31',
                           '151f5e5a', '6c16bfb4', 'd6d77e1e', 'a5625282'):
            with self.subTest(message_id=message_id), self.assertRaises(ValueError):
                gate.parse_validation(vvl_record() + vvl_record('Warning', 'VUID-test', message_id, 'not filtered'))

    def test_status_error_is_not_exempt(self):
        with self.assertRaises(ValueError): gate.parse_validation(vvl_record('Error'))

    def test_missing_and_duplicate_activation_fail(self):
        for text in (vvl_record(vuid='OTHER', body='not activation'), vvl_record()+vvl_record()):
            with self.subTest(text=text), self.assertRaises(ValueError): gate.parse_validation(text)

    def test_missing_or_changed_state_proof_fails(self):
        text = vvl_record()
        for before, after in ((gate.SYNC_ENABLE, 'None'),
                (gate.SYNC_ENABLE, gate.SYNC_ENABLE+', OTHER_ENABLE'),
                (gate.CACHE_DISABLE, gate.CACHE_DISABLE+', OTHER_DISABLE'),
                (gate.CACHE_DISABLE, 'VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT'),
                ('Khronos Validation Layer Active:', 'Validation Layer Installed:'),
                ('Current Enables:', 'Requested Enables:'),
                ('Current Disables:', 'Requested Disables:')):
            with self.subTest(before=before, after=after), self.assertRaises(ValueError):
                gate.parse_validation(text.replace(before, after))

    def test_duplicate_or_empty_state_tokens_fail(self):
        for tokens in (gate.SYNC_ENABLE+', '+gate.SYNC_ENABLE, gate.SYNC_ENABLE+', ', ', '+gate.SYNC_ENABLE):
            with self.subTest(tokens=tokens), self.assertRaises(ValueError):
                gate.parse_validation(vvl_record().replace(gate.SYNC_ENABLE, tokens))

    def test_duplicate_state_line_fails(self):
        text = vvl_record().replace('    Current Enables:', f'    Current Enables: {gate.SYNC_ENABLE}.\n    Current Enables:')
        with self.assertRaises(ValueError): gate.parse_validation(text)

    def test_empty_prefix_truncation_and_unrecognized_headers_fail(self):
        for text in ('', 'junk\n'+vvl_record(), vvl_record().rstrip('\n'),
                vvl_record()+'Validation Warning: invalid record\n',
                vvl_record()+'Validation Unexpected: invalid record\n',
                vvl_record()+'Verbose Information: unexpected active reporting\n',
                vvl_record()+'  Validation Error\n',
                vvl_record()+'Validation Error [ VUID-X ] | MessageID = 0x01234567\nbad\n',
                vvl_record()+'\tVerbose Information\n', vvl_record()+'Validation\n'):
            with self.subTest(text=text), self.assertRaises(ValueError): gate.parse_validation(text)

    def test_settings_failure_even_in_info_fails(self):
        for message in ('option not recognized', 'not a valid option', 'log could not be opened',
                'falling back to stdout', 'deprecated settings', 'LAYER SETTING logging error'):
            with self.subTest(message=message), self.assertRaises(ValueError):
                gate.parse_validation(vvl_record()+vvl_record(vuid='INFO-settings', body=message))


class LoaderReaderTests(unittest.TestCase):
    def test_exact_library_and_layer_chains_required(self):
        result = gate.check_loader(loader_text(), DXVK_LOG, LIBRARY)
        self.assertTrue(result['exactX64LayerLoaded'])
        self.assertTrue(result['implicitLayersDisabledByProcessEnvironment'])
        self.assertEqual(len(result['disabledImplicitLayerLines']), 1)

    def test_library_path_not_merely_layer_name(self):
        text = loader_text().replace(str(LIBRARY), str(ARTIFACT/'wrong.dll'))
        with self.assertRaises(ValueError): gate.check_loader(text, DXVK_LOG, LIBRARY)

    def test_extra_load_or_insert_is_not_accepted(self):
        for suffix in (f'LAYER: Loading layer library {ARTIFACT / "other.dll"}\n',
                f'LAYER: Insert instance layer "VK_LAYER_other" ({LIBRARY})\n',
                f'LAYER: Inserted device layer "VK_LAYER_other" ({LIBRARY})\n'):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                gate.check_loader(loader_text()+suffix, DXVK_LOG, LIBRARY)

    def test_any_missing_native_proof_fails(self):
        for token in ('forced disabled', 'Loading layer library', 'Insert instance layer', 'Inserted device layer'):
            text = '\n'.join(line for line in loader_text().splitlines() if token not in line)+'\n'
            with self.subTest(token=token), self.assertRaises(ValueError): gate.check_loader(text, DXVK_LOG, LIBRARY)

    def test_every_loader_severity_position_is_checked(self):
        for prefix in ('ERROR', 'WARNING', 'WARN', 'LAYER | ERROR', 'LAYER | WARNING', 'WARNING | LAYER'):
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                gate.check_loader(loader_text()+prefix+': unexpected problem\n', DXVK_LOG, LIBRARY)

    def test_disabled_substring_cannot_mask_another_warning(self):
        disabled = loader_text().splitlines()[0]
        text = loader_text() + disabled.replace(': Implicit', ': different failure; Implicit')+'\n'
        with self.assertRaises(ValueError): gate.check_loader(text, DXVK_LOG, LIBRARY)

    def test_dxvk_request_and_settings_errors_are_checked(self):
        for dxvk in ('info: '+gate.LAYER, 'info: Enabled instance layers:',
                     DXVK_LOG+'error: Failed to create debug messenger',
                     DXVK_LOG+'warn: Validation layers not found'):
            with self.subTest(dxvk=dxvk), self.assertRaises(ValueError): gate.check_loader(loader_text(), dxvk, LIBRARY)


class ConfigurationTests(unittest.TestCase):
    def test_raw_callback_is_nondefault_and_not_filtered(self):
        env = gate.validation_env(ARTIFACT/'case', ARTIFACT/'VkLayer_khronos_validation.json')
        self.assertEqual(env['VK_KHRONOS_VALIDATION_DEBUG_ACTION'], 'VK_DBG_LAYER_ACTION_LOG_MSG')
        self.assertNotIn('DEFAULT', env['VK_KHRONOS_VALIDATION_DEBUG_ACTION'])
        self.assertEqual(env['VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT'], 'false')
        self.assertEqual(env['VK_KHRONOS_VALIDATION_MESSAGE_ID_FILTER'], '')
        self.assertEqual(env['VK_KHRONOS_VALIDATION_REPORT_FLAGS'], 'info,warn,error,perf')
        self.assertEqual(env['VK_KHRONOS_VALIDATION_LOG_FILENAME'], str(ARTIFACT/'case/validation.raw.log'))

    def test_sync_enabled_without_gpu_assisted_or_descriptor_heuristic(self):
        env = gate.validation_env(ARTIFACT/'case', ARTIFACT/'VkLayer_khronos_validation.json')
        for name in ('VALIDATE_SYNC', 'SYNCVAL_SUBMIT_TIME_VALIDATION'):
            self.assertEqual(env['VK_KHRONOS_VALIDATION_'+name], 'true')
        for name in ('GPUAV_ENABLE', 'PRINTF_ENABLE', 'SYNCVAL_SHADER_ACCESSES_HEURISTIC'):
            self.assertEqual(env['VK_KHRONOS_VALIDATION_'+name], 'false')

    def test_process_local_layer_controls_and_environment_exclusions(self):
        env = gate.validation_env(ARTIFACT/'case', ARTIFACT/'VkLayer_khronos_validation.json')
        self.assertEqual(env['VK_LOADER_LAYERS_DISABLE'], '~implicit~')
        self.assertEqual(env['DXVK_DEBUG'], 'validation')
        self.assertEqual(env['VK_LAYER_PATH'], str(ARTIFACT))
        self.assertNotIn('VK_LAYER_ENABLES', env)
        self.assertNotIn('VK_LAYER_DISABLES', env)
        for key in ('VK_INSTANCE_LAYERS', 'VK_LAYER_SETTINGS_PATH', 'VK_ADD_LAYER_PATH',
                    'WARVK_D3D9_PROFILE', 'DSH_TOKEN', 'OPENAI_API_KEY'):
            self.assertNotIn(key, gate.ENV_ALLOW)

    def test_requested_setting_digest_order_independent_and_sensitive(self):
        first = gate.digest_json(gate.VVL_SETTINGS)
        self.assertEqual(first, gate.digest_json(dict(reversed(list(gate.VVL_SETTINGS.items())))))
        self.assertNotEqual(first, gate.digest_json(gate.VVL_SETTINGS | {'validate_sync': 'false'}))

    def test_five_case_command_scopes_do_not_mix_readback_modes(self):
        fixture, proxy = ARTIFACT/'fixture.exe', ARTIFACT/'proxy.dll'
        for label, kind, deadline in gate.CASE_SPECS:
            with self.subTest(label=label):
                args = gate.case_command(fixture, proxy, label, 'Isolated', 'Default')
                self.assertEqual(args[0], fixture)
                self.assertEqual(args.count('--dll'), 1)
                self.assertEqual(args[args.index('--dll')+1], proxy)
                if kind == 'lifetime':
                    self.assertEqual(args[args.index('--mode')+1], label)
                    self.assertNotIn('--wsi', args)
                    self.assertNotIn('--wsi-readback', args)
                elif label == 'clear-soak':
                    self.assertIn('--diagnostic-readback', args)
                    self.assertEqual(deadline, 45)
                else:
                    self.assertEqual('--wsi-readback' in args, label == 'presenter-readback-eight-frames')
                    self.assertEqual('--wsi' in args, label == 'presenter-eight-frames')
        with self.assertRaises(ValueError): gate.case_command(fixture, proxy, 'other', 'Isolated', 'Default')


class BaselineTests(unittest.TestCase):
    def load(self, value, kind='presenter'):
        with patch.object(Path, 'read_text', return_value=json.dumps(value)), \
                patch.object(gate, 'require_identity', side_effect=lambda row: Path(row['path'])), \
                patch.object(gate, 'verify_baseline_raw') as raw:
            result = gate.load_baseline(ARTIFACT/'receipt.json', kind)
            raw.assert_called_once_with(result, ARTIFACT/'receipt.json', kind)
            return result

    def test_passed_presenter_and_lifetime_receipts_accepted(self):
        for kind in ('presenter', 'lifetime'):
            with self.subTest(kind=kind): self.assertTrue(self.load(baseline(kind), kind)['ok'])

    def test_failed_receipt_fragments_not_promoted(self):
        for key, value in (('ok', False), ('ok', 1), ('sourceAndReferencePreserved', False),
                           ('playerDllChanged', True), ('scope', 'OFFSCREEN_ONLY')):
            row = baseline(); row[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): self.load(row)

    def test_failed_or_empty_case_and_unclosed_desktop_refused(self):
        for cases in ([], [dict(ok=False)], [dict(ok=True), dict(ok=False)], [dict(ok=1)]):
            row = baseline(); row['cases'] = cases
            with self.subTest(cases=cases), self.assertRaises(ValueError): self.load(row)
        row = baseline(); row['desktopSafety']['ok'] = False
        with self.assertRaises(ValueError): self.load(row)

    def test_missing_frozen_population_refused(self):
        for key in ('sources', 'binaries', 'caseCopies'):
            row = baseline(); row[key] = []
            with self.subTest(key=key), self.assertRaises(ValueError): self.load(row)

    def test_copy_not_equal_to_frozen_binary_refused(self):
        for key, value in (('bytes', 8), ('sha256', 'B'*64)):
            row = baseline(); row['caseCopies'][0]['copy'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.load(row)

    def test_unknown_kind_is_not_lifetime(self):
        with self.assertRaises(ValueError): self.load(baseline('lifetime'), 'unknown')

    def test_historical_passing_summary_without_initial_raw_manifest_refused(self):
        for kind in ('presenter', 'lifetime'):
            with self.subTest(kind=kind), patch.object(Path, 'read_text', return_value=json.dumps(baseline(kind))), \
                    patch.object(gate, 'require_identity', side_effect=lambda row: Path(row['path'])):
                with self.assertRaisesRegex(ValueError, 'not frozen and rechecked'):
                    gate.load_baseline(ARTIFACT/'receipt.json', kind)

    def test_duplicate_json_keys_refused(self):
        with patch.object(Path, 'read_text', return_value='{"ok":false,"ok":true}'):
            with self.assertRaises(ValueError): gate.load_baseline(ARTIFACT/'receipt.json', 'presenter')

    def test_changed_frozen_file_refused(self):
        record = frozen('test.exe')
        with patch.object(gate, 'identity', return_value=record | {'sha256': 'B'*64}):
            with self.assertRaises(ValueError): gate.require_identity(record)

    def test_frozen_identity_types_and_fields_are_exact(self):
        record = frozen('test.exe')
        with patch.object(gate, 'identity', return_value=record):
            self.assertEqual(gate.require_identity(record), Path(record['path']))
        for change in ({'bytes': True}, {'bytes': -1}, {'sha256': 'bad'}, {'sha256': 'a'*64},
                       {'path': 'relative.exe'}, {'extra': True}):
            row = record | change
            with self.subTest(change=change), patch.object(gate, 'identity', return_value=row):
                with self.assertRaises(ValueError): gate.require_identity(row)


class ProbeProvenanceTests(unittest.TestCase):
    def model(self):
        records = [frozen(f'desktop-probe-{bits}.exe') for bits in (32, 64)]
        return dict(desktopName='Isolated', desktopSafety=dict(inputBefore='Default'), binaries=records,
            desktopProbe=dict(argv=list(map(str, [Path(records[0]['path']), '--expect-desktop', 'Isolated',
                '--expect-station', 'WinSta0', '--input-desktop', 'Default', '--child', Path(records[1]['path']),
                '--child-log', ARTIFACT/'desktop-child.log'])), stdout=str(ARTIFACT/'probe.stdout'),
                stderr=str(ARTIFACT/'probe.stderr')),
            probeReports=[dict(bits=32), dict(bits=64)])

    def run_model(self, receipt, raw=None):
        raw = receipt['probeReports'] if raw is None else raw
        with patch.object(gate, 'check_launch'), patch.object(gate, 'validate_probe_receipt') as checked, \
                patch.object(gate, 'verify_raw_artifacts') as frozen_raw, \
                patch.object(gate, 'require_identity', side_effect=lambda row: Path(row['path'])), \
                patch.object(gate, 'pe_identity', side_effect=lambda path: (0x14c, 0x10b)
                             if path.name == 'desktop-probe-32.exe' else (0x8664, 0x20b)), \
                patch.object(gate, 'json_lines', side_effect=[[raw[0]], raw[1:]]):
            result = gate.baseline_probes(receipt, ARTIFACT/'receipt.json')
            checked.assert_called_once()
            frozen_raw.assert_called_once_with(receipt.get('probeRawArtifacts'), ARTIFACT,
                ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False)
            return result

    def test_both_frozen_probes_reuse_raw_strict_checker(self):
        probes = self.run_model(self.model())
        self.assertEqual(probes, {bits: ARTIFACT/f'desktop-probe-{bits}.exe' for bits in (32, 64)})

    def test_missing_or_duplicate_probe_refused(self):
        for count in (0, 2):
            row = self.model(); row['binaries'] = [row['binaries'][0]]*count + [row['binaries'][1]]
            with self.subTest(count=count), self.assertRaises(ValueError): self.run_model(row)

    def test_unfrozen_executed_probe_or_altered_command_refused(self):
        for index, value in ((0, str(ARTIFACT/'unfrozen.exe')), (4, 'OtherStation'), (8, str(ARTIFACT/'other64.exe'))):
            row = self.model(); row['desktopProbe']['argv'][index] = value
            with self.subTest(index=index), self.assertRaises(ValueError): self.run_model(row)

    def test_probe_cannot_escape_artifact_root(self):
        row = self.model(); row['binaries'][0]['path'] = str(ARTIFACT.parent/'desktop-probe-32.exe')
        with self.assertRaises(ValueError): self.run_model(row)

    def test_relabelled_raw_output_refused(self):
        row = self.model()
        with self.assertRaises(ValueError): self.run_model(row, [dict(bits=64), dict(bits=32)])
        row['desktopProbe']['stdout'] = str(ARTIFACT/'other.stdout')
        with self.assertRaises(ValueError): self.run_model(row)


class BaselineRawProvenanceTests(unittest.TestCase):
    def test_full_raw_baseline_requires_producer_manifest_and_stable_bytes(self):
        with tempfile.TemporaryDirectory(prefix='warvk-full-raw-reader-') as temporary:
            root = Path(temporary).resolve(); directory = root/'com'; directory.mkdir()
            groups = ((root, ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False),
                      (root, ['wire-32.stdout', 'wire-32.stderr'], False),
                      (directory, ['case.stdout', 'case.stderr', 'host.log'], True))
            manifests = []
            for folder, names, logs in groups:
                for name in names: (folder/name).write_bytes(b'{"ok":true}\n')
                manifests.append(gate.capture_raw_artifacts(folder, names, logs))
            case = dict(mode='com', process=dict(stdout=str(directory/'case.stdout')),
                        initialRawArtifacts=manifests[2])
            row = dict(rawArtifactsPreserved=True, probeRawArtifacts=manifests[0],
                       stepRawArtifacts=[manifests[1]], cases=[case])
            gate.verify_baseline_raw(row, root/'receipt.json', 'lifetime')
            for path in (root/'probe.stderr', root/'wire-32.stdout', directory/'host.log'):
                data = path.read_bytes(); path.write_bytes(data+b'ERROR: later diagnostic\n')
                with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, 'changed since launch return'):
                    gate.verify_baseline_raw(row, root/'receipt.json', 'lifetime')
                path.write_bytes(data)
            for key in ('probeRawArtifacts', 'stepRawArtifacts', 'cases'):
                changed = copy.deepcopy(row)
                if key == 'cases': del changed['cases'][0]['initialRawArtifacts']
                else: del changed[key]
                with self.subTest(key=key), self.assertRaises(ValueError):
                    gate.verify_baseline_raw(changed, root/'receipt.json', 'lifetime')


class CaseProvenanceTests(unittest.TestCase):
    def model(self, label='clear-soak', kind='presenter', root=None):
        directory = (root or ARTIFACT)/label
        name = ('d3d9-resource-lifetime32.exe' if kind == 'lifetime'
                else 'd3d9-presenter-fault32.exe' if label == 'clear-soak' else 'd3d9-api-parent32.exe')
        paths = (directory/name, directory/'proxy.dll', directory/'warvk-d3d9-host-dev.exe')
        case = dict(reports=[dict(fixture=True)], hostReports=[dict(host=True)], process=dict(
            argv=list(map(str, gate.case_command(paths[0], paths[1], label, 'Isolated', 'Default'))),
            stdout=str(directory/'case.stdout'), stderr=str(directory/'case.stderr')))
        case['mode' if kind == 'lifetime' else 'label'] = label
        return dict(cases=[case], desktopName='Isolated', desktopSafety=dict(inputBefore='Default'),
            caseCopies=[dict(copy=dict(path=str(path), bytes=7, sha256='A'*64)) for path in paths])

    def run_model(self, row, label='clear-soak', kind='presenter', raw=None):
        case = row['cases'][0]
        raw = [case['reports'], case['hostReports']] if raw is None else raw
        with patch.object(gate, 'verify_native') as verify, patch.object(gate, 'json_lines', side_effect=raw), \
                patch.object(gate, 'verify_raw_artifacts') as frozen_raw, \
                patch.object(gate, 'require_identity', side_effect=lambda record: Path(record['path'])), \
                patch.object(gate, 'pe_identity', side_effect=lambda path: (0x8664, 0x20b)
                             if path.name == 'warvk-d3d9-host-dev.exe' else (0x14c, 0x10b)):
            result = gate.baseline_case(row, ARTIFACT/'receipt.json', kind, label)
            verify.assert_called_once()
            frozen_raw.assert_called_once_with(case.get('initialRawArtifacts'), ARTIFACT/label,
                ['case.stdout', 'case.stderr', 'host.log'], True)
            return result

    def test_initial_manifest_binds_nonjson_raw_bytes_in_actual_case_reader(self):
        # Only native semantics/PE parsing are mocked. The production raw
        # manifest verifier and json_lines both read actual temporary files.
        with tempfile.TemporaryDirectory(prefix='warvk-baseline-reader-') as temporary:
            root = Path(temporary).resolve(); row = self.model(root=root)
            case = row['cases'][0]; directory = root/'clear-soak'; directory.mkdir()
            original = {'case.stdout': json.dumps(case['reports'][0]).encode()+b'\n',
                        'host.log': json.dumps(case['hostReports'][0]).encode()+b'\n', 'case.stderr': b''}
            for name, data in original.items(): (directory/name).write_bytes(data)
            case['initialRawArtifacts'] = gate.capture_raw_artifacts(directory, list(original), True)
            with patch.object(gate, 'verify_native'), \
                    patch.object(gate, 'require_identity', side_effect=lambda record: Path(record['path'])), \
                    patch.object(gate, 'pe_identity', side_effect=lambda path: (0x8664, 0x20b)
                                 if path.name == 'warvk-d3d9-host-dev.exe' else (0x14c, 0x10b)):
                gate.baseline_case(row, root/'receipt.json', 'presenter', 'clear-soak')
                for name, data in original.items():
                    (directory/name).write_bytes(data+b'ERROR: later non-JSON rewrite\n')
                    with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'changed since launch return'):
                        gate.baseline_case(row, root/'receipt.json', 'presenter', 'clear-soak')
                    (directory/name).write_bytes(data)
                del case['initialRawArtifacts']
                with self.assertRaisesRegex(ValueError, 'initial raw manifest required'):
                    gate.baseline_case(row, root/'receipt.json', 'presenter', 'clear-soak')

    def test_five_executed_cases_reuse_strict_native_readers(self):
        for label, kind, _ in gate.CASE_SPECS:
            with self.subTest(label=label):
                result = self.run_model(self.model(label, kind), label, kind)
                self.assertEqual(result[1:], (ARTIFACT/label/'proxy.dll', ARTIFACT/label/'warvk-d3d9-host-dev.exe'))

    def test_altered_command_and_redirection_rejected(self):
        row = self.model(); row['cases'][0]['process']['argv'][4] = 'different-scenario'
        with self.assertRaises(ValueError): self.run_model(row)
        row = self.model(); row['cases'][0]['process']['stderr'] = str(ARTIFACT/'other.stderr')
        with self.assertRaises(ValueError): self.run_model(row)

    def test_missing_duplicate_frozen_copy_and_relabelled_raw_rejected(self):
        for count in (0, 2):
            row = self.model(); row['caseCopies'] = [row['caseCopies'][0]]*count + row['caseCopies'][1:]
            with self.subTest(count=count), self.assertRaises(ValueError): self.run_model(row)
        with self.assertRaises(ValueError): self.run_model(self.model(), raw=[[dict(different=True)], [dict(host=True)]])

    def test_duplicate_case_cannot_pick_a_successful_fragment(self):
        row = self.model(); row['cases'].append(copy.deepcopy(row['cases'][0]))
        with self.assertRaises(ValueError): self.run_model(row)


if __name__ == '__main__':
    unittest.main(verbosity=2)
