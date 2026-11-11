"""Pure population/file/mocked-launch tests; no native, compiler, Desktop or GPU."""
import argparse
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_device_owned_regressions as gate
from test_isolated_render_desktop_contract import sample as probe_sample


def configured():
    return [dict(name='alpha'), dict(name='beta')]


def entries():
    return [dict(name='warvk:'+name, result='OK', returncode=0, duration=0.1, stdout='pass', stderr='')
            for name in ('alpha', 'beta')]


class PopulationTests(unittest.TestCase):
    def reject(self, definitions, results):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            gate.check_test_population(definitions, results)

    def test_nonempty_complete_population(self):
        results = entries()
        self.assertEqual(gate.check_test_population(configured(), results), results)
        self.assertEqual(len(gate.check_test_population(configured()[::-1], results)), 2)

    def test_empty_or_wrong_population_container_cannot_pass(self):
        for definitions, results in (([], []), ([], entries()), (configured(), []),
                ({}, entries()), (configured(), {}), (None, entries()), (configured(), None)):
            with self.subTest(definitions=definitions, results=results): self.reject(definitions, results)

    def test_missing_extra_duplicate_reordered_names(self):
        self.reject(configured(), entries()[:1])
        self.reject(configured()[:1], entries())
        self.reject(configured() + [dict(name='alpha')], entries())
        self.reject(configured(), entries() + [entries()[0]])
        self.reject([dict(name='alpha'), dict(name='alpha')], [entries()[0], entries()[0]])
        bad = entries(); bad[0]['name'] = 'warvk:other'; self.reject(configured(), bad)

    def test_names_are_nonempty_exact_strings(self):
        for value in (True, 1, None, [], '', ' alpha', 'alpha ', 'warvk:'):
            with self.subTest(value=value):
                bad = entries(); bad[0]['name'] = value; self.reject(configured(), bad)
        for value in (True, 1, None, [], '', ' alpha', 'alpha '):
            with self.subTest(configured_name=value):
                bad = configured(); bad[0]['name'] = value; self.reject(bad, entries())
        self.reject([None], entries()); self.reject(configured(), [None])

    def test_every_result_must_be_ok_integer_zero(self):
        for name, values in (('result', ('FAIL', 'SKIP', 'TIMEOUT', '', True, None)),
                ('returncode', (1, -1, True, False, 0.0, '0', None))):
            for value in values:
                bad = entries(); bad[0][name] = value
                with self.subTest(field=name, value=value): self.reject(configured(), bad)
            bad = entries(); del bad[0][name]; self.reject(configured(), bad)

    def test_optional_result_fields_cannot_change_type(self):
        for name, values in (('duration', (True, -1, float('inf'), float('nan'), '1')),
                ('stdout', (None, 1, [])), ('stderr', (None, 1, []))):
            for value in values:
                bad = entries(); bad[0][name] = value
                with self.subTest(field=name, value=value): self.reject(configured(), bad)

    def test_duplicate_json_keys_and_nonfinite_are_rejected(self):
        for raw in ('{"result":"FAIL","result":"OK"}', '{"returncode":1,"returncode":0}',
                '{"name":"alpha","name":"beta"}', '{"duration":NaN}'):
            with self.subTest(raw=raw), self.assertRaises(ValueError): gate.strict_json(raw)


class MockedRegressionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='warvk-offline-reader-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root/'build32_device_owner_mock'
        for directory in (self.build/'src/d3d9', self.build/'meson-info', self.build/'meson-logs',
                          self.root/'build32_weather_20261010/src/d3d9', self.root/'AutoTest/artifacts'):
            directory.mkdir(parents=True, exist_ok=True)
        for file in (self.build/'src/d3d9/d3d9.dll', self.root/'build32_weather_20261010/src/d3d9/d3d9.dll',
                     self.root/'probe32.exe', self.root/'probe64.exe', self.root/'source.cpp'):
            file.write_bytes(b'synthetic-only-never-executed')
        self.number = 0; self.trace = []; self.mode = ''; self.isolated = True
        self.definitions = configured(); self.result_text = '\n'.join(json.dumps(row) for row in entries())
        self.original_identity = gate.identity
        self.player = Path('E:/Work/Warcraft III/d3d9.dll')

    def identity(self, path):
        # No real player path is opened or changed by the pure test.
        if path == self.player: return dict(path=str(path), bytes=1, sha256='A'*64)
        return self.original_identity(path)

    def write_meson_logs(self, argv):
        base = argv[argv.index('--logbase')+1]
        self.assertTrue(str(base).startswith('warvk-owned-'))
        self.assertNotIn('/', str(base)); self.assertNotIn('\\', str(base))
        self.assertIn('--no-rebuild', argv); self.assertIn('-j2', argv)
        if self.mode == 'stale-default-only':
            (self.build/'meson-logs/testlog.json').write_text(self.result_text, encoding='utf-8')
            return
        for suffix, text in (('.json', self.result_text), ('.txt', 'synthetic complete log\n'),
                             ('.junit.xml', '<testsuites/>\n')):
            (self.build/'meson-logs'/(base+suffix)).write_text(text, encoding='utf-8')

    def fake_execute(self, argv, cwd, prefix, timeout, env):
        self.trace.append(('execute', prefix.name, list(argv), timeout))
        text = 'ninja: no work to do.' if prefix.name == 'no-work' else 'synthetic run\n'
        prefix.with_suffix('.stdout').write_text(text, encoding='utf-8')
        prefix.with_suffix('.stderr').write_bytes(b'')
        self.assertNotIn('SECRET_TEST_TOKEN', env)
        if prefix.name == 'build-tests' and self.mode == 'build-exception':
            raise RuntimeError('synthetic build exception')
        if prefix.name == 'meson-tests': self.write_meson_logs(argv)
        if prefix.name == 'no-work':
            if self.mode == 'raw-mutation':
                (prefix.parent/'probe.stderr').write_text('late error\n', encoding='utf-8')
            elif self.mode == 'raw-testlog-mutation':
                (prefix.parent/'testlog.txt').write_text('late raw rewrite', encoding='utf-8')
            elif self.mode == 'source-removed':
                (self.root/'source.cpp').unlink()
            elif self.mode == 'configured-drift':
                (self.build/'meson-info/intro-tests.json').write_text('[]', encoding='utf-8')
        return dict(command=list(map(str, argv)), exit=0, seconds=0.01)

    def desktop_factory(self, **kwargs):
        self.assertEqual(kwargs, dict(restricted_medium=True))
        owner = self

        class Desktop:
            def __init__(self):
                self.desktop_name = 'WarVK_ReaderOnly_Desktop'; self.input_before = 'Default'
                self.receipt = dict(ok=False, desktopCreated=True, desktopClosed=False, creatorExited=True,
                    restrictedMediumRequested=True, launches=[], cleanupErrors=[])

            def __enter__(self): return self

            def __exit__(self, kind, error, traceback):
                self.receipt['desktopClosed'] = True
                self.receipt['ok'] = error is None and all(row['ok'] for row in self.receipt['launches'])
                return False

            def run(self, argv, **kw):
                probe = '--child' in argv
                owner.trace.append(('desktop', 'probe' if probe else 'meson', list(argv), kw['timeout_s']))
                pid, tid = (101, 301) if probe else (201, 401)
                row = dict(ok=True, argv=list(map(str, argv)), actualExecutable=str(argv[0]),
                    jobActiveProcesses=0, jobAtomic=True, handleListApplied=True, processSettled=True,
                    jobEmpty=True, timedOut=False, forcedTermination=False, desktopHandleInherited=False,
                    pid=pid, creation100ns=10001, initialThreadId=tid, exitCode=0, cleanupErrors=[],
                    desktop=self.desktop_name, inputBefore='Default', inputAfter='Default',
                    restrictedToken=dict(schema=1, mode='restricted-medium-per-child/v1', ready=True,
                        parentTokenUnchanged=True, derivedTokenClosed=True, childTokenVerifiedBeforeResume=True,
                        noHighIntegrityFallback=True, resumedAfterVerification=True, resumePreviousCount=1,
                        globalSecurityChanged=False, cleanupErrors=[]))
                self.receipt['launches'].append(row)
                kw['stderr_path'].write_bytes(b'')
                if probe:
                    reports = [probe_sample(32, 101, 301), probe_sample(64, 102, 401)]
                    reports[0].update(childRequested=True, childDesktopExplicit=True,
                        childPid=102, childCreation=10001)
                    for report in reports:
                        for role in ('main', 'pump'): report[role]['desktop'] = self.desktop_name
                    kw['stdout_path'].write_text(json.dumps(reports[0]), encoding='utf-8')
                    Path(argv[argv.index('--child-log')+1]).write_text(json.dumps(reports[1]), encoding='utf-8')
                    if owner.mode == 'probe-fail': row.update(ok=False, exitCode=2)
                    if owner.mode == 'probe-owner-mismatch': row['initialThreadId'] += 1
                    if owner.mode == 'medium-fail': row['restrictedToken']['ready'] = False
                    if owner.mode == 'probe-exception': raise RuntimeError('synthetic probe exception')
                else:
                    owner.assertEqual(kw['timeout_s'], 300)
                    kw['stdout_path'].write_text('synthetic Meson process\n', encoding='utf-8')
                    owner.write_meson_logs(argv)
                    if owner.mode in ('meson-fail', 'failed-probe-change'):
                        row.update(ok=False, exitCode=1)
                        if owner.mode == 'failed-probe-change': (owner.root/'probe32.exe').write_bytes(b'changed')
                    if owner.mode == 'meson-timeout': row.update(ok=False, timedOut=True, forcedTermination=True, exitCode=2)
                    if owner.mode == 'meson-exception': raise RuntimeError('synthetic Meson exception')
                    if owner.mode == 'meson-medium-fail': row['restrictedToken']['childTokenVerifiedBeforeResume'] = False
                return row

        return Desktop()

    def run_gate(self, preexisting_log=False):
        self.number += 1
        output = self.root/'AutoTest/artifacts'/('reader-'+str(self.number))
        (self.build/'meson-info/intro-tests.json').write_text(json.dumps(self.definitions), encoding='utf-8')
        if preexisting_log:
            (self.build/'meson-logs'/('warvk-owned-'+output.name+'.json')).write_text(self.result_text, encoding='utf-8')
        args = argparse.Namespace(build=self.build, output=output, isolated_desktop=self.isolated,
            probe32=self.root/'probe32.exe' if self.isolated else None,
            probe64=self.root/'probe64.exe' if self.isolated else None)
        self.trace = []
        with patch.object(gate, 'ROOT', self.root), patch.object(gate, 'regression_sources', return_value=[self.root/'source.cpp']), \
                patch.object(gate, 'identity', side_effect=self.identity), patch.object(gate, 'execute', side_effect=self.fake_execute), \
                patch.object(gate, 'IsolatedRenderDesktop', side_effect=self.desktop_factory), \
                patch.object(argparse.ArgumentParser, 'parse_args', return_value=args), \
                patch.dict('os.environ', {'SECRET_TEST_TOKEN': 'must-not-enter-child-environment'}), \
                contextlib.redirect_stdout(io.StringIO()):
            code = gate.main()
        return code, gate.strict_json((output/'receipt.json').read_text('utf-8')), output

    def test_full_mock_path_probes_first_medium_desktop_frozen_raw(self):
        code, receipt, output = self.run_gate()
        self.assertEqual(code, 0); self.assertTrue(receipt['ok']); self.assertEqual(receipt['testCount'], 2)
        self.assertEqual([(kind, step) for kind, step, _, _ in self.trace],
            [('execute', 'build-tests'), ('desktop', 'probe'), ('desktop', 'meson'), ('execute', 'no-work')])
        self.assertTrue(receipt['probesPreserved']); self.assertTrue(receipt['sourcePreserved'])
        self.assertTrue(receipt['productAndPlayerPreserved']); self.assertTrue(receipt['rawArtifactsPreserved'])
        self.assertEqual(Path(receipt['testLog']['path']), output/'testlog.json')
        self.assertEqual(len(receipt['mesonLogCopies']), 3)
        for manifest in gate.raw_manifests(receipt): gate.verify_raw_artifacts(manifest)

    def test_empty_population_is_failed_not_zero_test_success(self):
        self.definitions = []; self.result_text = ''
        code, receipt, _ = self.run_gate()
        self.assertEqual(code, 1); self.assertFalse(receipt['ok'])
        self.assertIn('nonempty', receipt['error']); self.assertNotIn('testCount', receipt)

    def test_duplicate_result_json_cannot_overwrite_failure(self):
        self.definitions = [dict(name='alpha')]
        self.result_text = '{"name":"warvk:alpha","result":"FAIL","result":"OK","returncode":1,"returncode":0}'
        code, receipt, _ = self.run_gate()
        self.assertEqual(code, 1); self.assertIn('duplicate', receipt['error'])

    def test_stale_default_testlog_cannot_satisfy_unique_run(self):
        self.mode = 'stale-default-only'
        code, receipt, output = self.run_gate()
        self.assertEqual(code, 1); self.assertFalse(receipt['ok']); self.assertFalse((output/'testlog.json').exists())
        missing = [name for manifest in gate.raw_manifests(receipt) for name in manifest['missingNames']]
        self.assertIn('testlog.json', missing)

    def test_preexisting_unique_log_stops_before_any_desktop_or_test(self):
        code, receipt, _ = self.run_gate(preexisting_log=True)
        self.assertEqual(code, 1); self.assertIn('logbase already exists', receipt['error'])
        self.assertFalse(any(kind == 'desktop' for kind, _, _, _ in self.trace))

    def test_historical_copies_survive_later_mutable_build_logs(self):
        code, receipt, output = self.run_gate(); self.assertEqual(code, 0)
        for pair in receipt['mesonLogCopies']:
            Path(pair['source']['path']).write_text('future unrelated run\n', encoding='utf-8')
        (self.build/'meson-info/intro-tests.json').write_text('[]', encoding='utf-8')
        for manifest in gate.raw_manifests(receipt): gate.verify_raw_artifacts(manifest)
        self.assertEqual(gate.strict_json((output/'intro-tests.json').read_text('utf-8')), configured())

    def test_probe_failure_or_exception_never_reaches_meson(self):
        for mode in ('probe-fail', 'probe-owner-mismatch', 'probe-exception', 'medium-fail'):
            self.mode = mode
            with self.subTest(mode=mode):
                code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
                self.assertFalse(receipt['ok']); self.assertIn('probeRawArtifacts', receipt)
                self.assertTrue(receipt['probesPreserved'])
                self.assertFalse(any(kind == 'desktop' and name == 'meson' for kind, name, _, _ in self.trace))

    def test_failed_timeout_or_exception_meson_retains_raw_and_after_hashes(self):
        for mode in ('meson-fail', 'meson-timeout', 'meson-exception', 'meson-medium-fail'):
            self.mode = mode
            with self.subTest(mode=mode):
                code, receipt, output = self.run_gate(); self.assertEqual(code, 1)
                self.assertFalse(receipt['ok']); self.assertTrue(receipt['probesPreserved'])
                self.assertIn('probesAfter', receipt); self.assertTrue((output/'testlog.json').exists())
                self.assertEqual(len(receipt['mesonLogCopies']), 3)
                for manifest in gate.raw_manifests(receipt): gate.verify_raw_artifacts(manifest)

    def test_probe_mutation_on_failed_test_is_visible(self):
        self.mode = 'failed-probe-change'
        code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
        self.assertFalse(receipt['probesPreserved'])
        self.assertTrue(any('probes changed' in error for error in receipt['preservationErrors']))

    def test_initial_raw_mutation_cannot_rehash_itself_into_success(self):
        for mode in ('raw-mutation', 'raw-testlog-mutation'):
            self.mode = mode
            with self.subTest(mode=mode):
                code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
                self.assertFalse(receipt['rawArtifactsPreserved'])
                self.assertTrue(any('changed since launch return' in error for error in receipt['preservationErrors']))

    def test_final_source_read_failure_still_writes_failed_receipt(self):
        self.mode = 'source-removed'
        code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
        self.assertFalse(receipt['ok']); self.assertTrue(receipt['preservationErrors'])

    def test_configured_metadata_change_is_not_hidden_by_frozen_copy(self):
        self.mode = 'configured-drift'
        code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
        self.assertTrue(any('Meson source changed' in error for error in receipt['preservationErrors']))

    def test_build_failure_retains_partial_stdout_stderr(self):
        self.mode = 'build-exception'
        code, receipt, _ = self.run_gate(); self.assertEqual(code, 1)
        self.assertEqual(receipt['error'], 'synthetic build exception')
        self.assertEqual(len(receipt['stepRawArtifacts']), 1)
        self.assertEqual(len(receipt['stepRawArtifacts'][0]['files']), 2)

    def test_existing_nonisolated_mode_uses_same_strict_raw_gate(self):
        self.isolated = False
        code, receipt, _ = self.run_gate(); self.assertEqual(code, 0)
        self.assertFalse(receipt['isolatedDesktopRequested'])
        self.assertFalse(any(kind == 'desktop' for kind, _, _, _ in self.trace))
        self.assertTrue(receipt['rawArtifactsPreserved'])

    def test_create_new_raw_copy_refuses_existing_destination(self):
        original = self.root/'source.cpp'; destination = self.root/'existing.raw'
        destination.write_bytes(b'preserve-existing')
        with self.assertRaises(FileExistsError): gate.copy_raw_exact(original, destination)
        self.assertEqual(destination.read_bytes(), b'preserve-existing')


if __name__ == '__main__':
    unittest.main()
