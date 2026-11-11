"""X1a real x86 Connection/worker -> authenticated x64 adversarial CPU peer.

Native launches only through restricted-Medium non-input Win32 Desktop. No GPU,
public COM, game, or product-asynchronous-owner acceptance is inferred.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_core_gate import execute
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_presenter_gate import check_launch
from run_render_host_d3d9_gate import native_uint
from isolated_render_desktop import IsolatedRenderDesktop
from diagnose_render_host_initialization import ENV_ALLOW
from render_lab_raw_artifacts import capture_raw_artifacts, raw_manifests, verify_raw_artifacts

COMPILERS = Path('E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/llvm-mingw-20260616-ucrt-x86_64/bin')
CASES = ('sfalse', 'payload', 'nonce', 'peer-exit', 'partial-header')


def json_rows(text):
    require(type(text) is str and text.endswith('\n'), 'complete raw result text')
    rows = []
    for line in text.splitlines():
        if not line.strip():
            continue
        require(line.startswith('{'), 'unexpected stdout record')
        row = strict_json(line)
        require(type(row) is dict and type(row.get('schema')) is str, 'typed schema')
        rows.append(row)
    return rows


def exact_fields(row, fields, label):
    require(type(row) is dict and set(row) == set(fields), 'exact '+label+' fields')


def exact_values(row, expected, label):
    for key, value in expected.items():
        require(type(row[key]) is type(value) and row[key] == value,
                'exact '+label+' '+key)


def identities(row, pids=(), creations=()):
    for key in pids:
        require(native_uint(row[key], 32) and row[key] > 0, 'exact DWORD identity '+key)
    for key in creations:
        require(native_uint(row[key]) and row[key] > 0, 'exact creation identity '+key)


def check_stderr(case, text, result, proxy, draw_batch=False, event_wait=False):
    """Classify complete primary/secondary evidence, never substring success.

    The failed call returns through the existing unhealthy branch without a
    caller-side exception log. The worker prints primary -> drain -> original
    failure; the owner prints its cold async summary only after worker join.
    """
    require(type(text) is str and text.endswith('\n'), 'complete fault stderr required')
    rows = text.splitlines()
    require(len(rows) == 5, 'exact primary/drain/worker/async/batch stderr population')
    primary_prefix = 'D3D9 dev primary failure='
    worker_prefix = 'D3D9 dev worker: '
    require(rows[0].startswith(primary_prefix) and rows[2].startswith(worker_prefix),
            'primary and original worker failure records required')
    reason = rows[0][len(primary_prefix):]
    require(rows[2][len(worker_prefix):] == reason, 'worker must preserve the primary cause')
    if case in ('sfalse', 'payload'):
        require(reason == 'D3D9 asynchronous/init reply contract primaryExitWaitMs=0',
                'actual asynchronous reply contract failure required')
    elif case == 'nonce':
        require(reason == 'D3D9 reply identity primaryExitWaitMs=0',
                'actual reply identity failure required')
    elif case == 'partial-header':
        # Both timeout construction sites in the actual transport use the
        # Win32 WAIT_TIMEOUT value, not a fabricated zero error code.
        require(reason == 'timeout stage=header win32=258 primaryExitWaitMs=0'
                and result['barrierElapsedMs'] >= 4500, 'actual original header timeout')
    else:
        require(case == 'peer-exit' and reason in {
            'peer-exited stage=header win32=0 primaryExitWaitMs=0',
            'end-of-stream stage=header win32=0 primaryExitWaitMs=0',
            'system stage=header win32=109 primaryExitWaitMs=0',
            'system stage=header win32=232 primaryExitWaitMs=0'},
            'peer exit must be process exit/EOF/broken pipe, never timeout or unrelated error')
    drain = re.fullmatch(r'D3D9 dev secondary-exit-drain pid=([1-9][0-9]*) exit=0 '
                         r'elapsedMs=(0|[1-9][0-9]*) originalPreserved=true', rows[1])
    require(drain is not None and int(drain[1]) == proxy['childPid'] and int(drain[2]) < 5000,
            'exact bounded secondary cleanup')
    prefix = 'D3D9 async-clear: '
    require(rows[3].startswith(prefix), 'one cold async-clear diagnostic required')
    async_clear = strict_json(rows[3][len(prefix):])
    expected = dict(schema='warvk-d3d9-async-clear/v1', initializedAcknowledged=True,
        clearPublished=0 if draw_batch or event_wait else 1, clearRemoteSucceeded=0, fullObservations=0, backpressureCalls=0,
        backpressureMs=0, maxBackpressureMs=0, maxObservedOutstanding=2,
        lastPublished=3, lastReplySettled=1,
        asyncFailureSequence=2 if case in ('sfalse', 'payload') else 0,
        asyncFailureResult=1 if case == 'sfalse' else 0,
        queueCapacity=4, workerPrivateBatchCapacity=1, admissionDeadlineMs=5000,
        callDeadlineMs=30000, replyUsesRemainingCallBudget=True,
        transportDeadlineMs=5000, primaryExitDeadlineMs=5000,
        gpuCompletionInferredFromAdmission=False, productOwnerComplete=False)
    exact_fields(async_clear, expected, 'async-clear')
    exact_values(async_clear, expected, 'async-clear')
    prefix='D3D9 draw-batch-proxy: '
    require(rows[4].startswith(prefix),'one cold draw-batch diagnostic required')
    batch=strict_json(rows[4][len(prefix):])
    expected_batch=dict(schema='warvk-d3d9-draw-batch-proxy/v1',enabled=draw_batch or event_wait,
        admittedDraws=32 if draw_batch else 0,flushedBatches=int(draw_batch),pendingDraws=0,
        capacity=32,payloadCapacityBytes=37248,asyncAdmission=draw_batch or event_wait,
        remoteSucceededBatches=0,asyncEvents=event_wait,eventWaitsPublished=int(event_wait),eventWaitsSucceeded=0,
        eventPendingObservations=0,eventLocalCompletions=0,eventGpuCompleted=0,gpuCompletionInferredFromAdmission=False)
    exact_fields(batch,expected_batch,'draw-batch');exact_values(batch,expected_batch,'draw-batch')
    return dict(primaryFailure=reason, secondaryExitWaitMs=int(drain[2]), asyncClear=async_clear,drawBatch=batch)


def check_case(case, parent_text, peer_text, stderr, launch, desktop, input_desktop, draw_batch=False, event_wait=False):
    require(type(case) is str and case in CASES and type(desktop) is str
            and type(input_desktop) is str and desktop and input_desktop
            and desktop.casefold() != input_desktop.casefold()
            and desktop.casefold() != 'default', 'actual non-input case')
    identities(launch, ('pid', 'initialThreadId'), ('creation100ns',))
    parent = json_rows(parent_text); peer = json_rows(peer_text)
    require([r['schema'] for r in parent] == ['warvk-x1a-fault-desktop/v1',
        'warvk-x1a-fault-desktop/v1', 'warvk-x1a-fault-call/v1', 'warvk-d3d9-proxy/v1'], 'exact parent population')
    require([r['schema'] for r in peer] == ['warvk-x1a-fault-desktop/v1',
        'warvk-x1a-fault-peer/v1'], 'exact adversary population')
    owner, controller, result, proxy = parent; worker, adversary = peer
    exact_fields(result, {'schema', 'case', 'pid', 'creation', 'childPid', 'childCreation',
        'nonceLow', 'nonceHigh', 'published', 'acknowledged', 'queued', 'pendingCpuAck',
        'faulted', 'lastPublished', 'done', 'asyncFailureSequence', 'asyncFailureResult',
        'barrierHresult', 'guardUnchanged', 'guardBytes', 'resultBytes', 'waitingHandshake',
        'subsequentClearRefused', 'barrierElapsedMs', 'ipcTested', 'gpuTested',
        'publicComTested', 'productOwnerComplete'}, 'fault-call')
    exact_fields(adversary, {'schema', 'case', 'pid', 'creation', 'parentPid', 'parentCreation',
        'nonceLow', 'nonceHigh', 'requests', 'lastSequence', 'readbackHandshake',
        'endOfStreamObserved', 'intentionalExit', 'peerVerified', 'gpuTested',
        'productOwnerComplete'}, 'fault-peer')
    exact_fields(proxy, {'schema', 'childPid', 'childCreation', 'childExit', 'exchanges',
        'peerVerified', 'transportFailed', 'x86VulkanAbsent', 'deviceLost', 'remoteTerminal',
        'childNaturalExit', 'childProcessSettled', 'childSelfFailStop', 'primaryExitWaitMs',
        'productIntegrated', 'memorySavingsProven'}, 'proxy')
    identities(result, ('pid', 'childPid'), ('creation', 'childCreation'))
    identities(adversary, ('pid', 'parentPid'), ('creation', 'parentCreation'))
    identities(proxy, ('childPid',), ('childCreation',))
    for row, role, bits, pid in ((owner, 'owner', 32, launch['pid']),
                                (controller, 'controller', 32, launch['pid']),
                                (worker, 'adversary', 64, proxy['childPid'])):
        exact_fields(row, {'schema', 'role', 'pid', 'tid', 'bits', 'desktop',
            'inputDesktop', 'station', 'isolatedDesktop', 'vulkanAbsent'}, 'desktop')
        identities(row, ('pid', 'tid'))
        require(row['role'] == role and type(row['bits']) is int and row['bits'] == bits
                and row['pid'] == pid, 'role/width/process/thread identity')
        require(row['desktop'] == desktop and row['inputDesktop'] == input_desktop
                and row['station'] == 'WinSta0' and row['isolatedDesktop'] == 'WinSta0\\'+desktop
                and row['vulkanAbsent'] is True, 'actual Desktop and no GPU module')
    require(owner['tid'] == launch['initialThreadId'] and controller['tid'] != owner['tid'], 'actual controller thread')
    for row in (result, adversary):
        require(row['case'] == case and row['gpuTested'] is False
                and row['productOwnerComplete'] is False, 'scope cannot become GPU/product proof')
        require(native_uint(row['nonceLow']) and native_uint(row['nonceHigh'])
                and (row['nonceLow'] or row['nonceHigh']), 'nonempty exact nonce')
    for key, expected in dict(published=3,acknowledged=1,queued=1,lastPublished=3,done=1,
            guardBytes=32,resultBytes=16,barrierHresult=0x88760868,
            asyncFailureSequence=2 if case in ('sfalse','payload') else 0,
            asyncFailureResult=1 if case == 'sfalse' else 0).items():
        require(type(result[key]) is int and result[key] == expected, 'actual fault result '+key)
    for key in ('pendingCpuAck','faulted','guardUnchanged','waitingHandshake','subsequentClearRefused','ipcTested'):
        require(result[key] is True, 'fault evidence missing '+key)
    require(result['publicComTested'] is False and result['pid'] == launch['pid']
            and result['creation'] == launch['creation100ns'], 'only actual launcher-bound Connection IPC')
    require(native_uint(result['barrierElapsedMs']) and result['barrierElapsedMs'] < 15000, 'bounded barrier')
    require(adversary['pid'] == proxy['childPid'] == result['childPid']
            and adversary['creation'] == proxy['childCreation'] == result['childCreation']
            and adversary['parentPid'] == result['pid'] and adversary['parentCreation'] == result['creation']
            and adversary['creation'] >= adversary['parentCreation']
            and adversary['nonceLow'] == result['nonceLow'] and adversary['nonceHigh'] == result['nonceHigh'], 'authenticated identity binding')
    require(adversary['requests'] == 2 and type(adversary['requests']) is int
            and adversary['lastSequence'] == 2 and type(adversary['lastSequence']) is int
            and adversary['readbackHandshake'] is True and adversary['peerVerified'] is True,
            'exact actual peer transcript')
    require(adversary['endOfStreamObserved'] is (case != 'peer-exit')
            and adversary['intentionalExit'] is (case == 'peer-exit'), 'reply fault versus exact child exit')
    exact_values(proxy, dict(exchanges=1, childExit=259, primaryExitWaitMs=0), 'proxy primary phase')
    require(proxy['peerVerified'] is True and proxy['transportFailed'] is True
            and proxy['deviceLost'] is False and proxy['remoteTerminal'] is False
            and proxy['childProcessSettled'] is False and proxy['childNaturalExit'] is False
            and proxy['childSelfFailStop'] is False and proxy['x86VulkanAbsent'] is True
            and proxy['productIntegrated'] is False and proxy['memorySavingsProven'] is False,
            'expected transport fault is not normal primary exit or GPU DEVICELOST')
    evidence = check_stderr(case, stderr, result, proxy, draw_batch, event_wait)
    return dict(result=result, adversary=adversary, proxy=proxy, **evidence)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--draw-batch',action='store_true',help='fault the actual asynchronously admitted 32-draw packet')
    ap.add_argument('--event-wait',action='store_true',help='fault the actual asynchronous event wait before GPU completion')
    args = ap.parse_args(); out = args.output.resolve()
    require(not(args.draw_batch and args.event_wait),'choose exactly one asynchronous command type')
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh project artifact output')
    out.mkdir()
    receipt = dict(schema='warvk-x1a-fault-ipc-gate/v1',ok=False,steps=[],stepRawArtifacts=[],cases=[],
        ipcAttempted=False,ipcTested=False,gpuTested=False,publicComTested=False,productOwnerComplete=False,gameTouched=False,
        priority='BelowNormal',originalTransportMs=5000,originalPrimaryExitMs=5000,originalCallMs=30000)
    receipt['drawBatchAdmission']=args.draw_batch
    receipt['eventWaitAdmission']=args.event_wait
    frozen=[]; env={k.upper():v for k,v in os.environ.items() if k.upper() in ENV_ALLOW}
    try:
        sources=set((ROOT/'tools/render_host').rglob('*.h'))
        common=[ROOT/'tools/render_host'/name for name in ('protocol.cpp','win32_transport.cpp','win32_security.cpp','owned_child.cpp')]
        sources.update(common); source=ROOT/'AutoTest/test_d3d9_connection_fault_ipc.cpp';sources.add(source)
        sources.add(ROOT/'AutoTest/test_d3d9_connection_fault_ipc_reader.py')
        sources.update(Path(m.__file__).resolve() for m in list(sys.modules.values()) if getattr(m,'__file__',None)
            and Path(m.__file__).suffix=='.py' and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen=[identity(p) for p in sorted(sources)];receipt['sources']=list(frozen)
        for bits,arch in ((64,'x86_64'),(32,'i686')):
            compiler=COMPILERS/(arch+'-w64-mingw32-clang++.exe');frozen.append(identity(compiler))
            exe=out/f'fault-{bits}.exe'; stem=f'build-{bits}'
            argv=[compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-pthread','-municode',
                  '-I'+str(ROOT/'tools/render_host'),source,*common,'-lbcrypt','-ladvapi32','-luser32','-o',exe]
            if bits==64:argv.insert(1,'-DWARVK_X1A_FAULT_PEER=1')
            if args.draw_batch:argv.insert(1,'-DWARVK_X1A_FAULT_BATCH=1')
            if args.event_wait:argv.insert(1,'-DWARVK_X1A_FAULT_EVENT=1')
            try:receipt['steps'].append(execute(argv,ROOT,out/stem,120,env))
            finally:receipt['stepRawArtifacts'].append(capture_raw_artifacts(out,[stem+'.stdout',stem+'.stderr']))
            require(pe_identity(exe)==((0x14c,0x10b) if bits==32 else (0x8664,0x20b)),'exact architecture')
            frozen.append(identity(exe))
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety']=desktop.receipt
            for case in CASES:
                directory=out/case;directory.mkdir();row=dict(case=case,ok=False);receipt['cases'].append(row)
                controls=dict(WARVK_X1A_FAULT_CASE=case,WARVK_X1A_FAULT_TOKEN=secrets.token_hex(16),
                    WARVK_D3D9_HOST_EXE=str(out/'fault-64.exe'),WARVK_D3D9_HOST_LOG=str(directory/'peer.log'),
                    WARVK_X1A_EXPECT_DESKTOP=desktop.desktop_name,WARVK_X1A_EXPECT_INPUT=desktop.input_before)
                if args.draw_batch or args.event_wait:controls.update(WARVK_D3D9_BATCH_DRAWS='1',WARVK_D3D9_ASYNC_BATCH='1')
                if args.event_wait:controls['WARVK_D3D9_ASYNC_EVENTS']='1'
                row['requestedControls']=controls
                local={**env,**controls}
                require({k:v for k,v in local.items() if k.startswith('WARVK_')}==controls,
                        'only the exact requested development controls may be inherited')
                row['actualSafeEnvironment']=local
                receipt['ipcAttempted']=True
                try:row['process']=desktop.run([out/'fault-32.exe'],cwd=directory,
                    stdout_path=directory/'case.stdout',stderr_path=directory/'case.stderr',timeout_s=30,env=local)
                finally:row['initialRawArtifacts']=capture_raw_artifacts(directory,['case.stdout','case.stderr','peer.log'],True)
                check_launch(row['process'],desktop.desktop_name,desktop.input_before)
                for name in ('case.stdout','case.stderr','peer.log'):
                    require(0<(directory/name).stat().st_size<1024*1024,'bounded complete log '+name)
                row.update(check_case(case,*( (directory/name).read_text('utf-8') for name in ('case.stdout','peer.log','case.stderr')),
                    row['process'],desktop.desktop_name,desktop.input_before,args.draw_batch,args.event_wait))
                receipt['ipcTested']=True
                row['childSettlement']=child_settled(row['proxy']['childPid'],row['proxy']['childCreation']);row['ok']=True
        require(receipt['desktopSafety']['ok'] is True,'isolated Desktop cleanup')
        receipt['ok']=len(receipt['cases'])==len(CASES) and all(row['ok'] for row in receipt['cases'])
    except Exception as error:receipt['error']=str(error)
    finally:
        try:
            require(all(identity(Path(r['path']))==r for r in frozen),'source/binary identity drift')
            receipt['preserved']=frozen;receipt['inputsPreserved']=bool(frozen)
            for raw in raw_manifests(receipt):verify_raw_artifacts(raw)
        except Exception as error:receipt['ok']=False;receipt['preservationError']=str(error)
        with (out/'receipt.json').open('x',encoding='utf-8') as stream:json.dump(receipt,stream,indent=2)
    print(json.dumps({k:receipt.get(k) for k in ('ok','error','preservationError','inputsPreserved')}))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':raise SystemExit(main())
