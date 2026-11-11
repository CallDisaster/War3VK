"""Pure VVL 1.4.365 controls/parser tests. No native load, launch or build."""
import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/run_render_host_validation_gate.py').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_lab_vvl365 as m

EXPECTED_CLASSES = ('Core Checks', 'Synchronization', 'Stateless Parameter',
                    'Object lifetime', 'Thread Safety', 'Handle Wrapping')


def clean():
    return ('Validation Information: [ CURRENT-VALIDATION-ENABLED ] | MessageID = 0xfd7b2292\n'
        '[AppName: warvk-d3d9-host-dev.exe] vkCreateInstance(): Current Validation Enabled:\n'
        + ''.join('  - '+name+'\n' for name in EXPECTED_CLASSES)
        + '\nObjects: 1\n    [0] VkInstance 0x27a4eaa3db0\n\n')


def message(severity, vuid, body='independent raw evidence'):
    return f'Validation {severity}: [ {vuid} ] | MessageID = 0x1\n{body}\n\n'


def manifest():
    return dict(file_format_version='1.2.0', layer=dict(name=m.previous.LAYER,
        api_version='1.4.365', type='GLOBAL', library_path='.\\VkLayer_khronos_validation.dll',
        features=dict(settings=[dict(key=k) for k in sorted(set(m.SETTINGS)|{'log_filename'})])))


class ControlsTests(unittest.TestCase):
    def test_only_documented_setting_changes(self):
        old=copy.deepcopy(m.previous.VVL_SETTINGS)
        expected=copy.deepcopy(old)
        expected['syncval_full_validation']=expected.pop('syncval_submit_time_validation')
        expected['syncval_record_time_validation']='true'
        self.assertEqual(m.SETTINGS,expected)
        self.assertIsNot(m.SETTINGS,m.previous.VVL_SETTINGS)
        self.assertEqual(m.previous.VVL_SETTINGS,old)

    def test_environment_only_changes_two_keys_and_removes_deprecated_alias(self):
        case=Path('C:/pure/case'); layer=Path('C:/pure/layer/VkLayer_khronos_validation.json')
        for empty in (None,Path('C:/pure/empty')):
            with self.subTest(empty=empty):
                old=m.previous.validation_env(case,layer,empty)
                expected=dict(old)
                del expected['VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION']
                expected.update(VK_KHRONOS_VALIDATION_SYNCVAL_FULL_VALIDATION='true',
                    VK_KHRONOS_VALIDATION_SYNCVAL_RECORD_TIME_VALIDATION='true')
                self.assertEqual(m.validation_env(case,layer,empty),expected)
                self.assertEqual(m.previous.validation_env(case,layer,empty),old)

    def test_no_warning_filters_limits_or_threading_exemptions(self):
        self.assertEqual(m.SETTINGS['message_id_filter'],'')
        self.assertEqual(m.SETTINGS['enable_message_limit'],'false')
        self.assertEqual(m.SETTINGS['thread_safety'],'true')
        self.assertEqual(m.SETTINGS['validate_sync'],'true')
        self.assertEqual(m.SETTINGS['report_flags'],'info,warn,error,perf')
        self.assertEqual(m.SETTINGS['debug_action'],'VK_DBG_LAYER_ACTION_LOG_MSG')

    def test_manifest_complete_and_version_exact(self):
        m.check_manifest(manifest())
        for key,value in (('api_version','1.4.335'),('name','VK_LAYER_other'),
            ('type','INSTANCE'),('library_path','../other.dll')):
            doc=manifest();doc['layer'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):m.check_manifest(doc)

    def test_every_required_control_is_required(self):
        for key in set(m.SETTINGS)|{'log_filename'}:
            doc=manifest()
            doc['layer']['features']['settings']=[x for x in doc['layer']['features']['settings'] if x['key']!=key]
            with self.subTest(key=key),self.assertRaises(ValueError):m.check_manifest(doc)

    def test_actual_frozen_manifest_supports_new_recipe(self):
        path=ROOT/'AutoTest/artifacts/vvl-official-edeeee0-20261022/unpacked/VkLayer_khronos_validation.json'
        m.check_manifest(json.loads(path.read_text('utf-8')))


class ParserTests(unittest.TestCase):
    def test_exact_banner_does_not_prove_all_settings_or_runtime_filters(self):
        row=m.parse_validation(clean())
        self.assertTrue(row['activationProven'])
        self.assertTrue(row['synchronizationValidationProven'])
        self.assertEqual(row['enabledValidationClasses'],list(EXPECTED_CLASSES))
        self.assertFalse(row['runtimeFilterLimitProven'])
        self.assertFalse(row['runtimeAllSettingsProven'])
        self.assertEqual(row['nonStatusWarnings'],0)

    def test_crlf_supported(self):
        self.assertTrue(m.parse_validation(clean().replace('\n','\r\n'))['activationProven'])

    def test_each_missing_duplicated_or_extra_class_rejected(self):
        for name in m.ENABLED_CLASSES:
            line='  - '+name+'\n'
            for replacement in ('',line+line,line+'  - GPU-AV\n'):
                with self.subTest(name=name,replacement=replacement),self.assertRaises(ValueError):
                    m.parse_validation(clean().replace(line,replacement))

    def test_renamed_reordered_or_nonexact_class_rejected(self):
        for before,after in (('Object lifetime','Object Lifetime'),
            ('  - Thread Safety','  - Thread Safety disabled'),
            ('  - Core Checks\n  - Synchronization\n','  - Synchronization\n  - Core Checks\n')):
            with self.subTest(after=after),self.assertRaises(ValueError):
                m.parse_validation(clean().replace(before,after))

    def test_warning_error_and_performance_all_fail_including_settings_gpa_threading(self):
        for severity in ('Warning','Error','Performance Warning'):
            for vuid in ('VALIDATION-SETTINGS','WARNING-vkGetDeviceProcAddr-device',
                'UNASSIGNED-Threading-MultipleThreads-Write',
                'UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout','arbitrary-new-id'):
                with self.subTest(severity=severity,vuid=vuid),self.assertRaisesRegex(ValueError,vuid):
                    m.parse_validation(clean()+message(severity,vuid))

    def test_status_is_information_only(self):
        for severity in ('Warning','Error','Performance Warning'):
            with self.subTest(severity=severity),self.assertRaises(ValueError):
                m.parse_validation(clean().replace('Validation Information:','Validation '+severity+':'))

    def test_missing_duplicate_wrong_id_and_message_id_fail(self):
        for text in ('',clean()+clean(),clean().replace(m.STATUS_ID,'WARNING-CreateInstance-status-message'),
                     clean().replace('0xfd7b2292','0x1')):
            with self.subTest(text=text[:70]),self.assertRaises(ValueError):m.parse_validation(text)

    def test_truncated_or_malformed_boundaries_rejected(self):
        for text in (clean().rstrip('\n'),'prefix\n'+clean(),clean()+'Validation Debug: unknown\n',
            clean()+'Validation\n',clean()+'Verbose Information: unknown\n'):
            with self.subTest(text=text[-70:]),self.assertRaises(ValueError):m.parse_validation(text)

    def test_full_banner_body_including_instance_required(self):
        for before,after in (('Objects: 1','Objects: 0'),('VkInstance 0x27a4eaa3db0','VkInstance 0x0'),
            ('    [0] VkInstance 0x27a4eaa3db0',''),('Current Validation Enabled:','enabled:'),
            ('\n\nObjects:', '\n  - unrecognized\n\nObjects:')):
            with self.subTest(after=after),self.assertRaises(ValueError):
                m.parse_validation(clean().replace(before,after))
        with self.assertRaises(ValueError):m.parse_validation(clean()+'unexpected suffix\n')

    def test_informational_settings_failures_are_not_hidden(self):
        for body in ('setting not recognized','deprecated setting','could not be opened',
                     'falling back to stdout','LAYER SETTING has an error'):
            with self.subTest(body=body),self.assertRaises(ValueError):
                m.parse_validation(clean()+message('Information','info',body))

    def test_old_reader_contract_is_not_rewritten(self):
        old=('Validation Information: [ WARNING-CreateInstance-status-message ] | MessageID = 0x23dfd876\n'
             'Khronos Validation Layer Active:\n'
             '    Current Enables: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT.\n'
             '    Current Disables: VK_VALIDATION_FEATURE_DISABLE_SHADER_VALIDATION_CACHING_EXT.\n\n')
        self.assertTrue(m.previous.parse_validation(old)['activationProven'])
        with self.assertRaises(ValueError):m.parse_validation(old)
        with self.assertRaises(ValueError):m.parse_validation(clean()+old)
        with self.assertRaises(ValueError):m.previous.parse_validation(clean())

    def test_actual_r1_remains_failed_with_both_original_warnings(self):
        path=ROOT/'AutoTest/artifacts/x64-dispatch-vvl-control-20261023-r1/02-soak-clear-soak-host/validation.raw.log'
        before=path.read_bytes();text=before.decode('utf-8')
        with self.assertRaises(ValueError) as caught:m.parse_validation(text)
        self.assertIn('VALIDATION-SETTINGS',str(caught.exception))
        self.assertIn('WARNING-vkGetDeviceProcAddr-device',str(caught.exception))
        self.assertEqual(path.read_bytes(),before)


if __name__=='__main__':
    unittest.main(verbosity=2)
