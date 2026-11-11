"""Pure candidate-VVL contract tests; never compile, launch native, or load DLLs.

All build/layer files are in-memory mock fixtures, not historical artifacts.
The imported formal runner and its strict VVL/environment readers are real.
Move this file to AutoTest unchanged; no draft-directory import is required.
"""
import ast
from contextlib import ExitStack, contextmanager
import copy
from dataclasses import replace
import hashlib
import inspect
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/diagnose_surface_owner_candidate_vvl.py').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))
import diagnose_surface_owner_candidate_vvl as m
from render_lab_layer_source import NO_SETTINGS


def file_row(path):
    return dict(path=str(path), bytes=17,
                sha256=hashlib.sha256(str(path).encode()).hexdigest().upper())


def manifest_document():
    return dict(file_format_version='1.2.0', layer=dict(name=m.validation.LAYER,
        api_version='1.4.365', type='GLOBAL', library_path='.\\VkLayer_khronos_validation.dll',
        features=dict(settings=[dict(key=k) for k in sorted(set(m.validation.VVL_SETTINGS)|{'log_filename'})])))


class BuildModel:
    def __init__(self):
        self.build = ROOT/'build64_render_host_pure_fixture'
        self.path = ROOT/'AutoTest/artifacts/pure-fixture-never-created/receipt.json'
        self.sources = [ROOT/'src/vulkan/vulkan_loader.h', ROOT/'tools/render_host/dxvk_core/d3d9_backend.cpp']
        self.allowed = sorted(str(Path(p)) for p in
            ('src/d3d9/d3d9_options.cpp', 'src/d3d9/d3d9_shader_helpers.cpp'))
        paths = self.sources + list(m.PROTECTED) + [self.path, self.build/m.TARGET,
            self.build/'compile_commands.json', self.build/'build.ninja']
        self.identities = {str(p): file_row(p) for p in paths}
        opts = ['-D'+k+'=false' for k in ('enable_dxgi','enable_d3d8','enable_d3d9','enable_d3d10','enable_d3d11')]
        commands = [list(map(str, [Path(sys.executable), '-m', 'mesonbuild.mesonmain', 'setup',
            self.build, ROOT, '--buildtype=release', '--cross-file', ROOT/'build-win64-render-host-llvm.txt',
            '-Dwarvk_render_host_core_dev=true', *opts])),
            list(map(str, ['ninja', '-C', self.build, '-j2', m.TARGET.as_posix()])),
            list(map(str, ['ninja', '-C', self.build, '-n', m.TARGET.as_posix()]))]
        self.raw = []
        for name in ('configure','build','no-work'):
            names = sorted([name+'.stdout', name+'.stderr'])
            files = [file_row(self.path.parent/n) for n in names]
            self.identities.update((r['path'], r) for r in files)
            self.raw.append(dict(schema='render-lab-initial-raw/v1', capturedAt='launch-return',
                directory=str(self.path.parent), requiredNames=names, includeLogs=False,
                files=files, missingNames=[]))
        self.record = dict(schema='warvk-x64-candidate-build/v1', ok=True, nativeExecuted=False,
            productIntegrated=False, releaseReady=False, priority='BelowNormal', parallelism=2,
            build=str(self.build), sourcesPreserved=True, protectedUnchanged=True,
            sources=[self.identity(p) for p in self.sources],
            protectedBefore=[self.identity(p) for p in m.PROTECTED],
            protectedAfter=[self.identity(p) for p in m.PROTECTED], allowedSharedD3D9Inputs=self.allowed,
            host=self.identity(self.build/m.TARGET), compileCommands=self.identity(self.build/'compile_commands.json'),
            buildNinja=self.identity(self.build/'build.ninja'),
            steps=[dict(command=c, exit=0, seconds=1.0) for c in commands],
            stepRawArtifacts=copy.deepcopy(self.raw))

    def identity(self, path):
        return copy.deepcopy(self.identities[str(path)])

    def same_file(self, row):
        m.require(row == self.identity(Path(row['path'])), 'mock file bytes drifted')
        return row

    def verify(self, raw, directory=None, required_names=None, include_logs=None):
        # This models file access only. The runner must request the exact
        # production raw-reader directory/names/policy, not merely loop a flag.
        m.require(raw in self.raw, 'changed/missing original raw manifest')
        if directory is not None:
            m.require(directory == self.path.parent, 'wrong raw directory')
        if required_names is not None:
            m.require(sorted(required_names) == raw['requiredNames'], 'wrong raw names')
        if include_logs is not None:
            m.require(include_logs is False, 'wrong raw discovery policy')
        return copy.deepcopy(raw['files'])

    @contextmanager
    def active(self, record=None, bitness=(0x8664, 0x20b), no_work='ninja: no work to do.\n'):
        record = self.record if record is None else record
        def read(path, *args, **kwargs):
            if path == self.path: return json.dumps(record)
            if path == self.path.parent/'no-work.stdout': return no_work
            raise AssertionError('unmodelled file read: '+str(path))
        with ExitStack() as stack:
            stack.enter_context(patch.object(Path, 'resolve', lambda p, *a, **k: p))
            stack.enter_context(patch.object(Path, 'read_text', read))
            stack.enter_context(patch.object(m, 'identity', side_effect=self.identity))
            stack.enter_context(patch.object(m, 'pe_identity', return_value=bitness))
            stack.enter_context(patch.object(m, 'build_sources', return_value=self.sources))
            stack.enter_context(patch.object(m, 'check_host_compile_boundary', return_value=self.allowed))
            stack.enter_context(patch.object(m, 'verify_raw_artifacts', side_effect=self.verify))
            stack.enter_context(patch.object(m.init.copied, 'same_file', side_effect=self.same_file))
            yield


class CandidateBuildTests(unittest.TestCase):
    def rejected(self, change, **kwargs):
        model = BuildModel(); record = copy.deepcopy(model.record); change(record)
        with model.active(record, **kwargs), self.assertRaises((ValueError, KeyError, TypeError)):
            m.bind_build(model.path, [])

    def test_complete_mock_build_binds_all_inputs(self):
        model = BuildModel(); before = copy.deepcopy(model.record); frozen=[]
        with model.active():
            record, build = m.bind_build(model.path, frozen)
        self.assertEqual(record, before); self.assertEqual(build, model.build)
        self.assertEqual(model.record, before)
        self.assertEqual({r['path'] for r in frozen}, set(model.identities))

    def test_critical_missing_build_evidence_rejected(self):
        for key in ('steps','stepRawArtifacts','protectedAfter','allowedSharedD3D9Inputs'):
            with self.subTest(key=key): self.rejected(lambda r: r.pop(key))

    def test_critical_empty_protected_before_rejected(self):
        self.rejected(lambda r: r.update(protectedBefore=[]))

    def test_critical_failed_build_step_rejected(self):
        self.rejected(lambda r: r['steps'][1].update(exit=7))

    def test_critical_foreign_x86_host_rejected(self):
        self.rejected(lambda r: r.update(host=file_row(ROOT/'AutoTest/foreign-parent32.exe')))
        self.rejected(lambda r: None, bitness=(0x14c, 0x10b))
        self.rejected(lambda r: None, bitness=(0x8664, 0x10b))

    def test_incomplete_wrong_version_and_claims_rejected(self):
        changes = [('schema','warvk-x64-candidate-build/v0'),('ok',1),('ok',False),
            ('sourcesPreserved',False),('protectedUnchanged',False),('nativeExecuted',True),
            ('productIntegrated',True),('releaseReady',True),('priority','Normal'),('parallelism',True)]
        for key,value in changes:
            with self.subTest(key=key,value=value): self.rejected(lambda r: r.update({key:value}))

    def test_source_digest_and_population_drift_rejected(self):
        self.rejected(lambda r: r['sources'][0].update(sha256='0'*64))
        self.rejected(lambda r: r.update(sources=[]))
        self.rejected(lambda r: r['sources'].append(copy.deepcopy(r['sources'][0])))

    def test_original_three_commands_and_raw_manifest_population_required(self):
        self.rejected(lambda r: r['steps'].pop())
        self.rejected(lambda r: r['stepRawArtifacts'].pop())
        self.rejected(lambda r: r['steps'][1]['command'].__setitem__(3,'-j8'))
        self.rejected(lambda r: r['stepRawArtifacts'][0].update(capturedAt='after-verdict'))
        self.rejected(lambda r: r['stepRawArtifacts'][0].update(missingNames=['configure.stdout']))
        self.rejected(lambda r: None, no_work='work remains\n')

    def test_compile_boundary_and_target_identity_required(self):
        self.rejected(lambda r: r.update(allowedSharedD3D9Inputs=[]))
        self.rejected(lambda r: r['buildNinja'].update(path=str(ROOT/'build.ninja')))
        self.rejected(lambda r: r['compileCommands'].update(sha256='0'*64))


class CandidateLayerTests(unittest.TestCase):
    def test_exact_supported_manifest(self):
        m.check_layer_manifest(manifest_document())

    def test_version_bitness_and_library_route_not_relaxed(self):
        for key, values in (('api_version',('1.4.335','1.4.365 ',None)),
            ('name',('VK_LAYER_other',)),('type',('INSTANCE',)),
            ('library_path',('../other.dll','.\\other.dll','VkLayer_khronos_validation.dll'))):
            for value in values:
                doc=manifest_document(); doc['layer'][key]=value
                with self.subTest(key=key,value=value), self.assertRaises((ValueError,TypeError)):
                    m.check_layer_manifest(doc)

    def test_every_required_manifest_control_is_required(self):
        for key in set(m.validation.VVL_SETTINGS)|{'log_filename'}:
            doc=manifest_document()
            doc['layer']['features']['settings']=[x for x in doc['layer']['features']['settings'] if x['key']!=key]
            with self.subTest(key=key), self.assertRaises(ValueError): m.check_layer_manifest(doc)

    def test_layer_pins_and_both_x64_checks(self):
        manifest=m.LAYER_ROOT/'unpacked/VkLayer_khronos_validation.json'
        library=manifest.with_suffix('.dll'); loader=ROOT/'mock-system/System32/vulkan-1.dll'
        rows={str(m.LAYER_ROOT/p):dict(path=str(m.LAYER_ROOT/p),bytes=size,sha256=digest)
              for p,(size,digest) in m.LAYER_PINS.items()}
        rows[str(loader)]=file_row(loader)
        def run(*, bad_path=None, bits=(0x8664,0x20b), bad_pin=None):
            changed=copy.deepcopy(rows)
            if bad_pin: changed[str(m.LAYER_ROOT/bad_pin)]['sha256']='0'*64
            with ExitStack() as stack:
                stack.enter_context(patch.object(Path,'resolve',lambda p,*a,**k:p))
                stack.enter_context(patch.object(Path,'read_text',lambda p,*a,**k:json.dumps(manifest_document())))
                stack.enter_context(patch.object(m,'os',SimpleNamespace(environ={'SYSTEMROOT':str(ROOT/'mock-system')})))
                stack.enter_context(patch.object(m,'identity',side_effect=lambda p:copy.deepcopy(changed[str(p)])))
                stack.enter_context(patch.object(m,'pe_identity',side_effect=lambda p:bits if p==bad_path else (0x8664,0x20b)))
                frozen=[]; result=m.bind_layer(frozen)
                self.assertEqual(result,(manifest,library,loader)); self.assertEqual(len(frozen),4)
        run()
        for target in (library,loader):
            for bits in ((0x14c,0x10b),(0x8664,0x10b)):
                with self.subTest(target=target,bits=bits),self.assertRaises(ValueError):run(bad_path=target,bits=bits)
        for pin in m.LAYER_PINS:
            with self.subTest(pin=pin),self.assertRaises(ValueError):run(bad_pin=pin)


def clean_vvl():
    return ('Validation Information: [ WARNING-CreateInstance-status-message ] | MessageID = 0x23dfd876\n'
        'Khronos Validation Layer Active:\n'
        '    Current Enables: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT.\n'
        '    Current Disables: VK_VALIDATION_FEATURE_DISABLE_SHADER_VALIDATION_CACHING_EXT.\n\n')


class SharedReaderTests(unittest.TestCase):
    def parse(self, extra=''):
        directory=ROOT/'pure-never-created'; library=directory/'VkLayer_khronos_validation.dll'
        source=directory/'empty'; loader=Path('C:/Windows/System32/vulkan-1.dll')
        host='\n'.join(('INFO: '+NO_SETTINGS,'INFO: Using Vulkan Loader '+str(loader),
            'INFO: Vulkan Loader Version 1.4.341','LAYER: Searching for implicit layer manifest files',
            'LAYER: In following locations:','LAYER: '+str(source),'LAYER: Found no files',
            'LAYER: Loading layer library '+str(library),
            f'LAYER: Insert instance layer "{m.validation.LAYER}" ({library})',
            f'LAYER: Inserted device layer "{m.validation.LAYER}" ({library})'))+'\n'
        raw=clean_vvl()+extra
        with patch.object(m.validation,'parse_validation',wraps=m.validation.parse_validation) as reader:
            result=m.b2.parse_evidence(host,'info: Enabled instance layers:\ninfo:   VK_LAYER_KHRONOS_validation\n',
                raw,library,source,loader)
            reader.assert_called_once_with(raw)
        return result

    def test_shared_strict_reader_clean_does_not_grant_acceptance(self):
        result=self.parse(); self.assertTrue(result['checksPassed'])
        self.assertFalse(result['acceptancePassed']);self.assertFalse(result['completeC2AcceptancePassed'])
        self.assertFalse(result['runtimeFilterLimitProven'])

    def test_named_warning_threading_and_ignored_ids_never_filtered(self):
        for severity,vuid in (('Warning','WARNING-vkGetDeviceProcAddr-device'),
            ('Error','UNASSIGNED-Threading-MultipleThreads-Write'),
            ('Warning','UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout'),
            ('Performance Warning','arbitrary-performance-warning')):
            raw=f'Validation {severity}: [ {vuid} ] | MessageID = 0x1\nindependent raw evidence\n\n'
            with self.subTest(vuid=vuid):
                result=self.parse(raw);self.assertFalse(result['checksPassed'])
                self.assertTrue(any(vuid in error for error in result['errors']))

    def test_corrected_layer_still_uses_exact_controls_and_deadlines(self):
        cases=m.b2.CASES
        self.assertEqual([(c.index,c.kind,c.mode,c.seconds) for c in cases],
                         [(2,'soak','host',45),(12,'shader','host',90)])
        with self.assertRaises(ValueError):m.b2.checked_case(replace(cases[0],seconds=90))
        directory=ROOT/'pure-never-created';manifest=directory/'VkLayer_khronos_validation.json';source=directory/'empty'
        controls=m.b2.controls_for(cases[0],directory,manifest,source)
        self.assertEqual(controls,m.validation.validation_env(directory,manifest,source))
        for key,value in (('VK_DRIVER_FILES','redirect.json'),('VK_INSTANCE_LAYERS','other'),
            ('VK_KHRONOS_VALIDATION_MESSAGE_ID_FILTER','WARNING-vkGetDeviceProcAddr-device'),
            ('VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT','true'),('VK_KHRONOS_VALIDATION_THREAD_SAFETY','false')):
            with self.subTest(key=key),self.assertRaises(ValueError):
                m.b2.check_controls(controls|{key:value},directory,manifest,source)

    def test_main_reuses_guarded_native_and_copy_tu_not_custom_parser(self):
        tree=ast.parse(inspect.getsource(m.main))
        calls=[n for n in ast.walk(tree) if isinstance(n,ast.Call)]
        names=[ast.unparse(n.func) for n in calls]
        self.assertEqual(names.count('b2.run_native'),1)
        self.assertEqual(names.count('init.build_diagnostic'),1)
        self.assertEqual(names.count('preserve_failure_and_deadlines'),1)
        self.assertEqual(names.count('surface.instrument_source'),1)
        self.assertNotIn('validation.parse_validation',names)
        self.assertEqual(names.count('target.write_text'),1)
        for name in ('b2.run_native','init.build_diagnostic'):
            guarded=[n for n in ast.walk(tree) if isinstance(n,ast.If)
                     and ast.unparse(n.test)=='args.execute_native'
                     and any(isinstance(c,ast.Call) and ast.unparse(c.func)==name for c in ast.walk(n))]
            self.assertEqual(len(guarded),1,name)


if __name__=='__main__':
    unittest.main(verbosity=2)
