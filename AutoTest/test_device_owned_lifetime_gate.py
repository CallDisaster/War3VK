"""Pure reader/command-plan negatives, never compiler, GPU, DLL or game proof.

All native-looking records in this module are deliberately synthetic inputs
for the production receipt checker. Only the separate C++ execution may supply
COM, GPU readback, or normal-teardown acceptance evidence.
"""

import copy
import json
import unittest

import run_device_owned_lifetime_gate as gate


def synthetic_transcript(cycles=3, enabled=True):
    """Independent scalar fixture-shaped input; never runtime evidence."""
    lines = []

    def stage(name):
        lines.append('DEVICE_OWNER_STAGE stage=' + name)

    def probe(operation, secondary=False, ended=False, pending_atlas=False):
        values = [0] * 39
        values[0], values[1] = 1, operation
        if not ended:
            values[2] = values[3] = values[4] = values[6] = 1
            values[5] = int(not secondary)
            values[10], values[11] = (0, 0) if pending_atlas else (1, 64)
            values[18] = int(enabled)
            values[36] = int(operation == 9)
            if operation in (1, 2, 3, 7):
                values[7], values[8], values[9] = 12, 18, 1
                values[12] = values[13] = 1
            if operation in (2, 3, 7):
                extent, color = {2: (8, 0xff80c040), 3: (17, 0xff40a0e0), 7: (8, 0xffe05060)}[operation]
                values[19] = values[20] = extent
                values[23], values[24], values[26] = 1, extent * extent, color
            if operation in (4, 6):
                values[12], values[13], values[14], values[15] = 12, 1, 1, 1
                values[16] = int(operation == 6)
                values[21] = 1024
            if operation == 5:
                values[17] = 1
            if operation == 8:
                values[27], values[28], values[29], values[30], values[31] = 1, 288, 96, 1, 1
                values[32], values[34], values[35], values[37] = 16, int(secondary), int(secondary), 1
        lines.append('DEVICE_OWNER_PROBE ' + json.dumps(dict(op=operation, hr=0, values=values)))

    stage('hidden-windows')
    stage('load-companion-dll')
    for _ in range(cycles):
        stage('create-primary')
        probe(0)
        stage('ui-and-cursor-resources')
        for operation in (1, 2, 7, 3):
            probe(operation)
        stage('postfx-and-partial-retry')
        probe(4)
        probe(6)
        stage('instance-storage-before-reset')
        probe(8)
        stage('reset-resource-contract')
        lines.append('DEVICE_OWNER_RESET ' + json.dumps(dict(enabled=enabled, hr=0x8876086c if enabled else 0)))
        stage('instance-storage-after-reset')
        probe(8, pending_atlas=True)
        probe(1)
        stage('second-device-owner-isolation')
        probe(0, secondary=True)
        probe(5, secondary=True)
        probe(8, secondary=True)
        probe(0)
        probe(1)
        probe(9, secondary=True)
        probe(0)
        probe(1)
        stage('external-resource-retains-device')
        stage('normal-device-destruction')
        probe(9)
        probe(0, ended=True)
    for name in ('normal-factory-destruction', 'normal-dll-unload', 'normal-window-close', 'complete'):
        stage(name)
    summary = dict(schema=1, ok=True, stage='complete', error='', requestedCycles=cycles,
        cycles=cycles, devicesCreated=2 * cycles, devicesDestroyed=2 * cycles, probes=20 * cycles,
        uiDraws=7 * cycles, cursorUpdates=3 * cycles, postFxRuns=2 * cycles, partialRetries=cycles,
        resets=2 * cycles, resetRejectChecks=cycles if enabled else 0,
        resetRejectNotEnabled=0 if enabled else cycles, ownerChecks=cycles, heldDeviceChecks=cycles,
        actualGpuDraws=31 * cycles, postFxPixels=2048 * cycles, pixelFailures=0,
        cursorTexturePixels=417 * cycles, cursorPixelFailures=0,
        instanceBufferRuns=3 * cycles, instanceArms=2 * cycles, instanceOtherOwnerChecks=cycles,
        instanceTlsIsolationChecks=cycles, instanceBytesWritten=864 * cycles,
        instanceTexturePixels=48 * cycles, instanceTextureFailures=0,
        deviceFinalRefs=0, factoryFinalRefs=0, moduleUnloaded=True, windowsClosed=True,
        normalTeardown=True, gameLaunched=False, globalInput=False,
        allocatorFailureInjected=False, visualGameAcceptance=False, instanceDrawAcceptance=False)
    lines.append('DEVICE_OWNER_RESULT ' + json.dumps(summary))
    return '\n'.join(lines) + '\n'


def mutate_record(transcript, prefix, index, mutate):
    lines = transcript.splitlines()
    found = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    selected = found[index]
    value = json.loads(lines[selected][len(prefix):])
    mutate(value)
    lines[selected] = prefix + json.dumps(value)
    return '\n'.join(lines) + '\n'


class NativeReceiptReaderTests(unittest.TestCase):
    def setUp(self):
        self.transcript = synthetic_transcript()

    def reject(self, transcript):
        with self.assertRaises(ValueError):
            gate.check_output(transcript, '', 0, 3)

    def test_complete_scalar_population_and_repeated_cycles(self):
        result = gate.check_output(self.transcript, 'driver diagnostic retained\n', 0, 3)
        self.assertEqual(result['probeRecords'], 60)
        self.assertEqual(result['summary']['actualGpuDraws'], 93)
        self.assertEqual(result['resetResourceRejection'], 'PROVEN')
        self.assertFalse(result['instanceDrawAcceptance'])
        self.assertFalse(result['gameVisualAcceptance'])

    def test_disabled_reset_check_is_explicitly_not_enabled(self):
        transcript = synthetic_transcript(enabled=False)
        result = gate.check_output(transcript, '', 0, 3)
        self.assertEqual(result['resetResourceRejection'], 'NOT_ENABLED')
        self.assertEqual(result['summary']['resetRejectChecks'], 0)
        transcript = mutate_record(transcript, 'DEVICE_OWNER_RESULT ', 0,
                                   lambda row: row.update(resetRejectChecks=3, resetRejectNotEnabled=0))
        self.reject(transcript)

    def test_each_failed_actual_field_overrides_optimistic_summary(self):
        # Ref leaks, actual draws, cursor same-extent update, PostFX recovery,
        # per-device instance/TLS ownership, and final clearing are independent.
        cases = ((0, 0, 2), (0, 2, 2), (0, 3, 2), (1, 7, 0), (1, 10, 0), (1, 13, 0),
                 (2, 19, 9), (2, 23, 0), (2, 24, 63), (3, 26, 0xff80c040), (4, 25, 1),
                 (5, 12, 11), (5, 21, 1023), (5, 22, 1), (6, 16, 0),
                 (7, 27, 0), (7, 28, 287), (7, 29, 95), (7, 30, 0), (7, 31, 0),
                 (7, 32, 15), (7, 33, 1), (7, 37, 0), (8, 36, 1),
                 (10, 5, 1), (11, 17, 0), (12, 34, 0), (12, 35, 0),
                 (15, 36, 0), (16, 36, 1), (18, 36, 0), (19, 6, 1), (19, 36, 1),
                 (0, 38, 1), (1, 14, 1))
        for position, field, value in cases:
            with self.subTest(probe=position, field=field, value=value):
                self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', position,
                    lambda row, f=field, v=value: row['values'].__setitem__(f, v)))

    def test_probe_field_types_and_widths_are_strict(self):
        for value in (True, False, -1, 1 << 64, 1.0, '1', None):
            with self.subTest(value=value):
                self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', 1,
                    lambda row, v=value: row['values'].__setitem__(13, v)))
        self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', 1,
                                  lambda row: row['values'].pop()))
        self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', 1,
                                  lambda row: row.update(hr=0x80004005)))

    def test_summary_counts_flags_and_types_cannot_override_records(self):
        for key, value in (('actualGpuDraws', 94), ('probes', 59), ('cycles', 2), ('uiDraws', 20),
                           ('cursorTexturePixels', 0), ('postFxPixels', 0), ('partialRetries', 0),
                           ('instanceTexturePixels', 143), ('instanceTextureFailures', 1),
                           ('instanceOtherOwnerChecks', 0), ('instanceTlsIsolationChecks', 0),
                           ('instanceBytesWritten', 0), ('instanceArms', 0),
                           ('deviceFinalRefs', 1), ('factoryFinalRefs', 1), ('ok', False),
                           ('moduleUnloaded', False), ('normalTeardown', False), ('windowsClosed', False),
                           ('gameLaunched', True), ('globalInput', True), ('allocatorFailureInjected', True),
                           ('visualGameAcceptance', True), ('instanceDrawAcceptance', True),
                           ('schema', True), ('requestedCycles', 3.0), ('error', 'failed')):
            with self.subTest(key=key, value=value):
                self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_RESULT ', 0,
                                          lambda row, k=key, v=value: row.update({k: v})))

    def test_reset_observation_matches_live_configuration_and_hresult(self):
        for changes in (dict(enabled=False), dict(hr=0), dict(enabled=1), dict(hr=-2005530516)):
            with self.subTest(changes=changes):
                self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_RESET ', 0,
                                          lambda row, c=changes: row.update(c)))
        self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', 1,
                                  lambda row: row['values'].__setitem__(18, 0)))

    def test_native_sequence_missing_duplicate_and_reordered_fail(self):
        lines = self.transcript.splitlines()
        probes = [i for i, line in enumerate(lines) if line.startswith('DEVICE_OWNER_PROBE ')]
        missing = lines.copy()
        del missing[probes[7]]
        self.reject('\n'.join(missing))
        duplicate = lines.copy()
        duplicate.insert(probes[7], duplicate[probes[7]])
        self.reject('\n'.join(duplicate))
        reordered = lines.copy()
        reordered[probes[2]], reordered[probes[3]] = reordered[probes[3]], reordered[probes[2]]
        self.reject('\n'.join(reordered))
        self.reject(self.transcript + lines[-1] + '\n')

    def test_protocol_unknown_and_duplicate_keys_fail(self):
        self.reject(self.transcript + 'DEVICE_OWNER_UNKNOWN {}\n')
        self.reject(self.transcript.replace('"hr": 0', '"hr": 5, "hr": 0', 1))
        self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_PROBE ', 1,
                                  lambda row: row.update(extra=0)))
        self.reject(mutate_record(self.transcript, 'DEVICE_OWNER_RESULT ', 0,
                                  lambda row: row.update(extra=True)))


class MetadataReaderTests(unittest.TestCase):
    def test_duplicate_json_key_rejected(self):
        with self.assertRaises(ValueError):
            gate.strict_json('{"ok":false,"ok":true}')

    def test_nonfinite_json_rejected(self):
        for value in ('NaN', 'Infinity', '-Infinity'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.strict_json('{"value":' + value + '}')

    def test_native_uint_is_not_bool_float_signed_or_overflow(self):
        for value in (True, False, 1.0, -1, 1 << 64, '1', None):
            with self.subTest(value=value):
                self.assertFalse(gate.native_uint(value))
        self.assertTrue(gate.native_uint((1 << 64) - 1))

    @staticmethod
    def exports():
        original = [dict(ordinal=37, names=['Direct3DCreate9'], forwarder=None),
                    dict(ordinal=28257, names=[], forwarder=None)]
        added = dict(ordinal=28275, names=[gate.PROBE_EXPORT], forwarder=None)
        return original, original + [added]

    def test_export_extension_preserves_ordinal_only_entries(self):
        product, diagnostic = self.exports()
        gate.check_exports(product, diagnostic)

    def test_product_probe_export_forbidden(self):
        _, diagnostic = self.exports()
        with self.assertRaises(ValueError):
            gate.check_exports(diagnostic, diagnostic)

    def test_named_or_ordinal_only_export_loss_rejected(self):
        for index in (0, 1):
            product, diagnostic = self.exports()
            del diagnostic[index]
            with self.subTest(index=index), self.assertRaises(ValueError):
                gate.check_exports(product, diagnostic)

    def test_export_ordinal_alias_forwarder_changes_rejected(self):
        for key, value in (('ordinal', 38), ('names', ['Renamed']), ('forwarder', 'other.Real')):
            product, diagnostic = self.exports()
            diagnostic = copy.deepcopy(diagnostic)
            diagnostic[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.check_exports(product, diagnostic)

    def test_extra_diagnostic_export_rejected(self):
        product, diagnostic = self.exports()
        diagnostic.append(dict(ordinal=28276, names=['Unexpected'], forwarder=None))
        with self.assertRaises(ValueError):
            gate.check_exports(product, diagnostic)

    @staticmethod
    def link_plan():
        # No file is created or command executed by this test.
        build = gate.ROOT / 'build32_device_owner_reader_only'
        product = build / 'src/d3d9/d3d9.dll'
        diagnostic = gate.ROOT / 'AutoTest/artifacts/reader-only/diagnostic/d3d9.dll'
        probe = gate.ROOT / 'AutoTest/artifacts/reader-only/probe.obj'
        command = ['clang.exe', '-o', str(product), 'production.obj', '-shared',
                   '../src/d3d9/d3d9.def', '-Wl,--out-implib=' + str(product) + '.a',
                   '-Wl,--start-group', 'production.a', '-luser32', '-Wl,--end-group']
        return build, product, diagnostic, probe, command

    def test_only_link_outputs_and_probe_are_changed(self):
        build, product, diagnostic, probe, command = self.link_plan()
        original = command.copy()
        actual = gate.isolated_link_command(command, build, product, diagnostic, probe)
        self.assertEqual(command, original)
        self.assertEqual(actual[-1], str(probe))
        self.assertEqual(len(actual), len(command) + 1)
        self.assertEqual(sum(x != y for x, y in zip(actual, command)), 2)
        self.assertIn('../src/d3d9/d3d9.def', actual)
        self.assertIn('production.obj', actual)
        self.assertIn('production.a', actual)

    def test_wrong_output_or_import_library_fails_closed(self):
        for index, value in ((2, 'other.dll'), (6, '-Wl,--out-implib=other.a')):
            build, product, diagnostic, probe, command = self.link_plan()
            command[index] = value
            with self.subTest(index=index), self.assertRaises(ValueError):
                gate.isolated_link_command(command, build, product, diagnostic, probe)

    def test_unexpanded_rsp_and_unowned_output_flags_rejected(self):
        for value in ('@product.rsp', '-Wl,-Map=product.map', '-Wl,--output-def=product.def', '&&'):
            build, product, diagnostic, probe, command = self.link_plan()
            command.append(value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.isolated_link_command(command, build, product, diagnostic, probe)

    def test_no_actual_records_is_never_success(self):
        for stdout in ('', 'PASS\n', 'DEVICE_OWNER_RESULT {"ok":true}\n'):
            with self.subTest(stdout=stdout), self.assertRaises(ValueError):
                gate.check_output(stdout, '', 0, 3)

    def test_nonzero_or_missing_exit_is_not_normal_teardown(self):
        for exit_code in (2, -1, None, False):
            with self.subTest(exit_code=exit_code), self.assertRaises(ValueError):
                gate.check_output('', '', exit_code, 3)

    def test_native_probe_failure_on_stderr_rejected(self):
        with self.assertRaises(ValueError):
            gate.check_output('', 'DEVICE_OWNER_PROBE_FAILURE {"op":4,"error":"failure"}', 0, 3)


if __name__ == '__main__':
    unittest.main()
