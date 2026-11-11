"""Pure file counterexamples for native log freezing; no native/GPU launch."""
import copy
from pathlib import Path
import tempfile
import unittest

from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts


class RawArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='warvk-raw-reader-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.names = ['case.stdout', 'case.stderr', 'host.log']
        for name in self.names:
            (self.root/name).write_bytes(b'{"ok":true}\n' if name != 'case.stderr' else b'')
        (self.root/'host_dxvk.log').write_bytes(b'info: actual diagnostic\n')
        self.manifest = capture_raw_artifacts(self.root, self.names, True)

    def test_exact_initial_bytes_and_empty_stderr_accepted(self):
        records = verify_raw_artifacts(self.manifest, self.root, self.names, True)
        self.assertEqual(len(records), 4)
        self.assertEqual(next(row['bytes'] for row in records if Path(row['path']).name == 'case.stderr'), 0)

    def test_unchanged_json_cannot_hide_nonjson_rewrite(self):
        for name in self.names + ['host_dxvk.log']:
            with self.subTest(name=name):
                path = self.root/name; before = path.read_bytes()
                path.write_bytes(before+b'ERROR: appended after launch return\n')
                with self.assertRaisesRegex(ValueError, 'changed since launch return'):
                    verify_raw_artifacts(self.manifest)
                path.write_bytes(before)

    def test_late_log_cannot_escape_initial_population(self):
        (self.root/'late.log').write_bytes(b'late diagnostic\n')
        with self.assertRaisesRegex(ValueError, 'population changed'):
            verify_raw_artifacts(self.manifest)

    def test_failed_capture_retains_available_files_but_cannot_pass(self):
        (self.root/'host.log').unlink()
        manifest = capture_raw_artifacts(self.root, self.names, True)
        self.assertEqual(manifest['missingNames'], ['host.log'])
        self.assertEqual(len(manifest['files']), 3)
        with self.assertRaisesRegex(ValueError, 'initial raw artifacts missing'):
            verify_raw_artifacts(manifest)

    def test_missing_historical_or_late_manifest_rejected(self):
        for manifest in (None, [], {}, self.manifest | {'capturedAt': 'analysis-time'}):
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                verify_raw_artifacts(manifest)

    def test_missing_duplicate_or_unbound_file_refused(self):
        for operation in ('missing', 'duplicate', 'escape'):
            manifest = copy.deepcopy(self.manifest)
            if operation == 'missing': manifest['files'].pop(0)
            elif operation == 'duplicate': manifest['files'].append(copy.deepcopy(manifest['files'][0]))
            else: manifest['files'][0]['path'] = str(self.root.parent/'other.stdout')
            with self.subTest(operation=operation), self.assertRaises(ValueError):
                verify_raw_artifacts(manifest)

    def test_expected_case_policy_cannot_be_relabelled(self):
        for names, logs in ((['case.stdout'], True), (self.names, False)):
            with self.subTest(names=names, logs=logs), self.assertRaises(ValueError):
                verify_raw_artifacts(self.manifest, self.root, names, logs)

    def test_cpu_probe_freezes_only_its_explicit_outputs(self):
        manifest = capture_raw_artifacts(self.root, ['case.stdout', 'case.stderr'])
        (self.root/'later-build.log').write_bytes(b'unrelated later step\n')
        self.assertEqual(len(verify_raw_artifacts(manifest)), 2)


if __name__ == '__main__':
    unittest.main()
