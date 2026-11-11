"""Synthetic Python-reader proof only; does not run native peers or a GPU.

The fixed bytes and expected resource/phase populations below are independent
test inputs, not native observations and not imported from the reader's tables.
"""
import copy
from pathlib import Path
import struct
import tempfile
import unittest

import run_render_host_memory_gate as gate


KINDS = ('vertexBuffer', 'indexBuffer', 'texture', 'systemSurface',
         'vertexShader', 'pixelShader', 'declaration', 'presentedReadback')
PHASES = ((1, 0), (2, 1), (3, 1), (4, 1), (4, 16), (4, 48), (4, 95), (5, 96))
SEQUENCE = (3, 15, 20, 27, 328, 969, 1910, 2437)
SERIAL = (1, 5, 8, 8, 113, 337, 666, 673)
DRAW = (0, 0, 1, 1, 16, 48, 95, 96)
UPLOAD = (0, 3, 3, 3, 48, 144, 285, 288)
DESKTOP = 'WarVK-M0-reader-only'
INPUT = 'Default'


def os_row(host=False):
    unit = 1024*1024
    return dict(valid=True, win32=0, privateBytes=(12 if host else 4)*unit,
        peakCommitBytes=(128 if host else 64)*unit, workingSetBytes=8*unit,
        usedVirtualBytes=32*unit, privateVirtualBytes=16*unit, mappedVirtualBytes=4*unit, regions=400)


def cpu_row(live, host=False):
    kinds = {}
    for kind in KINDS:
        kinds[kind] = dict(objects=0, logicalBytes=0, cpuSizeBytes=0, cpuCapacityBytes=0)
    if not host:
        kinds['systemSurface'] = dict(objects=1, logicalBytes=16384, cpuSizeBytes=16384, cpuCapacityBytes=16384)
    if live:
        sizes = dict(vertexBuffer=160, indexBuffer=12, texture=4, vertexShader=208,
                     pixelShader=112, declaration=24 if host else 32)
        for kind, logical in sizes.items():
            size = (12 if kind == 'indexBuffer' else 20 if kind == 'vertexShader' else 0) if host else logical
            capacity = 16384 if not host and kind in ('vertexShader', 'pixelShader') else size
            kinds[kind] = dict(objects=1, logicalBytes=logical, cpuSizeBytes=size, cpuCapacityBytes=capacity)
    return dict(valid=True, kinds=kinds)


def golden_wire(index):
    """Independent explicit offsets, including nonzero high nonce bits."""
    phase, ordinal = PHASES[index]; live = phase in (2, 3, 5)
    raw = bytearray(512)
    struct.pack_into('<8I', raw, 0, 0x314d3944, 1, phase, ordinal, 202, 404, 64, 6 if live else 0)
    struct.pack_into('<6Q', raw, 32, 20002, 0x1122334455667788, 0x8877665544332211,
                     SEQUENCE[index], 1000+index*100+2, 1000+index*100+3)
    struct.pack_into('<9Q', raw, 80, 520 if live else 0, SERIAL[index], SERIAL[index], SERIAL[index],
                     DRAW[index], UPLOAD[index], DRAW[index], 131072, 1048576)
    process = os_row(True)
    struct.pack_into('<2I7Q', raw, 152, 1, 0, process['privateBytes'], process['peakCommitBytes'],
                     process['workingSetBytes'], process['usedVirtualBytes'], process['privateVirtualBytes'],
                     process['mappedVirtualBytes'], process['regions'])
    struct.pack_into('<2I', raw, 216, 1, 0)
    for at, kind in enumerate(KINDS):
        row = cpu_row(live, True)['kinds'][kind]
        struct.pack_into('<4Q', raw, 224+at*32, row['objects'], row['logicalBytes'], row['cpuSizeBytes'], row['cpuCapacityBytes'])
    return raw.hex()


def records():
    launch = dict(ok=True, argv=['reader-fixture.exe'], actualExecutable='reader-fixture.exe', jobActiveProcesses=0,
        jobAtomic=True, handleListApplied=True, processSettled=True, jobEmpty=True, timedOut=False,
        forcedTermination=False, desktopHandleInherited=False, pid=101, creation100ns=10001, initialThreadId=303,
        exitCode=0, cleanupErrors=[], desktop=DESKTOP, inputBefore=INPUT, inputAfter=INPUT)
    proxy = dict(schema='warvk-d3d9-proxy/v1', childPid=202, childCreation=20002, childExit=0, exchanges=2445,
        peerVerified=True, transportFailed=False, x86VulkanAbsent=True, deviceLost=False, remoteTerminal=False,
        childNaturalExit=True, childProcessSettled=True, childSelfFailStop=False, primaryExitWaitMs=2,
        productIntegrated=False, memorySavingsProven=False)
    summary = dict(schema='warvk-d3d9-memory/v1', ok=True, bits=32, mode='com', publicD3D9Api=True,
        rounds=96, draws=96, readbacks=96, pixelsChecked=319488, pixelFailures=0, maxChannelError=1,
        resourcesCreated=576, resourcesReleased=576, privateBoundDraws=96, finalPrivateBoundResources=6,
        queryIssues=256, queryPolls=256, checkpoints=8, cpuHelperChecks=54, vertexShaderBytes=208,
        pixelShaderBytes=112, vertexFloatConstants=1, pixelFloatConstants=0, vertexIntConstants=0,
        pixelIntConstants=0, vertexBoolConstants=0, pixelBoolConstants=0, hostFloatConstantBytes=20,
        parentPid=101, parentThread=303, parentCreation=10001, initialProcess=os_row(), finalProcess=os_row(),
        fixtureCleanupComplete=True, parentVulkanAbsent=True, desktop=DESKTOP, inputBefore=INPUT, inputAfter=INPUT,
        globalInputUsed=False, desktopSwitched=False, gameTouched=False, productIntegrated=False,
        memorySavingsProven=False, gpuPhysicalMeasured=False, opaqueCpuMeasured=False, synchronizedInstant=False,
        windowPixelsVerified=False, playerVisualAccepted=False)
    host = dict(schema='warvk-d3d9-host/v1', ok=True, bits=64, actualDxvkCore=True, actualDxso=True,
        accepted=2445, rejected=0, backendCalls=2243, submitted=673, gpuCompleted=673, retired=673,
        draws=96, clears=96, uploads=288, uploadedBytes=16896, readbacks=96, queries=256,
        buffersCreated=192, texturesCreated=96, shadersCompiled=192, declarationsCreated=96,
        liveResources=0, liveLogicalBytes=0, peakLogicalBytes=520, vendorId=4318, deviceId=1234,
        normalTeardown=True, presentImplemented=False, presentAttempts=0, acquiredImages=0, presentQueued=0,
        presentApiSucceeded=0, presentGpuCompleted=0, presentRetired=0, lastAcquireVkResult=1, lastPresentVkResult=1,
        windowProcessId=0, windowThreadId=0, gpuInitialized=True, shutdownQueueDrained=True,
        presentedReadbacks=0, presentedFrame=0, logicalHandlesCreated=577, logicalHandlesDestroyed=577,
        logicalHandlesReused=570, resourcesReused=570, queryHandlesCreated=1, queryHandlesDestroyed=1,
        queryHandlesReused=0, queryIssues=256, queryPolls=256, queryWaits=0, liveLogicalHandles=0, liveQueries=0,
        productIntegrated=False, gameTouched=False, memorySavingsProven=False)
    desktop = dict(schema='warvk-d3d9-desktop/v1', pid=202, threadId=404, peerPid=101, windowThreadId=0,
                   windowStation='WinSta0', desktop=DESKTOP, inputDesktop=INPUT, beforeGpu=True)
    checkpoints = []; mirrors = []
    for index, (phase, ordinal) in enumerate(PHASES):
        begin = 1000+index*100; live = phase in (2, 3, 5); private = phase in (3, 5)
        parent = dict(version=1, phase=phase, ordinal=ordinal, pid=101, thread=303, bits=32,
            logicalHandles=7 if live else 1, reservedHandles=0, privateBindings=6 if private else 0,
            publicZeroBindings=6 if private else 0, expectedHostPid=202, creation=10001,
            tickBegin=begin, beforeEnd=begin+1, afterBegin=begin+4, tickEnd=begin+5,
            expectedHostCreation=20002, expectedNonceLow=0x1122334455667788,
            expectedNonceHigh=0x8877665544332211, expectedSequence=SEQUENCE[index],
            connectionInlineBytes=409600, fixedPacketPayloadBytes=393216, workerBatchPayloadBytes=65536,
            before=os_row(), after=os_row(), cpu=cpu_row(live))
        wire = golden_wire(index)
        checkpoints.append(dict(schema='warvk-d3d9-memory-checkpoint/v1', phase=phase, ordinal=ordinal,
            parent=parent, hostWireHex=wire, synchronizedInstant=False, gpuPhysicalMeasured=False,
            opaqueCpuMeasured=False, gameTouched=False, productIntegrated=False, memorySavingsProven=False))
        mirrors.append(dict(schema='warvk-d3d9-memory-host/v1', phase=phase, ordinal=ordinal,
                            sequence=SEQUENCE[index], wireHex=wire))
    return checkpoints+[proxy, summary], [desktop]+mirrors+[host], launch


def edit_wire(parents, hosts, index, offset, fmt, value, paired=True):
    raw = bytearray.fromhex(parents[index]['hostWireHex'])
    struct.pack_into('<'+fmt, raw, offset, value)
    parents[index]['hostWireHex'] = raw.hex()
    if paired: hosts[index+1]['wireHex'] = raw.hex()


class MemoryReaderTests(unittest.TestCase):
    def check(self, state=None):
        return gate.check_case(*(records() if state is None else state), DESKTOP, INPUT)

    def reject(self, state):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            self.check(state)

    def test_valid_synthetic_account(self):
        result = self.check()
        self.assertTrue(result['ok'])
        self.assertEqual(result['checkpoints'], 8)
        self.assertEqual(result['drainedOrdinals'], [1, 16, 48, 95])
        self.assertTrue(result['publicZeroPrivateResourcesStayedLive'])
        self.assertTrue(result['exactOwnedBaselineRecovered'])
        self.assertFalse(result['memorySavingsProven'])
        self.assertFalse(result['gpuPhysicalMeasured'])
        self.assertFalse(result['synchronizedInstant'])
        self.assertIsNone(result['osPlateauAcceptanceThreshold'])

    def test_fixed_binary_golden_offsets(self):
        raw = bytes.fromhex(golden_wire(2))
        self.assertEqual(raw[:16].hex(), '44394d31010000000300000001000000')
        self.assertEqual(raw[40:56].hex(), '88776655443322111122334455667788')
        self.assertEqual(raw[480:], bytes(32))
        host = gate.decode_host(raw.hex())
        self.assertEqual(host['phase'], 3)
        self.assertEqual(host['sequence'], 20)
        self.assertEqual(host['logicalBytes'], 520)
        self.assertEqual(host['cpu']['kinds']['vertexShader']['cpuSizeBytes'], 20)
        self.assertEqual(host['cpu']['kinds']['indexBuffer']['logicalBytes'], 12)

    def test_wire_encoding_shape(self):
        for bad in (None, b'00', '', '00'*511, '00'*513, golden_wire(0).upper(), 'g'*1024):
            with self.subTest(bad=str(bad)[:20]), self.assertRaises((ValueError, TypeError)):
                gate.decode_host(bad)

    def test_wire_reserved_and_boolean_rejections(self):
        for offset, fmt, value in ((480, 'I', 1), (511, 'B', 1), (220, 'I', 1),
                                    (216, 'I', 2), (152, 'I', 2), (152, 'I', 0), (216, 'I', 0), (156, 'I', 5)):
            state = records(); edit_wire(*state[:2], 0, offset, fmt, value)
            with self.subTest(offset=offset, value=value): self.reject(state)

    def test_wire_identity_rejections_both_peers_agree(self):
        for offset, fmt, value in ((0, 'I', 0), (4, 'I', 2), (8, 'I', 2), (12, 'I', 1),
            (16, 'I', 203), (20, 'I', 405), (24, 'I', 32), (32, 'Q', 20003),
            (40, 'Q', 7), (48, 'Q', 8), (56, 'Q', 4)):
            state = records(); edit_wire(*state[:2], 0, offset, fmt, value)
            with self.subTest(offset=offset): self.reject(state)

    def test_independent_mirror_mismatch(self):
        state = records(); edit_wire(*state[:2], 1, 168, 'Q', 128*1024*1024+1, paired=False)
        self.reject(state)

    def test_matching_wrong_nonce_not_expected_nonce(self):
        state = records()
        for index in range(8): edit_wire(*state[:2], index, 40, 'Q', 5)
        self.reject(state)

    def test_sample_nonce_change_even_if_parent_agrees(self):
        state = records(); state[0][4]['parent']['expectedNonceHigh'] = 9
        edit_wire(*state[:2], 4, 48, 'Q', 9); self.reject(state)

    def test_nonce_pair_cannot_be_zero(self):
        state = records()
        for index in range(8):
            state[0][index]['parent']['expectedNonceLow'] = 0
            state[0][index]['parent']['expectedNonceHigh'] = 0
            edit_wire(*state[:2], index, 40, 'Q', 0); edit_wire(*state[:2], index, 48, 'Q', 0)
        self.reject(state)

    def test_parent_launch_identity(self):
        for field in ('pid', 'thread', 'creation', 'expectedHostPid', 'expectedHostCreation', 'expectedSequence'):
            state = records(); state[0][0]['parent'][field] += 1
            with self.subTest(field=field): self.reject(state)

    def test_parent_width_and_exact_number_types(self):
        for field in ('bits', 'phase', 'ordinal', 'logicalHandles', 'reservedHandles', 'tickBegin',
                      'expectedNonceLow', 'creation', 'privateBindings', 'connectionInlineBytes'):
            for bad in (False, True, -1, 1.0, '1', 1 << 64, None):
                state = records(); state[0][0]['parent'][field] = bad
                with self.subTest(field=field, bad=bad): self.reject(state)

    def test_u32_parent_overflow(self):
        state = records(); state[0][0]['parent']['pid'] = 1 << 32; self.reject(state)

    def test_time_window_each_boundary(self):
        for field, bad in (('tickBegin', 1002), ('beforeEnd', 1003), ('afterBegin', 1002), ('tickEnd', 1003)):
            state = records(); state[0][0]['parent'][field] = bad
            with self.subTest(field=field): self.reject(state)
        for offset, bad in ((64, 1000), (72, 1001), (72, 1005)):
            state = records(); edit_wire(*state[:2], 0, offset, 'Q', bad)
            with self.subTest(offset=offset, bad=bad): self.reject(state)

    def test_cross_checkpoint_window_overlap(self):
        state = records(); parent = state[0][1]['parent']
        parent.update(tickBegin=1000, beforeEnd=1001, afterBegin=1004, tickEnd=1005)
        edit_wire(*state[:2], 1, 64, 'Q', 1002); edit_wire(*state[:2], 1, 72, 'Q', 1003)
        self.reject(state)

    def test_zero_width_windows_allowed(self):
        state = records()
        for index in range(8):
            begin = 1000+index*100
            for name in ('tickBegin', 'beforeEnd', 'afterBegin', 'tickEnd'): state[0][index]['parent'][name] = begin
            edit_wire(*state[:2], index, 64, 'Q', begin); edit_wire(*state[:2], index, 72, 'Q', begin)
        self.assertTrue(self.check(state)['ok'])

    def test_gpu_counts_not_increased_by_diagnostic(self):
        for offset in (88, 96, 104, 112, 120, 128):
            state = records(); edit_wire(*state[:2], 0, offset, 'Q', 2)
            with self.subTest(offset=offset): self.reject(state)
        for field in ('accepted', 'backendCalls', 'submitted', 'draws'):
            state = records(); state[1][-1][field] += 8
            with self.subTest(field=field): self.reject(state)

    def test_owned_private_binding_lifetime(self):
        for field, bad in (('privateBindings', 0), ('publicZeroBindings', 0), ('logicalHandles', 1), ('reservedHandles', 1)):
            state = records(); state[0][2]['parent'][field] = bad
            with self.subTest(field=field): self.reject(state)
        state = records(); state[0][-1]['finalPrivateBoundResources'] = 0; self.reject(state)

    def test_exact_parent_owned_bytes(self):
        for kind in KINDS:
            for field in ('objects', 'logicalBytes', 'cpuSizeBytes'):
                state = records(); state[0][1]['parent']['cpu']['kinds'][kind][field] += 1
                with self.subTest(kind=kind, field=field): self.reject(state)

    def test_exact_host_owned_bytes(self):
        for kind_index in range(8):
            for offset in (0, 8, 16):
                state = records(); raw = bytes.fromhex(state[0][1]['hostWireHex'])
                where = 224+kind_index*32+offset; value = struct.unpack_from('<Q', raw, where)[0]
                edit_wire(*state[:2], 1, where, 'Q', value+1)
                with self.subTest(kind=kind_index, offset=offset): self.reject(state)

    def test_capacity_cannot_be_below_size(self):
        state = records(); state[0][1]['parent']['cpu']['kinds']['indexBuffer']['cpuCapacityBytes'] = 11; self.reject(state)
        state = records(); edit_wire(*state[:2], 1, 224+32+24, 'Q', 11); self.reject(state)

    def test_dead_private_payload_baseline(self):
        state = records(); row = state[0][3]['parent']['cpu']['kinds']['vertexBuffer']
        row.update(objects=1, logicalBytes=160, cpuSizeBytes=160, cpuCapacityBytes=160); self.reject(state)
        state = records(); state[0][3]['parent']['cpu']['kinds']['vertexBuffer']['cpuCapacityBytes'] = 160; self.reject(state)

    def test_same_shape_capacity_growth(self):
        state = records(); state[0][7]['parent']['cpu']['kinds']['vertexBuffer']['cpuCapacityBytes'] = 320; self.reject(state)
        state = records(); edit_wire(*state[:2], 7, 224+32+24, 'Q', 24); self.reject(state)

    def test_constant_counts_bind_host_cpu(self):
        state = records(); state[0][-1]['vertexFloatConstants'] = 2; self.reject(state)
        state = records(); state[0][-1]['hostFloatConstantBytes'] = 24; self.reject(state)
        state = records(); state[0][-1]['pixelIntConstants'] = 1; self.reject(state)
        state = records(); state[0][-1]['vertexShaderBytes'] += 4; self.reject(state)

    def test_non_payload_host_kinds_capacity_zero(self):
        for index in (0, 2, 6):
            state = records(); edit_wire(*state[:2], 1, 224+index*32+24, 'Q', 4)
            with self.subTest(kind=index): self.reject(state)

    def test_fixed_buffers_are_subset_not_totals(self):
        for field, value in (('connectionInlineBytes', 393215), ('fixedPacketPayloadBytes', 393215),
                             ('workerBatchPayloadBytes', 65535)):
            state = records(); state[0][0]['parent'][field] = value
            with self.subTest(field=field): self.reject(state)
        state = records(); edit_wire(*state[:2], 0, 136, 'Q', 131073); self.reject(state)
        state = records(); edit_wire(*state[:2], 0, 144, 'Q', 0); self.reject(state)

    def test_unknown_measurements_rejected(self):
        for parent_where in ('before', 'after'):
            for field, value in (('valid', False), ('win32', 5), ('privateBytes', 0), ('regions', 0)):
                state = records(); state[0][0]['parent'][parent_where][field] = value
                with self.subTest(where=parent_where, field=field): self.reject(state)
        state = records(); state[0][0]['parent']['cpu']['valid'] = False; self.reject(state)

    def test_os_types_missing_and_overflow(self):
        for field in os_row():
            state = records(); del state[0][0]['parent']['before'][field]
            with self.subTest(field=field): self.reject(state)
        for bad in (False, -1, 1.5, 1 << 64, None):
            state = records(); state[0][0]['parent']['after']['privateBytes'] = bad
            with self.subTest(bad=bad): self.reject(state)
        state = records(); state[0][0]['parent']['before']['usedVirtualBytes'] = 1 << 32; self.reject(state)

    def test_peak_and_virtual_account_invariants(self):
        for field, value in (('peakCommitBytes', 1), ('privateVirtualBytes', 1 << 30)):
            state = records(); state[0][0]['parent']['before'][field] = value
            with self.subTest(field=field): self.reject(state)
        state = records(); state[0][3]['parent']['before']['peakCommitBytes'] -= 4096; self.reject(state)

    def test_os_growth_is_observation_not_invented_plateau_gate(self):
        state = records(); state[0][-1]['finalProcess']['privateBytes'] = 60*1024*1024
        state[0][-1]['finalProcess']['usedVirtualBytes'] = 96*1024*1024
        result = self.check(state)
        self.assertEqual(result['parentOS']['privateBytes']['delta'], 56*1024*1024)
        self.assertFalse(result['memorySavingsProven'])
        self.assertIsNone(result['osPlateauAcceptanceThreshold'])

    def test_empty_missing_duplicate_unknown_reordered_records(self):
        for stream in (0, 1):
            state = records(); state[stream].clear(); self.reject(state)
            for index in range(10):
                state = records(); del state[stream][index]
                with self.subTest(stream=stream, index=index): self.reject(state)
                state = records(); state[stream].append(copy.deepcopy(state[stream][index])); self.reject(state)
            state = records(); state[stream].append(dict(schema='unexpected/v1')); self.reject(state)
        state = records(); state[0][0], state[0][1] = state[0][1], state[0][0]; self.reject(state)
        state = records(); state[1][1], state[1][2] = state[1][2], state[1][1]; self.reject(state)
        state = records(); state[0][0], state[0][-1] = state[0][-1], state[0][0]; self.reject(state)
        state = records(); state[1][0], state[1][1] = state[1][1], state[1][0]; self.reject(state)

    def test_exact_memory_field_sets(self):
        for index in (0, -1):
            state = records(); state[0][index]['unknownField'] = 0; self.reject(state)
        state = records(); state[0][0]['parent']['unknownField'] = 0; self.reject(state)
        state = records(); state[0][0]['parent']['cpu']['kinds']['missingType'] = {}; self.reject(state)
        state = records(); del state[0][0]['parent']['cpu']['kinds']['vertexBuffer']; self.reject(state)
        state = records(); state[1][1]['unknownField'] = 0; self.reject(state)

    def test_all_scope_flags_are_real_boolean_false(self):
        for index in (0, -1):
            for field in ('synchronizedInstant', 'gpuPhysicalMeasured', 'opaqueCpuMeasured',
                          'gameTouched', 'productIntegrated', 'memorySavingsProven'):
                for value in (True, 0, 'false'):
                    state = records(); state[0][index][field] = value
                    with self.subTest(index=index, field=field, value=value): self.reject(state)

    def test_primary_exit_not_later_cleanup(self):
        for field, value in (('childExit', 1), ('childNaturalExit', False), ('childProcessSettled', False),
            ('primaryExitWaitMs', 5001), ('transportFailed', True), ('childSelfFailStop', True)):
            state = records(); state[0][-2][field] = value
            with self.subTest(field=field): self.reject(state)

    def test_real_launch_settlement_and_image_required(self):
        for field, value in (('jobActiveProcesses', 1), ('jobActiveProcesses', False), ('exitCode', 1),
            ('timedOut', True), ('forcedTermination', True), ('jobEmpty', False), ('processSettled', False),
            ('cleanupErrors', ['bad']), ('actualExecutable', 'other.exe'), ('inputAfter', 'Changed')):
            state = records(); state[2][field] = value
            with self.subTest(field=field): self.reject(state)

    def test_pixels_and_full_workload_not_weakened(self):
        for field, value in (('rounds', 16), ('pixelsChecked', 3328), ('pixelFailures', 1), ('maxChannelError', 2),
            ('resourcesReleased', 570), ('queryIssues', 255), ('queryPolls', 255), ('cpuHelperChecks', 53)):
            state = records(); state[0][-1][field] = value
            with self.subTest(field=field): self.reject(state)

    def test_final_resource_retirement_and_gpu_population(self):
        for field in ('liveResources', 'liveLogicalBytes', 'liveLogicalHandles', 'liveQueries', 'retired',
                      'logicalHandlesDestroyed', 'resourcesReused', 'presentAttempts', 'peakLogicalBytes'):
            state = records(); state[1][-1][field] += 1
            with self.subTest(field=field): self.reject(state)
        state = records(); state[1][-1]['normalTeardown'] = False; self.reject(state)

    def test_strict_json_duplicate_keys_nonfinite_numbers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'reader.jsonl'
            for text in ('{"schema":"x","schema":"x"}', '{"value":NaN}', '{"value":Infinity}'):
                path.write_text(text+'\n', encoding='utf-8')
                with self.subTest(text=text), self.assertRaises(ValueError): gate.json_lines(path)

    def test_actual_record_mutation_engine_on_synthetic_inputs(self):
        result = gate.mutations(*records(), DESKTOP, INPUT)
        self.assertEqual(result['tested'], result['killed'])
        self.assertGreater(result['tested'], 1000)
        self.assertFalse(result['nativeRerun'])
        self.assertFalse(result['gpuRerun'])


class RawReceiptTests(unittest.TestCase):
    def test_producer_time_probe_cpu_case_population_and_bytes(self):
        with tempfile.TemporaryDirectory(prefix='warvk-m0-raw-reader-') as temporary:
            root = Path(temporary).resolve(); directory = root/'memory-account'; directory.mkdir()
            def capture(folder, names, logs=False):
                for name in names: (folder/name).write_bytes(b'{"ok":true}\n')
                return gate.capture_raw_artifacts(folder, names, logs)
            receipt = dict(rawArtifactsPreserved=True,
                probeRawArtifacts=capture(root, ['probe.stdout', 'probe.stderr', 'desktop-child.log']),
                stepRawArtifacts=[capture(root, [f'{name}-{bits}.stdout', f'{name}-{bits}.stderr'])
                    for bits in (32, 64) for name in ('resource_registry', 'wire')],
                cases=[dict(label='memory-account', process=dict(stdout=str(directory/'case.stdout'),
                    stderr=str(directory/'case.stderr')), initialRawArtifacts=capture(directory,
                    ['case.stdout', 'case.stderr', 'host.log'], True))])
            self.assertEqual(len(gate.verify_raw_receipt(receipt, root)), 6)
            for path in (root/'probe.stdout', root/'probe.stderr', root/'desktop-child.log',
                         root/'wire-64.stdout', directory/'host.log', directory/'case.stderr'):
                original = path.read_bytes(); path.write_bytes(original+b'ERROR: late non-JSON bytes\n')
                with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, 'changed since launch return'):
                    gate.verify_raw_receipt(receipt, root)
                path.write_bytes(original)
            for key in ('rawArtifactsPreserved', 'probeRawArtifacts', 'stepRawArtifacts'):
                changed = copy.deepcopy(receipt); del changed[key]
                with self.subTest(missing=key), self.assertRaises(ValueError): gate.verify_raw_receipt(changed, root)
            changed = copy.deepcopy(receipt); del changed['cases'][0]['initialRawArtifacts']
            with self.assertRaises(ValueError): gate.verify_raw_receipt(changed, root)
            changed = copy.deepcopy(receipt); changed['stepRawArtifacts'].reverse()
            with self.assertRaises(ValueError): gate.verify_raw_receipt(changed, root)
            (directory/'late.log').write_bytes(b'late log\n')
            with self.assertRaisesRegex(ValueError, 'population changed'): gate.verify_raw_receipt(receipt, root)


if __name__ == '__main__':
    unittest.main(verbosity=2)
