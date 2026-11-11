"""Pure counterexamples for the actual X1a CPU/IPC fault reader.

The independent fixture below is a protocol transcript, not a native run. These
tests never create a process, pipe, event, Desktop, Vulkan device or temporary
file. Native/build ownership remains with the integration owner.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_d3d9_connection_fault_ipc as m

CASES = ('sfalse', 'payload', 'nonce', 'peer-exit', 'partial-header')
DESKTOP = 'WarVK_RenderLab_independent_fixture'
INPUT = 'Default'
LOST = 0x88760868
PARENT_PID, CHILD_PID = 101, 202
PARENT_CREATED, CHILD_CREATED = 100001, 100002
OWNER_TID, CONTROLLER_TID, CHILD_TID = 11, 12, 21
NONCE_LOW, NONCE_HIGH = 0x1122334455667788, 0x8877665544332211


def desktop_row(role, bits, pid, tid):
    return dict(schema='warvk-x1a-fault-desktop/v1', role=role, pid=pid, tid=tid,
        bits=bits, desktop=DESKTOP, inputDesktop=INPUT, station='WinSta0',
        isolatedDesktop='WinSta0\\' + DESKTOP, vulkanAbsent=True)


def diagnostic(case):
    """Literal expected contract, deliberately not built from reader tables."""
    return dict(schema='warvk-d3d9-async-clear/v1', initializedAcknowledged=True,
        clearPublished=1, clearRemoteSucceeded=0, fullObservations=0,
        backpressureCalls=0, backpressureMs=0, maxBackpressureMs=0,
        maxObservedOutstanding=2, lastPublished=3, lastReplySettled=1,
        asyncFailureSequence=2 if case in ('sfalse', 'payload') else 0,
        asyncFailureResult=1 if case == 'sfalse' else 0, queueCapacity=4,
        workerPrivateBatchCapacity=1, admissionDeadlineMs=5000,
        callDeadlineMs=30000, replyUsesRemainingCallBudget=True,
        transportDeadlineMs=5000, primaryExitDeadlineMs=5000,
        gpuCompletionInferredFromAdmission=False, productOwnerComplete=False)


def fault_reason(case):
    if case in ('sfalse', 'payload'):
        return 'D3D9 asynchronous/init reply contract primaryExitWaitMs=0'
    if case == 'nonce':
        return 'D3D9 reply identity primaryExitWaitMs=0'
    if case == 'peer-exit':
        return 'peer-exited stage=header win32=0 primaryExitWaitMs=0'
    return 'timeout stage=header win32=258 primaryExitWaitMs=0'


def stderr_for(case, values=None):
    reason = fault_reason(case)
    return (f'D3D9 dev primary failure={reason}\n'
        f'D3D9 dev secondary-exit-drain pid={CHILD_PID} exit=0 elapsedMs=3 originalPreserved=true\n'
        f'D3D9 dev worker: {reason}\n'
        'D3D9 async-clear: ' + json.dumps(diagnostic(case) if values is None else values,
                                        separators=(',', ':')) + '\n'
        'D3D9 draw-batch-proxy: ' + json.dumps(dict(schema='warvk-d3d9-draw-batch-proxy/v1',
            enabled=False,admittedDraws=0,flushedBatches=0,pendingDraws=0,capacity=32,
            payloadCapacityBytes=37248,asyncAdmission=False,remoteSucceededBatches=0,
            asyncEvents=False,eventWaitsPublished=0,eventWaitsSucceeded=0,eventPendingObservations=0,
            eventLocalCompletions=0,eventGpuCompleted=0,gpuCompletionInferredFromAdmission=False),separators=(',',':'))+'\n')


def fixture(case='sfalse'):
    reply_fault = case in ('sfalse', 'payload')
    result = dict(schema='warvk-x1a-fault-call/v1', case=case,
        pid=PARENT_PID, creation=PARENT_CREATED, childPid=CHILD_PID,
        childCreation=CHILD_CREATED, nonceLow=NONCE_LOW, nonceHigh=NONCE_HIGH,
        published=3, acknowledged=1, queued=1, pendingCpuAck=True,
        faulted=True, lastPublished=3, done=1,
        asyncFailureSequence=2 if reply_fault else 0,
        asyncFailureResult=1 if case == 'sfalse' else 0,
        barrierHresult=LOST, guardUnchanged=True, guardBytes=32, resultBytes=16,
        waitingHandshake=True, subsequentClearRefused=True,
        barrierElapsedMs=5001 if case == 'partial-header' else 15,
        ipcTested=True, gpuTested=False, publicComTested=False,
        productOwnerComplete=False)
    proxy = dict(schema='warvk-d3d9-proxy/v1', childPid=CHILD_PID,
        childCreation=CHILD_CREATED, childExit=259, exchanges=1,
        peerVerified=True, transportFailed=True, x86VulkanAbsent=True,
        deviceLost=False, remoteTerminal=False, childNaturalExit=False,
        childProcessSettled=False, childSelfFailStop=False, primaryExitWaitMs=0,
        productIntegrated=False, memorySavingsProven=False)
    peer = dict(schema='warvk-x1a-fault-peer/v1', case=case, pid=CHILD_PID,
        creation=CHILD_CREATED, parentPid=PARENT_PID, parentCreation=PARENT_CREATED,
        nonceLow=NONCE_LOW, nonceHigh=NONCE_HIGH, requests=2, lastSequence=2,
        readbackHandshake=True, endOfStreamObserved=case != 'peer-exit',
        intentionalExit=case == 'peer-exit', peerVerified=True,
        gpuTested=False, productOwnerComplete=False)
    return dict(case=case,
        parent=[desktop_row('owner', 32, PARENT_PID, OWNER_TID),
                desktop_row('controller', 32, PARENT_PID, CONTROLLER_TID), result, proxy],
        peer=[desktop_row('adversary', 64, CHILD_PID, CHILD_TID), peer],
        stderr=stderr_for(case), launch=dict(pid=PARENT_PID,
            creation100ns=PARENT_CREATED, initialThreadId=OWNER_TID),
        desktop=DESKTOP, input=INPUT)


def lines(rows):
    return ''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows)


class FaultReaderTests(unittest.TestCase):
    def read(self, f, *, parent_text=None, peer_text=None):
        return m.check_case(f['case'], lines(f['parent']) if parent_text is None else parent_text,
            lines(f['peer']) if peer_text is None else peer_text, f['stderr'],
            f['launch'], f['desktop'], f['input'])

    def refused(self, f, **kwargs):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            self.read(f, **kwargs)

    def test_five_independent_complete_transcripts(self):
        self.assertEqual(tuple(m.CASES), CASES)
        for case in CASES:
            with self.subTest(case=case):
                result = self.read(fixture(case))
                self.assertEqual(result['result']['barrierHresult'], LOST)
                self.assertFalse(result['result']['gpuTested'])
                self.assertFalse(result['result']['publicComTested'])
                self.assertEqual(result['proxy']['childExit'], 259)

    def test_unknown_or_cross_case_not_accepted(self):
        f = fixture(); f['case'] = 'invented'; self.refused(f)
        for stream, index in (('parent', 2), ('peer', 1)):
            f = fixture(); f[stream][index]['case'] = 'nonce'; self.refused(f)

    def test_exact_populations_and_order(self):
        for stream in ('parent', 'peer'):
            for edit in ('missing', 'duplicate', 'reorder', 'unknown'):
                with self.subTest(stream=stream, edit=edit):
                    f = fixture(); rows = f[stream]
                    if edit == 'missing': rows.pop()
                    elif edit == 'duplicate': rows.append(copy.deepcopy(rows[-1]))
                    elif edit == 'reorder': rows.reverse()
                    else: rows.append({'schema': 'unrelated/v1'})
                    self.refused(f)

    def test_no_unrecognized_fields_on_any_record(self):
        for stream, count in (('parent', 4), ('peer', 2)):
            for index in range(count):
                with self.subTest(stream=stream, index=index):
                    f = fixture(); f[stream][index]['unrecognizedEvidence'] = True
                    self.refused(f)

    def test_exact_required_fields_on_result_peer_and_proxy(self):
        for stream, index in (('parent', 2), ('parent', 3), ('peer', 1)):
            for field in fixture()[stream][index]:
                with self.subTest(stream=stream, index=index, field=field):
                    f = fixture(); del f[stream][index][field]; self.refused(f)

    def test_duplicate_json_keys_and_malformed_tail(self):
        f = fixture(); text = lines(f['parent'])
        duplicate = text.replace('"schema":"warvk-x1a-fault-call/v1"',
            '"schema":"decoy/v1","schema":"warvk-x1a-fault-call/v1"')
        self.refused(f, parent_text=duplicate)
        for bad in (text.rstrip('\n'), text + 'not-json\n', text + '{\n'):
            self.refused(f, parent_text=bad)
        self.refused(f, peer_text=lines(f['peer']).rstrip('\n'))

    def test_parent_creation_must_match_actual_launch_not_just_each_other(self):
        f = fixture()
        f['parent'][2]['creation'] += 1
        f['peer'][1]['parentCreation'] += 1
        self.refused(f)

    def test_exact_process_creation_and_nonce_bindings(self):
        targets = (('parent', 2, 'pid'), ('parent', 2, 'childPid'),
            ('parent', 2, 'childCreation'), ('parent', 2, 'nonceLow'),
            ('parent', 2, 'nonceHigh'), ('parent', 3, 'childPid'),
            ('parent', 3, 'childCreation'), ('peer', 1, 'pid'),
            ('peer', 1, 'creation'), ('peer', 1, 'parentPid'),
            ('peer', 1, 'parentCreation'), ('peer', 1, 'nonceLow'),
            ('peer', 1, 'nonceHigh'))
        for stream, index, key in targets:
            with self.subTest(stream=stream, key=key):
                f = fixture(); f[stream][index][key] += 1; self.refused(f)

    def test_child_pid_cannot_exceed_dword_even_if_every_record_agrees(self):
        f = fixture(); bad = 2**32 + 202
        f['peer'][0]['pid'] = f['peer'][1]['pid'] = bad
        f['parent'][2]['childPid'] = f['parent'][3]['childPid'] = bad
        f['stderr'] = f['stderr'].replace('pid=202 ', f'pid={bad} ')
        self.refused(f)

    def test_nonce_allows_one_zero_half_but_not_an_empty_scope(self):
        for key in ('nonceLow', 'nonceHigh'):
            f = fixture(); f['parent'][2][key] = f['peer'][1][key] = 0
            self.read(f)
        f = fixture()
        for key in ('nonceLow', 'nonceHigh'):
            f['parent'][2][key] = f['peer'][1][key] = 0
        self.refused(f)

    def test_exact_integer_types_on_identity_fields(self):
        targets = (('parent', 2, 'childPid'), ('parent', 2, 'childCreation'),
            ('parent', 2, 'creation'), ('parent', 3, 'childPid'),
            ('parent', 3, 'childCreation'), ('peer', 1, 'parentPid'),
            ('peer', 1, 'parentCreation'), ('peer', 1, 'creation'))
        for stream, index, key in targets:
            for convert in (float, str):
                with self.subTest(stream=stream, key=key, convert=convert.__name__):
                    f = fixture(); f[stream][index][key] = convert(f[stream][index][key])
                    self.refused(f)

    def test_owning_and_controller_threads_must_be_real_and_distinct(self):
        for stream, index, key, value in (('parent', 0, 'tid', CONTROLLER_TID),
                ('parent', 1, 'tid', OWNER_TID), ('peer', 0, 'tid', 0),
                ('parent', 0, 'bits', True), ('peer', 0, 'bits', 32),
                ('parent', 1, 'pid', CHILD_PID)):
            with self.subTest(stream=stream, index=index, key=key):
                f = fixture(); f[stream][index][key] = value; self.refused(f)

    def test_noninput_desktop_and_station_not_spoofed(self):
        for stream, index in (('parent', 0), ('parent', 1), ('peer', 0)):
            for key, value in (('desktop', INPUT), ('inputDesktop', DESKTOP),
                    ('station', 'Other'), ('isolatedDesktop', 'WinSta0\\Other'),
                    ('vulkanAbsent', False)):
                with self.subTest(stream=stream, index=index, key=key):
                    f = fixture(); f[stream][index][key] = value; self.refused(f)
        f = fixture(); f['desktop'] = 'dEfAuLt'; self.refused(f)

    def test_guard_and_exact_pending_prefix_are_mandatory(self):
        for key in ('published', 'acknowledged', 'queued', 'lastPublished', 'done',
                'guardBytes', 'resultBytes', 'barrierHresult', 'asyncFailureSequence',
                'asyncFailureResult'):
            with self.subTest(key=key):
                f = fixture(); f['parent'][2][key] += 1; self.refused(f)
                f = fixture(); f['parent'][2][key] = float(f['parent'][2][key]); self.refused(f)
        for key in ('pendingCpuAck', 'faulted', 'guardUnchanged', 'waitingHandshake',
                'subsequentClearRefused', 'ipcTested'):
            for value in (False, 1):
                f = fixture(); f['parent'][2][key] = value; self.refused(f)

    def test_decode_fault_does_not_claim_clear_reply_failure_identity(self):
        for case in ('nonce', 'peer-exit', 'partial-header'):
            f = fixture(case); f['parent'][2]['asyncFailureSequence'] = 2; self.refused(f)
            f = fixture(case); f['parent'][2]['asyncFailureResult'] = LOST; self.refused(f)

    def test_cpu_ipc_must_not_be_promoted_to_com_gpu_or_product(self):
        for stream, index, fields in (('parent', 2, ('gpuTested', 'publicComTested', 'productOwnerComplete')),
                ('peer', 1, ('gpuTested', 'productOwnerComplete')),
                ('parent', 3, ('productIntegrated', 'memorySavingsProven', 'deviceLost', 'remoteTerminal'))):
            for field in fields:
                with self.subTest(stream=stream, field=field):
                    f = fixture(); f[stream][index][field] = True; self.refused(f)

    def test_peer_exit_cannot_be_satisfied_by_an_unrelated_header_timeout(self):
        f = fixture('peer-exit')
        f['stderr'] = f['stderr'].replace('peer-exited stage=header', 'timeout stage=header')
        self.refused(f)

    def test_peer_exit_rejects_unrelated_system_error(self):
        f = fixture('peer-exit')
        f['stderr'] = f['stderr'].replace('peer-exited stage=header win32=0',
            'system stage=header win32=5')
        self.refused(f)

    def test_peer_exit_accepts_only_the_observed_transport_close_family(self):
        for why in ('end-of-stream stage=header win32=0',
                    'system stage=header win32=109', 'system stage=header win32=232'):
            with self.subTest(why=why):
                f = fixture('peer-exit')
                f['stderr'] = f['stderr'].replace('peer-exited stage=header win32=0', why)
                self.read(f)

    def test_partial_header_requires_the_actual_header_timeout_and_elapsed_window(self):
        for elapsed in (True, -1, 0, 4499, 15000, 5000.0):
            with self.subTest(elapsed=elapsed):
                f = fixture('partial-header'); f['parent'][2]['barrierElapsedMs'] = elapsed
                self.refused(f)
        f = fixture('partial-header')
        f['stderr'] = f['stderr'].replace('timeout stage=header', 'timeout stage=body')
        self.refused(f)

    def test_failure_must_be_unique_primary_not_only_a_matching_worker_substring(self):
        for case in CASES:
            f = fixture(case); primary = f['stderr'].splitlines(keepends=True)[0]
            f['stderr'] = f['stderr'].replace(primary, '')
            self.refused(f)
            f = fixture(case); f['stderr'] = primary + f['stderr']; self.refused(f)

    def test_worker_and_primary_reason_must_agree(self):
        f = fixture('sfalse')
        f['stderr'] = f['stderr'].replace('D3D9 dev worker: D3D9 asynchronous/init reply contract',
            'D3D9 dev worker: D3D9 reply identity')
        self.refused(f)

    def test_unrecognized_or_contradictory_stderr_never_hidden(self):
        for suffix in ('D3D9 dev primary failure=other primaryExitWaitMs=0\n',
                'fault Connection fixture failure: unexpected\n', 'unparsed tail\n'):
            f = fixture(); f['stderr'] += suffix; self.refused(f)

    def test_secondary_cleanup_exact_identity_count_exit_and_bound(self):
        f = fixture(); drain = f['stderr'].splitlines(keepends=True)[1]
        for replacement in ('', drain + drain, drain.replace('pid=202', 'pid=203'),
                drain.replace('exit=0', 'exit=4'), drain.replace('elapsedMs=3', 'elapsedMs=5000'),
                drain.replace('originalPreserved=true', 'originalPreserved=false')):
            with self.subTest(replacement=replacement):
                f = fixture(); f['stderr'] = f['stderr'].replace(drain, replacement)
                self.refused(f)

    def test_proxy_primary_exit_is_unreached_not_upgraded_by_cleanup(self):
        for key, value in (('childExit', 0), ('childExit', 259.0), ('primaryExitWaitMs', 1),
                ('primaryExitWaitMs', False), ('exchanges', True), ('exchanges', 1.0),
                ('childProcessSettled', True), ('childNaturalExit', True), ('childSelfFailStop', True)):
            with self.subTest(key=key, value=value):
                f = fixture(); f['parent'][3][key] = value; self.refused(f)

    def test_peer_handshake_and_fault_mode_cannot_be_relabelled(self):
        for case in CASES:
            for key in ('readbackHandshake', 'peerVerified', 'endOfStreamObserved', 'intentionalExit'):
                f = fixture(case); f['peer'][1][key] = not f['peer'][1][key]; self.refused(f)
            for key in ('requests', 'lastSequence'):
                f = fixture(case); f['peer'][1][key] = 3; self.refused(f)

    def test_async_summary_required_once_and_not_truncated(self):
        f = fixture(); line = f['stderr'].splitlines(keepends=True)[-1]
        for replacement in ('', line + line, line.rstrip('\n'), 'D3D9 async-clear: {\n'):
            f = fixture(); f['stderr'] = f['stderr'].replace(line, replacement); self.refused(f)

    def test_async_summary_matches_actual_prefix_and_failure(self):
        values = dict(initializedAcknowledged=False, clearPublished=2, clearRemoteSucceeded=1,
            fullObservations=1, backpressureCalls=1, backpressureMs=1, maxBackpressureMs=1,
            maxObservedOutstanding=5, lastPublished=4, lastReplySettled=3,
            asyncFailureSequence=0, asyncFailureResult=0, queueCapacity=8,
            workerPrivateBatchCapacity=2, admissionDeadlineMs=6000, callDeadlineMs=35000,
            replyUsesRemainingCallBudget=False, transportDeadlineMs=10000,
            primaryExitDeadlineMs=6000, gpuCompletionInferredFromAdmission=True,
            productOwnerComplete=True)
        for key, value in values.items():
            with self.subTest(key=key):
                f = fixture(); diag = diagnostic('sfalse'); diag[key] = value
                f['stderr'] = stderr_for('sfalse', diag); self.refused(f)

    def test_async_summary_has_strict_types_keys_and_schema(self):
        for key, value in (('clearPublished', True), ('lastReplySettled', 1.0),
                ('gpuCompletionInferredFromAdmission', 0), ('schema', 'decoy/v1'),
                ('unrecognizedEvidence', True)):
            with self.subTest(key=key):
                f = fixture(); diag = diagnostic('sfalse'); diag[key] = value
                f['stderr'] = stderr_for('sfalse', diag); self.refused(f)

    def test_actual_parent_uses_real_constructor_and_worker_not_synthetic_replies(self):
        source = (Path(__file__).parent/'test_d3d9_connection_fault_ipc.cpp').read_text('utf-8')
        parent = source.split('#else\nstatic_assert(sizeof(void*)==4', 1)[-1]
        self.assertIn('d3d9proxy::Connection c(true)', parent)
        self.assertIn('c.call(read,nullptr,guard.data()+8,16)', parent)
        self.assertIn('controller.join()', parent)
        self.assertNotIn('PreparedOnly', parent)
        self.assertNotIn('acceptReply(', parent)
        self.assertNotIn('EncodeReply(', parent)
        self.assertNotIn('replyPacket', parent)

    def test_async_draw_batch_cannot_be_certified_as_clear_or_success(self):
        f=fixture();rows=f['stderr'].splitlines()
        diag=diagnostic('sfalse');diag['clearPublished']=0
        rows[3]='D3D9 async-clear: '+json.dumps(diag)
        batch=dict(schema='warvk-d3d9-draw-batch-proxy/v1',enabled=True,admittedDraws=32,
            flushedBatches=1,pendingDraws=0,capacity=32,payloadCapacityBytes=37248,
            asyncAdmission=True,remoteSucceededBatches=0,asyncEvents=False,eventWaitsPublished=0,eventWaitsSucceeded=0,
            eventPendingObservations=0,eventLocalCompletions=0,eventGpuCompleted=0,gpuCompletionInferredFromAdmission=False)
        rows[4]='D3D9 draw-batch-proxy: '+json.dumps(batch)
        m.check_stderr('sfalse','\n'.join(rows)+'\n',f['parent'][2],f['parent'][3],True)
        with self.assertRaises(ValueError):
            m.check_stderr('sfalse','\n'.join(rows)+'\n',f['parent'][2],f['parent'][3],False)
        for key,value in (('admittedDraws',0),('remoteSucceededBatches',1),('asyncAdmission',False),('flushedBatches',True)):
            bad=rows.copy();bad[4]='D3D9 draw-batch-proxy: '+json.dumps(batch|{key:value})
            with self.subTest(key=key),self.assertRaises(ValueError):
                m.check_stderr('sfalse','\n'.join(bad)+'\n',f['parent'][2],f['parent'][3],True)

    def test_async_event_failure_cannot_publish_gpu_completion(self):
        f=fixture('sfalse');rows=stderr_for('sfalse').splitlines()
        diag=diagnostic('sfalse');diag['clearPublished']=0
        rows[3]='D3D9 async-clear: '+json.dumps(diag)
        batch=dict(schema='warvk-d3d9-draw-batch-proxy/v1',enabled=True,admittedDraws=0,
            flushedBatches=0,pendingDraws=0,capacity=32,payloadCapacityBytes=37248,
            asyncAdmission=True,remoteSucceededBatches=0,asyncEvents=True,eventWaitsPublished=1,
            eventWaitsSucceeded=0,eventPendingObservations=0,eventLocalCompletions=0,eventGpuCompleted=0,
            gpuCompletionInferredFromAdmission=False)
        rows[4]='D3D9 draw-batch-proxy: '+json.dumps(batch)
        m.check_stderr('sfalse','\n'.join(rows)+'\n',f['parent'][2],f['parent'][3],False,True)
        for key,value in (('eventGpuCompleted',9),('eventWaitsSucceeded',1),('eventWaitsPublished',True),
                          ('eventLocalCompletions',1),('asyncEvents',False)):
            bad=rows.copy();bad[4]='D3D9 draw-batch-proxy: '+json.dumps(batch|{key:value})
            with self.subTest(key=key),self.assertRaises(ValueError):
                m.check_stderr('sfalse','\n'.join(bad)+'\n',f['parent'][2],f['parent'][3],False,True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
