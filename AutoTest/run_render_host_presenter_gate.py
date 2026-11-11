"""Real D3D9 -> existing DXVK Presenter, only on a measured non-input desktop.

The old offscreen oracle is retained, not weakened into accepting WSI claims.
No Warcraft, player deployment, input injection, release, or memory-benefit claim.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, child_settled
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import sources, json_lines, check_fixture, check_remote, native_uint
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests


def check_case_records(reports,host_reports,fixture_schema,host_schema=None,host_desktop=False,init_cases=False):
    """Require complete per-case record populations before selecting any row.

    Summary schemas are singletons. Initialization rejection events are the
    sole repeatable schema: their explicit index, not their repeated case name,
    must form exactly 1..invalidCallChecks. No unrelated records are ignored.
    """
    require(not host_desktop or host_schema is not None,'desktop without host receipt')
    require(not init_cases or fixture_schema=='warvk-d3d9-init/v1','initialization event scope')
    expected_reports={fixture_schema}
    expected_host=set()
    if host_schema is not None:
        expected_reports.add('warvk-d3d9-proxy/v1')
        expected_host.add(host_schema)
    if host_desktop:expected_host.add('warvk-d3d9-desktop/v1')
    summaries=[];events=[]
    for stream,(records,expected) in enumerate(((reports,expected_reports),(host_reports,expected_host))):
        require(type(records) is list,'case record stream must be a list')
        rows={}
        for row in records:
            require(type(row) is dict and type(row.get('schema')) is str,'case record schema required')
            schema=row['schema']
            if stream==0 and init_cases and schema=='warvk-d3d9-init-case/v1':
                events.append(row)
                continue
            require(schema in expected,'unexpected case record schema '+schema)
            require(schema not in rows,'duplicate or conflicting case record schema '+schema)
            rows[schema]=row
        require(set(rows)==expected,'missing case summary schema')
        summaries.append(rows)
    if init_cases:
        count=summaries[0][fixture_schema]['invalidCallChecks']
        require(native_uint(count,32) and len(events)==count,'initialization event population')
        for index,row in enumerate(events,1):
            require(native_uint(row['index'],32) and row['index']==index,
                    'duplicate, missing or reordered initialization event index')
            require(type(row['case']) is str and bool(row['case']),'initialization event case')
            require(native_uint(row['result'],32) and row['result']==0x8876086c
                    and row['transportHealthy'] is True,'initialization event did not refuse cleanly')
    return tuple(summaries)


def check_case_record_mutations(reports,host_reports,fixture_schema,host_schema=None,host_desktop=False,init_cases=False):
    """Append duplicate/conflicting rows to each actual passing runtime list."""
    scope=(fixture_schema,host_schema,host_desktop,init_cases)
    check_case_records(reports,host_reports,*scope)
    tested=killed=duplicates=conflicts=0
    for stream,records in enumerate((reports,host_reports)):
        for row in records:
            for conflicting in (False,True):
                lists=copy.deepcopy([reports,host_reports]);extra=copy.deepcopy(row)
                if conflicting:
                    fields=[key for key in ('ok','transportHealthy','beforeGpu','transportFailed')
                            if type(extra.get(key)) is bool]
                    require(bool(fields),'runtime row has no typed verdict to contradict')
                    extra[fields[0]]=not extra[fields[0]]
                    require(extra!=row,'record conflict mutation was a no-op')
                    conflicts+=1
                else:duplicates+=1
                lists[stream].append(extra);tested+=1
                try:check_case_records(*lists,*scope)
                except (ValueError,KeyError,TypeError):killed+=1
    require(tested>0 and killed==tested,'case record duplicate/conflict mutation survived')
    return dict(tested=tested,killed=killed,duplicates=duplicates,conflicts=conflicts,
                source='actual-passing-case-record-lists',gpuRerun=False)


def check_peer_identity(proxy,desktop,windowed=True):
    require(native_uint(proxy['childPid'],32) and proxy['childPid']>0
            and native_uint(proxy['childCreation']) and proxy['childCreation']>0,'exact native child identity')
    for key in ('pid','threadId','peerPid'):
        require(native_uint(desktop[key],32) and desktop[key]>0,'actual desktop identity '+key)
    require(native_uint(desktop['windowThreadId'],32) and
            (desktop['windowThreadId']>0 if windowed else desktop['windowThreadId']==0),'actual window thread identity')


def check_launch(launch,desktop,input_desktop):
    require(type(launch) is dict and launch['ok'] is True,'actual isolated launcher receipt')
    require(type(launch['argv']) is list and len(launch['argv']) > 0
            and type(launch['argv'][0]) is str and bool(launch['argv'][0])
            and type(launch['actualExecutable']) is str and bool(launch['actualExecutable'])
            and Path(launch['actualExecutable']).resolve() == Path(launch['argv'][0]).resolve(),
            'actual process image differs from requested executable')
    require(native_uint(launch['jobActiveProcesses'],32) and launch['jobActiveProcesses']==0,
            'empty Job witness contradicts active process count')
    for key in ('jobAtomic','handleListApplied','processSettled','jobEmpty'):
        require(launch[key] is True,'isolated launch missing '+key)
    for key in ('timedOut','forcedTermination','desktopHandleInherited'):
        require(launch[key] is False,'isolated launch failure '+key)
    require(native_uint(launch['pid'],32) and launch['pid']>0 and native_uint(launch['creation100ns'])
            and launch['creation100ns']>0 and native_uint(launch['initialThreadId'],32)
            and launch['initialThreadId']>0,'isolated native parent identity')
    require(native_uint(launch['exitCode'],32) and launch['exitCode']==0 and launch['cleanupErrors']==[],
            'timeout/kill/native error cannot pass')
    require(launch['desktop']==desktop and launch['inputBefore']==launch['inputAfter']==input_desktop
            and input_desktop!=desktop,'isolated input desktop identity')


def check_wsi(f, proxy, host, desktop, expected_desktop, readback, launch, input_desktop):
    check_peer_identity(proxy,desktop)
    check_launch(launch,expected_desktop,input_desktop)
    require(f['schema']=='warvk-d3d9-wsi-fixture/v1' and f['ok'] is True, 'WSI fixture failed')
    profile='presenter-readback-isolated-v1' if readback else 'presenter-isolated-v1'
    require(f['stage']=='complete' and f['error']=='' and f['profile']==profile, 'WSI scope')
    for field in ('publicD3D9Api','parentVulkanAbsent','profileVerified','nonInputDesktopVerified',
                  'windowShownOnNonInputDesktop','readbackVerified','shaderBytesRoundtripped',
                  'resourceHeldDevice','windowClosed','normalTeardown','queryCompleted'):
        require(f[field] is True,'WSI missing '+field)
    for field in ('reference','gameTouched','globalInputUsed','desktopSwitched','productIntegrated',
                  'memorySavingsProven','windowPixelReadbackAttempted','windowPixelsVerified','playerVisualAccepted'):
        require(f[field] is False,'WSI overclaim '+field)
    exact=dict(bits=32,width=64,height=64,presentationInterval=1,createRejectionChecks=2,
               presentRejectionChecks=3,invalidPresentCalls=3,framesExpected=8,framesStarted=8,framesCompleted=8,
               clearColorVariantsExpected=8 if readback else 2,clearColorVariants=8 if readback else 2,
               drawCalls=24,primitiveCount=48,
               presentApiCallsTotal=11,presentCalls=8,successfulPresents=8,readbackCalls=16,
               successfulReadbacks=16,prePresentReadbacks=8,postPresentReadbacks=8,windowProgressFrames=8,
               windowThreadFailureCode=0,deviceFinalRefs=0,factoryFinalRefs=0,windowPulseCount=16,
               processBoundaryChecks=36 if readback else 28)
    for key,value in exact.items():
        require(native_uint(f[key],32) and f[key]==value,'WSI fixture '+key)
    for key in ('apiThreadId','windowThreadId','desktopChecks','windowMessages','deviceRefsWhileResourceHeld','queryPolls','queryPending'):
        require(native_uint(f[key],32),'WSI integer '+key)
    require(f['queryPolls']==f['queryPending']+8,'query completion per frame')
    require(f['oracleScope']=='D3D9-color-target-before-and-after-Present-not-HWND-pixels','oracle scope')
    require(f['apiThreadId']!=f['windowThreadId'] and f['apiThreadId']>0 and f['windowThreadId']>0,'independent pump')
    require(f['apiDesktopName']==f['windowDesktopName']==expected_desktop,'actual fixture desktops')
    require(f['inputDesktopNameAtStart']==f['inputDesktopNameAtEnd']==f['windowInitialInputDesktopName']
            ==input_desktop,'input desktop unchanged')
    require(f['apiThreadId']==launch['initialThreadId'],'actual parent thread witness')
    require(f['desktopChecks']>=18 and f['windowMessages']>0 and f['deviceRefsWhileResourceHeld']>0,'live ownership witnesses')
    oracle=f['pixelOracle']
    require(all(native_uint(oracle[key],32) for key in ('checked','failures','maxChannelError')),'oracle integer types')
    require(oracle['checked']==40768 and oracle['failures']==0 and oracle['maxChannelError']<=1,'multi-frame oracle')
    require(type(f['frames']) is list and len(f['frames'])==8,'frame count')
    hashes=[]
    for i,frame in enumerate(f['frames']):
        require(all(native_uint(frame[key],32) for key in ('index','drawCalls','clearColorBGRA','presentHresult',
                'windowMessagesBefore','windowMessagesAfter')),'frame integer types')
        require(frame['index']==i and frame['complete'] is True and frame['drawCalls']==3
                and frame['presentHresult']==0 and type(frame['presentHresult']) is int,'strict per-frame Present')
        require(frame['clearColorBGRA']==(0xff101820+i*0x00020304 if readback else
                                        0xff101820 if i%2==0 else 0xff203040),'changing payload')
        require(frame['windowMessagesAfter']>frame['windowMessagesBefore'],'live message pump across Present')
        for name in (('beforeOracle','afterOracle','presentedOracle') if readback else ('beforeOracle','afterOracle')):
            p=frame[name]
            for key,value in dict(checked=2548,clear=1392,opaque=960,blend=196,depthProtected=416,failures=0).items():
                require(native_uint(p[key],32) and p[key]==value,'per-frame oracle '+key)
            require(native_uint(p['maxChannelError'],32) and p['maxChannelError']<=1,'pixel difference')
            require(type(p['imageFnv']) is str and re.fullmatch('[0-9a-f]{16}',p['imageFnv']),'image digest')
            require(all(native_uint(p['firstFailure'][key],32) and p['firstFailure'][key]==0
                for key in ('x','y','actualBGRA','expectedBGRA')),'pixel error diagnostic')
        require(frame['beforeOracle']['imageFnv']==frame['afterOracle']['imageFnv'],'Present corrupted backbuffer')
        if readback:
            require(native_uint(frame['presentedFrame']) and frame['presentedFrame']==i+1,
                    'presented image belongs to a stale/different frame')
            require(native_uint(frame['presentedReadbackHresult'],32) and frame['presentedReadbackHresult']==0,
                    'actual swapchain readback failed')
        hashes.append(frame['beforeOracle']['imageFnv'])
    require(len(set(hashes))==(8 if readback else 2) and
            (readback or all(hashes[i]==hashes[i%2] for i in range(8))),'actual image sequence')
    if readback:
        require(f['presentedImageReadbackEnabled'] is True and f['presentedImagePixelsVerified'] is True,
                'actual presented image was not checked')
        require(f['presentedImageBeforeFirstPresentRejected'] is True,'uninitialized readback must refuse without writes')
        require(f['presentedImageScope']=='DXVK-Presenter-swapchain-output-not-compositor-or-HWND-screenshot',
                'presented-image scope overclaim')
        for key,value in dict(presentedImageReadAttempts=8,presentedImageReads=8,
                              presentedImageBufferGuardChecks=8,presentedImageBeforeFirstPresentAttempts=1,
                              totalPixelOracleChecks=61152).items():
            require(native_uint(f[key],32) and f[key]==value,'presented image coverage '+key)
        p=f['presentedImageOracle']
        require(all(native_uint(p[key],32) for key in ('checked','failures','maxChannelError')),
                'presented oracle integer types')
        require(p['checked']==20384 and p['failures']==0 and p['maxChannelError']<=1,'presented image pixel errors')
        hashes=[frame['presentedOracle']['imageFnv'] for frame in f['frames']]
        require(len(set(hashes))==8,'actual swapchain output did not follow eight independent frames')
    require(host['schema']=='warvk-d3d9-host/v1' and host['ok'] is True, 'host did not finish')
    require(proxy['schema']=='warvk-d3d9-proxy/v1' and proxy['childExit']==0
            and proxy['peerVerified'] is True and proxy['transportFailed'] is False, 'peer settlement')
    require(proxy['x86VulkanAbsent'] is True and proxy['deviceLost'] is False, 'x86 Vulkan/terminal state')
    require(proxy['remoteTerminal'] is False and proxy['childNaturalExit'] is True,'normal peer exit proof')
    require(host['actualDxvkCore'] is True and host['actualDxso'] is True and host['gpuInitialized'] is True
            and host['bits']==64,'real x64 backend')
    require(host['presentImplemented'] is True and host['normalTeardown'] is True
            and host['shutdownQueueDrained'] is True, 'real Presenter teardown and queue drain')
    for field in ('presentAttempts','acquiredImages','presentQueued','presentApiSucceeded',
                  'presentGpuCompleted','presentRetired'):
        require(native_uint(host[field]) and host[field]==8, 'WSI milestone '+field)
    require(all(native_uint(host[key],32) and host[key]==0 for key in
                ('lastAcquireVkResult','lastPresentVkResult')), 'WSI Vulkan results')
    require(all(native_uint(host[key]) for key in ('submitted','gpuCompleted','retired','draws','clears','readbacks',
                'liveResources','liveLogicalBytes','accepted','rejected','windowProcessId','windowThreadId')),
            'host counter integer types')
    require(native_uint(proxy['exchanges']) and native_uint(proxy['childExit'],32),'proxy counter integer types')
    require(host['submitted']==host['gpuCompleted']==host['retired']==65, 'exact GPU and resources must retire')
    require(host['draws']==24 and host['clears']==8 and host['readbacks']==16, 'real multi-frame population')
    require(host['liveResources']==host['liveLogicalBytes']==0, 'live resource leak')
    for field in ('presentedReadbacks','presentedFrame'):
        require(native_uint(host[field]) and host[field]==(8 if readback else 0),'diagnostic mode counter '+field)
    require(proxy['exchanges']==host['accepted']+host['rejected'], 'command balance')
    for key,value in dict(bits=64,backendCalls=85+f['queryPolls'],accepted=101+f['queryPolls'],rejected=0,
                          liveLogicalHandles=0,liveQueries=0,queryHandlesCreated=8,queryHandlesDestroyed=8,
                          queryIssues=8,queryPolls=f['queryPolls'],
                          uploads=7,uploadedBytes=131291,queries=8,buffersCreated=2,texturesCreated=1,
                          shadersCompiled=2,declarationsCreated=1).items():
        require(native_uint(host[key]) and host[key]==value,'WSI host population '+key)
    require(host['gameTouched'] is False,'host must not claim game integration')
    require(desktop['schema']=='warvk-d3d9-desktop/v1' and desktop['beforeGpu'] is True
            and desktop['windowStation']=='WinSta0' and desktop['desktop']==expected_desktop
            and desktop['inputDesktop']==input_desktop, 'x64 real desktop before GPU')
    require(desktop['pid']==proxy['childPid'] and desktop['peerPid']==host['windowProcessId']==launch['pid']
            and desktop['windowThreadId']==host['windowThreadId']==f['windowThreadId'], 'authenticated HWND owner')
    for row in (host,proxy):
        require(row['productIntegrated'] is False and row['memorySavingsProven'] is False, 'scope overclaim')


def check_reader_mutations(f,proxy,host,desktop,expected_desktop,readback,launch,input_desktop):
    """Mutate the actual run's read model, not pretend synthetic GPU evidence."""
    mutations=[(0,('ok',),False),(0,('frames',),[]),(0,('frames',0,'presentHresult'),0x80004001),
        (0,('frames',2,'afterOracle','failures'),1),(0,('pixelOracle','checked'),0),
        (0,('deviceFinalRefs',),1),(0,('apiDesktopName',),'Default'),
        (0,('inputDesktopNameAtEnd',),'changed'),(0,('windowPixelsVerified',),True),
        (0,('framesCompleted',),True),(0,('pixelOracle','failures'),False),
        (0,('queryCompleted',),False),(0,('frames',3,'windowMessagesAfter'),0),
        (1,('childExit',),2),(1,('deviceLost',),True),(1,('x86VulkanAbsent',),False),
        (2,('presentRetired',),7),(2,('presentGpuCompleted',),7),(2,('presentApiSucceeded',),0),
        (2,('lastAcquireVkResult',),1000001003),(2,('lastPresentVkResult',),1),
        (2,('liveResources',),1),(2,('productIntegrated',),True),(2,('shutdownQueueDrained',),False),
        (3,('beforeGpu',),False),(3,('desktop',),'Default'),(3,('peerPid',),0),
        (1,('childNaturalExit',),False),(1,('remoteTerminal',),True),
        (2,('presentedReadbacks',),7 if readback else 1),(1,('childPid',),0),(1,('childCreation',),False),
        (2,('submitted',),True),(2,('retired',),66),(2,('lastPresentVkResult',),False),
        (2,('gameTouched',),True),(2,('liveResources',),False),
        (4,('jobAtomic',),False),(4,('handleListApplied',),False),(4,('forcedTermination',),True),
        (4,('creation100ns',),0),(4,('pid',),0),
        (4,('actualExecutable',),'wrong-image.exe'),(4,('jobActiveProcesses',),1),
        (4,('jobActiveProcesses',),False)]
    if readback:
        mutations.extend([(0,('frames',0,'presentedFrame'),0),(0,('frames',4,'presentedFrame'),4),
            (0,('frames',7,'presentedOracle','failures'),1),(0,('presentedImageOracle','checked'),40768),
            (0,('presentedImagePixelsVerified',),False),(0,('presentedImageBufferGuardChecks',),0),
            (0,('frames',1,'presentedReadbackHresult'),1),(2,('presentedFrame',),7)])
    killed=0
    for index,path,value in mutations:
        rows=copy.deepcopy([f,proxy,host,desktop,launch]);target=rows[index]
        for key in path[:-1]:target=target[key]
        target[path[-1]]=value
        try:check_wsi(*rows[:4],expected_desktop,readback,rows[4],input_desktop)
        except (ValueError,KeyError,TypeError):killed+=1
    require(killed==len(mutations),'acceptance reader mutation survived')
    return dict(tested=len(mutations),killed=killed,source='actual-passing-run',gpuRerun=False)


def check_fault(f,proxy,host,desktop,launch,scenario,expected_desktop,input_desktop,readback=False,live_resource=False):
    """Expected window rejection is not normal GPU teardown or a driver-loss test."""
    check_peer_identity(proxy,desktop)
    check_launch(launch,expected_desktop,input_desktop)
    fault=scenario!='control'
    require(not live_resource or scenario=='marker','live fault scope')
    expected_exit=4 if live_resource else 2 if fault else 0
    require(scenario in ('control','marker','extent','destroy'),'fault case scope')
    require(f['schema']=='warvk-d3d9-presenter-fault-fixture/v1' and f['ok'] is True
            and f['scenario']==scenario and f['scope']=='controlled-window-failure-not-driver-loss',
            'fault fixture result')
    require(readback or 'diagnosticReadback' not in f,'unrequested diagnostic readback evidence')
    require(live_resource or 'liveResource' not in f,'unrequested live resource evidence')
    for key in ('publicD3D9Api','fixtureCleanupComplete','moduleUnloaded','windowClosed','windowDestroyed',
                'windowClassUnregistered','windowShownOnNonInputDesktop','parentVulkanAbsent'):
        require(f[key] is True,'fault fixture incomplete '+key)
    for key in ('postFaultNoGpuLocallyCertified','gpuNormalTeardownClaimed','globalInputUsed','desktopSwitched',
                'gameTouched','productIntegrated','memorySavingsProven','windowPixelsVerified','playerVisualAccepted'):
        require(f[key] is False,'fault scope overclaim '+key)
    for key in ('observedExpectedRejection','mutationApplied','repeatedPresentCalled','cooperativeFinalCalled'):
        require(f[key] is fault,'fault scenario flag '+key)
    values=dict(bits=32,deviceFinalRefs=0,factoryFinalRefs=0,expectedHostExit=expected_exit,
                clearCalls=2,presentCalls=3 if fault else 2,cooperativeCalls=3 if fault else 2,
                successfulPresents=1 if fault else 2,terminalRejectionChecks=5 if fault else 0,
                windowPulses=2,markerMatches=1 if scenario=='marker' else 0,
                markersRemoved=1 if scenario=='marker' else 0,
                widthAfterMutation=0 if scenario=='destroy' else 65 if scenario=='extent' else 64,
                heightAfterMutation=0 if scenario=='destroy' else 64,
                warmupClearHresult=0,warmupPresentHresult=0,cooperativeBeforeHresult=0,
                nextPresentHresult=0x88760868 if fault else 0,cooperativeAfterHresult=0x88760868 if fault else 0,
                nextClearHresult=0x88760868 if fault else 0,repeatedPresentHresult=0x88760868 if fault else 1,
                cooperativeFinalHresult=0x88760868 if fault else 1)
    for key,value in values.items():
        require(native_uint(f[key],32) and f[key]==value,'fault fixture population '+key)
    require(all(native_uint(f[key],32) and f[key]>0 for key in ('apiThreadId','windowThreadId','windowMessages')),
            'fault native thread witnesses')
    require(f['apiThreadId']!=f['windowThreadId'] and f['apiDesktop']==f['windowDesktop']==expected_desktop,
            'fault independent pump/desktop')
    require(f['inputBefore']==f['inputAfter']==input_desktop and input_desktop!=expected_desktop,
            'fault input desktop changed')
    require(launch['ok'] is True and launch['exitCode']==0 and launch['timedOut'] is False
            and launch['forcedTermination'] is False and launch['jobEmpty'] is True
            and launch['processSettled'] is True and launch['cleanupErrors']==[],
            'fault expected outcome cannot be timeout/kill/cleanup failure')
    require(launch['desktop']==expected_desktop and launch['inputBefore']==launch['inputAfter']==input_desktop,
            'fault launch desktop identity')
    require(native_uint(launch['pid'],32) and launch['pid']>0 and native_uint(launch['creation100ns'])
            and launch['creation100ns']>0 and native_uint(launch['exitCode'],32)
            and launch['initialThreadId']==f['apiThreadId'],'actual parent process/thread identity')
    require(proxy['schema']=='warvk-d3d9-proxy/v1' and proxy['peerVerified'] is True
            and proxy['transportFailed'] is False and proxy['x86VulkanAbsent'] is True
            and proxy['childProcessSettled'] is True
            and proxy['childNaturalExit'] is (not live_resource)
            and proxy['childSelfFailStop'] is live_resource,'fault exact child exit classification')
    require(native_uint(proxy['primaryExitWaitMs']),'invalid primary child wait duration')
    for key in ('remoteTerminal','deviceLost'):
        require(proxy[key] is fault,'terminal latch '+key)
    for key,value in dict(childExit=expected_exit,exchanges=(4 if fault else 6)+2*int(live_resource)).items():
        require(native_uint(proxy[key]) and proxy[key]==value,'fault wire population '+key)
    require(host['schema']==('warvk-d3d9-host-fault/v1' if fault else 'warvk-d3d9-host/v1')
            and host['ok'] is (not fault) and host['normalTeardown'] is (not fault),'fault GPU status was conflated')
    require(host['actualDxvkCore'] is True and host['actualDxso'] is True and host['gpuInitialized'] is True,
            'fault was not reached through real DXVK')
    require(host['shutdownQueueDrained'] is (not live_resource),'fault shutdown queue drain classification')
    submissions=(3 if fault else 5)+int(live_resource)
    values=dict(bits=64,accepted=(3 if fault else 6)+2*int(live_resource),rejected=1 if fault else 0,
                backendCalls=(4 if fault else 6)+2*int(live_resource),presentAttempts=2,presentApiSucceeded=1 if fault else 2,
                presentRetired=1 if fault else 2,draws=0,clears=1 if fault else 2,
                submitted=submissions,gpuCompleted=submissions,retired=submissions)
    for key,value in values.items():
        require(native_uint(host[key]) and host[key]==value,'post-fault GPU population '+key)
    if fault:
        require(host['quarantined'] is live_resource and host['faultKind']=='window-preflight'
                and host['gpuCleanupCompleted'] is (not live_resource)
                and host['exitStrategy']==('self-fail-stop' if live_resource else 'owner-cleanup-return')
                and native_uint(host['result'],32) and host['result']==0x88760868,'typed terminal cause')
        require(native_uint(host['gpuCleanupMs']),'fault cleanup duration')
        for key,value in dict(acquiredImages=1,presentQueued=1,presentGpuCompleted=1,
                              liveResources=int(live_resource),liveLogicalBytes=64*int(live_resource)).items():
            require(native_uint(host[key]) and host[key]==value,'window fault boundary '+key)
    else:
        for key,value in dict(liveResources=0,liveLogicalBytes=0,
                              lastAcquireVkResult=0,lastPresentVkResult=0,acquiredImages=2,
                              presentQueued=2,presentGpuCompleted=2).items():
            require(native_uint(host[key]) and host[key]==value,'fault control normal teardown '+key)
    require(desktop['schema']=='warvk-d3d9-desktop/v1' and desktop['beforeGpu'] is True
            and desktop['windowStation']=='WinSta0' and desktop['desktop']==expected_desktop
            and desktop['inputDesktop']==input_desktop,'fault x64 Desktop witness')
    require(desktop['pid']==proxy['childPid'] and desktop['peerPid']==host['windowProcessId']==launch['pid']
            and desktop['windowThreadId']==host['windowThreadId']==f['windowThreadId'],
            'fault exact peer/HWND ownership')
    for row in (proxy,host):
        require(row['productIntegrated'] is False and row['memorySavingsProven'] is False,'fault scope')
    require(host['gameTouched'] is False,'fault host scope')
    for key in ('presentedReadbacks','presentedFrame'):
        require(native_uint(host[key]) and host[key]==int(readback)*(1 if fault else 2),'fault diagnostic count '+key)
    if live_resource:
        r=f['liveResource']
        require(r['enabled'] is True,'live resource test disabled')
        for key,value in dict(bytes=64,createHresult=0,lockHresult=0,unlockHresult=0,finalRefs=0).items():
            require(native_uint(r[key]) and r[key]==value,'live resource fixture '+key)
    if readback:
        d=f['diagnosticReadback']
        require(scenario in ('marker','control') and d['schema']=='warvk-d3d9-presenter-readback-check/v1'
                and d['enabled'] is True and d['profile']=='presenter-readback-isolated-v1'
                and d['scope']=='acquired-present-image-not-window-pixels' and d['format']=='BGRA8',
                'terminal readback scope')
        for key,value in dict(imageBytes=16384,readCalls=2,warmupHresult=0,warmupFrame=1,
                              expectedBgra=0xff204060,rgbTolerance=1,expectedAlpha=255,pixelsChecked=4096,
                              pixelFailures=0,alphaFailures=0).items():
            require(native_uint(d[key]) and d[key]==value,'terminal readback population '+key)
        require(native_uint(d['maxRgbError'],32) and d['maxRgbError']<=1,'warmup RGB mismatch')
        if fault:
            for key,value in dict(postFaultHresult=0x88760868,postFaultBufferBytesChecked=16384,
                                  postFaultBufferBytesChanged=0).items():
                require(native_uint(d[key]) and d[key]==value,'terminal readback output '+key)
            require(d['postFaultFrameUnchanged'] is True,'stale output escaped after fault')
        else:
            s=d['controlSecond']
            for key,value in dict(hresult=0,frame=2,pixelsChecked=4096,pixelFailures=0,alphaFailures=0,
                                  expectedBgra=0xff806040).items():
                require(native_uint(s[key]) and s[key]==value,'second clear-only frame '+key)
            require(native_uint(s['maxRgbError'],32) and s['maxRgbError']<=1,'second clear-only RGB mismatch')


def check_fault_mutations(f,proxy,host,desktop,launch,scenario,expected_desktop,input_desktop,readback=False,live_resource=False):
    mutations=[(0,('ok',),False),(0,('fixtureCleanupComplete',),False),(0,('deviceFinalRefs',),1),
        (0,('successfulPresents',),0),(0,('nextPresentHresult',),1),(0,('gpuNormalTeardownClaimed',),True),
        (0,('inputAfter',),'Default-changed'),(1,('childNaturalExit',),live_resource),
        (1,('childProcessSettled',),False),(1,('childSelfFailStop',),not live_resource),(1,('transportFailed',),True),
        (1,('exchanges',),99),(2,('submitted',),99),(2,('clears',),99),(2,('presentApiSucceeded',),0),
        (2,('shutdownQueueDrained',),live_resource),
        (3,('beforeGpu',),False),(3,('peerPid',),0),(4,('forcedTermination',),True),
        (4,('timedOut',),True),(4,('jobEmpty',),False),(4,('exitCode',),2),
        (1,('childCreation',),False),(1,('childPid',),0),(2,('gameTouched',),True),
        (4,('jobAtomic',),False),(4,('handleListApplied',),False),
        (4,('actualExecutable',),'wrong-image.exe'),(4,('jobActiveProcesses',),1),
        (4,('jobActiveProcesses',),False)]
    if scenario!='control':
        mutations.extend([(0,('terminalRejectionChecks',),0),(1,('remoteTerminal',),False),
            (1,('deviceLost',),False),(1,('childExit',),0),(2,('quarantined',),not live_resource),
            (2,('gpuCleanupCompleted',),live_resource),(2,('faultKind',),'unknown'),
            (2,('exitStrategy',),'normal'),(2,('liveResources',),99),(2,('acquiredImages',),2),
            (2,('normalTeardown',),True),(2,('ok',),True),(2,('result',),0)])
    if readback and scenario!='control':
        mutations.extend([(0,('diagnosticReadback','postFaultHresult'),0),
            (0,('diagnosticReadback','postFaultBufferBytesChanged'),1),
            (0,('diagnosticReadback','postFaultFrameUnchanged'),False),
            (0,('diagnosticReadback','warmupFrame'),0),(0,('diagnosticReadback','pixelsChecked'),0)])
    if readback and scenario=='control':
        mutations.extend([(0,('diagnosticReadback','controlSecond','frame'),1),
                          (0,('diagnosticReadback','controlSecond','pixelFailures'),1),
                          (0,('diagnosticReadback','controlSecond','pixelsChecked'),0)])
    if live_resource:
        mutations.extend([(0,('liveResource','finalRefs'),1),(0,('liveResource','bytes'),0)])
    else:
        mutations.append((0,('liveResource',),dict(enabled=True)))
    if not readback:
        mutations.append((0,('diagnosticReadback',),dict(enabled=True)))
    killed=0
    for index,path,value in mutations:
        rows=copy.deepcopy([f,proxy,host,desktop,launch]);target=rows[index]
        for key in path[:-1]:target=target[key]
        target[path[-1]]=value
        try:check_fault(*rows,scenario,expected_desktop,input_desktop,readback,live_resource)
        except (ValueError,KeyError,TypeError):killed+=1
    require(killed==len(mutations),'fault acceptance reader mutation survived')
    return dict(tested=len(mutations),killed=killed,source='actual-passing-controlled-run',gpuRerun=False)


def check_soak(f,proxy,host,desktop,launch,expected_desktop,input_desktop):
    check_launch(launch,expected_desktop,input_desktop)
    check_peer_identity(proxy,desktop)
    require(f['schema']=='warvk-d3d9-presenter-clear-soak/v1' and f['ok'] is True
            and f['scope']=='clear-handoff-acquired-image-not-window-pixels','soak scope')
    for key in ('moduleUnloaded','windowClosed','windowDestroyed','windowClassUnregistered',
                'publicD3D9Api','parentVulkanAbsent'):
        require(f[key] is True,'soak missing '+key)
    for key in ('gameTouched','globalInputUsed','desktopSwitched','productIntegrated',
                'memorySavingsProven','windowPixelsVerified','playerVisualAccepted'):
        require(f[key] is False,'soak overclaim '+key)
    for key,value in dict(bits=32,frames=256,clears=256,pixelsChecked=1048576,frameSum=32896,
            pixelFailures=0,alphaFailures=0,sourceReadbacks=0,queryCalls=0,authorResources=0,
            # One startup pulse plus one after each of the 256 Presents.
            deviceFinalRefs=0,factoryFinalRefs=0,windowPulses=257).items():
        require(native_uint(f[key]) and f[key]==value,'soak population '+key)
    require(native_uint(f['maxRgbError']) and f['maxRgbError']<=1,'soak pixels')
    # Pulses are acknowledged HANDLE events, not posted window messages. The
    # exact 257 pulses above prove per-frame pump service; OS message count is
    # separately observed and is not proportional to Presents.
    require(native_uint(f['elapsedMs']) and f['elapsedMs']>0 and native_uint(f['windowMessages'])
            and f['windowMessages']>0,'soak actual progress')
    for key in ('apiThreadId','windowThreadId'):
        require(native_uint(f[key],32) and f[key]>0,'soak thread identity')
    require(f['apiThreadId']==launch['initialThreadId'] and f['apiThreadId']!=f['windowThreadId'],'soak pump')
    require(f['apiDesktop']==f['windowDesktop']==desktop['desktop']==expected_desktop
            and f['inputBefore']==f['inputAfter']==desktop['inputDesktop']==input_desktop,'soak desktops')
    require(desktop['pid']==proxy['childPid'] and desktop['peerPid']==launch['pid']
            and desktop['windowThreadId']==f['windowThreadId'] and desktop['beforeGpu'] is True,'soak host owner')
    for key in ('peerVerified','x86VulkanAbsent','childNaturalExit','childProcessSettled'):
        require(proxy[key] is True,'soak proxy '+key)
    for key in ('transportFailed','deviceLost','remoteTerminal','childSelfFailStop','productIntegrated','memorySavingsProven'):
        require(proxy[key] is False,'soak proxy '+key)
    for key,value in dict(childExit=0,exchanges=514).items():
        require(native_uint(proxy[key]) and proxy[key]==value,'soak proxy count '+key)
    require(native_uint(proxy['primaryExitWaitMs']) and proxy['primaryExitWaitMs']<=5000,'soak primary exit')
    require(host['schema']=='warvk-d3d9-host/v1' and host['ok'] is True,'soak host')
    for key in ('actualDxvkCore','actualDxso','normalTeardown','presentImplemented','gpuInitialized','shutdownQueueDrained'):
        require(host[key] is True,'soak host '+key)
    for key in ('productIntegrated','gameTouched','memorySavingsProven'):
        require(host[key] is False,'soak host overclaim '+key)
    counts=dict(bits=64,accepted=514,rejected=0,backendCalls=514,submitted=513,gpuCompleted=513,retired=513,
        draws=0,clears=256,uploads=0,uploadedBytes=0,readbacks=0,queries=0,liveResources=0,liveLogicalBytes=0,
        liveLogicalHandles=0,liveQueries=0,queryIssues=0,queryPolls=0,buffersCreated=0,texturesCreated=0,
        shadersCompiled=0,declarationsCreated=0,presentedReadbacks=256,presentedFrame=256,
        presentAttempts=256,acquiredImages=256,presentQueued=256,presentApiSucceeded=256,
        presentGpuCompleted=256,presentRetired=256,windowProcessId=launch['pid'],windowThreadId=f['windowThreadId'])
    for key,value in counts.items():
        require(native_uint(host[key]) and host[key]==value,'soak host population '+key)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('toolchain','build','reference-dll','output'):
        ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--fault-repetitions',type=int,default=1,choices=range(1,9))
    ap.add_argument('--restricted-medium',action='store_true',
                    help='opt-in restricted medium-integrity children on the same non-input Desktop')
    args=ap.parse_args()
    out=args.output.resolve();out.mkdir(parents=False,exist_ok=False)
    receipt=dict(schema=1,scope='D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP',ok=False,
                 gameTouched=False,playerDllChanged=None,productIntegrated=False,
                 memorySavingsProven=False,windowPixelsVerified=False,steps=[],cases=[],caseCopies=[],
                 stepRawArtifacts=[],rawArtifactsPreserved=False,
                 faultRepetitions=args.fault_repetitions,restrictedMediumRequested=args.restricted_medium)
    def source_set():
        return set(sources()) | {ROOT/'AutoTest'/name for name in ('run_render_host_presenter_gate.py',
            'isolated_render_desktop.py','render_lab_token.py','test_render_lab_token.py',
            'render_lab_default_dacl.py','test_render_lab_default_dacl.py',
            'render_lab_raw_artifacts.py','test_render_lab_raw_artifacts.py',
            'test_render_host_desktop_probe.cpp','test_isolated_render_desktop_contract.py',
            'test_render_host_d3d9_init.cpp','test_render_host_presenter_fault.cpp')}
    def copy_artifact(source,target):
        origin=identity(source);shutil.copy2(source,target);copied=identity(target)
        require(all(origin[k]==copied[k] for k in ('bytes','sha256')),'case copy mismatch')
        receipt['caseCopies'].append(dict(source=origin,copy=copied))
    try:
        tc=args.toolchain.resolve(strict=True);build=args.build.resolve(strict=True)
        ref=args.reference_dll.resolve(strict=True)
        player=Path('E:/Work/Warcraft III/d3d9.dll')
        receipt['referenceBefore']=identity(ref);receipt['playerBefore']=identity(player)
        paths=source_set()
        before=[identity(p) for p in sorted(paths)];receipt['sources']=before
        receipt['allowedSharedD3D9Inputs']=check_host_compile_boundary(build)
        cc={bits:tc/(arch+'-w64-mingw32-clang++.exe') for bits,arch in ((32,'i686'),(64,'x86_64'))}
        receipt['compilers']=[identity(p) for p in cc.values()]
        # Minimal process environment: Meson and native diagnostics must never
        # receive unrelated credentials through copied automation environments.
        env={k:v for k,v in os.environ.items() if k.upper() in {
            'SYSTEMROOT','WINDIR','COMSPEC','PATH','TEMP','TMP','USERPROFILE','APPDATA','LOCALAPPDATA',
            'PROGRAMFILES','PROGRAMFILES(X86)','PROGRAMDATA','NUMBER_OF_PROCESSORS','PROCESSOR_ARCHITECTURE'}}
        env.update(DXVK_LOG_LEVEL='info',DXVK_SHADER_CACHE='0')
        receipt['steps'].append(execute([sys.executable,'-B',ROOT/'AutoTest/test_isolated_render_desktop_contract.py'],
            ROOT,out/'desktop-reader-tests',30,env))
        receipt['steps'].append(execute([sys.executable,'-B',ROOT/'AutoTest/test_render_host_d3d9_gate.py'],
            ROOT,out/'offscreen-reader-tests',30,env))
        common=[ROOT/'tools/render_host'/p for p in
                ('protocol.cpp','win32_transport.cpp','win32_security.cpp','owned_child.cpp')]
        for bits in (32,64):
            exe=out/f'desktop-probe-{bits}.exe'
            receipt['steps'].append(execute([cc[bits],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
                '-municode','-D_WIN32_WINNT=0x0a00',ROOT/'AutoTest/test_render_host_desktop_probe.cpp',*common,
                '-luser32','-lbcrypt','-ladvapi32','-o',exe],ROOT,out/f'compile-probe-{bits}',180,env))
            wire=out/f'wire-{bits}.exe'
            receipt['steps'].append(execute([cc[bits],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
                ROOT/'AutoTest/test_render_host_d3d9_wire.cpp',ROOT/'tools/render_host/protocol.cpp','-o',wire],
                ROOT,out/f'compile-wire-{bits}',180,env))
            registry=out/f'resource-registry-{bits}.exe'
            receipt['steps'].append(execute([cc[bits],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
                ROOT/'AutoTest/test_render_host_d3d9_resource_registry.cpp','-o',registry],
                ROOT,out/f'compile-resource-registry-{bits}',180,env))
        target='tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe'
        receipt['steps'].append(execute(['ninja','-C',build,'-j2',target],ROOT,out/'host-build',600,env))
        receipt['steps'].append(execute(['ninja','-C',build,'-n',target],ROOT,out/'host-no-work',30,env))
        require('no work to do' in (out/'host-no-work.stdout').read_text('utf-8'),'host not frozen')
        require(receipt['allowedSharedD3D9Inputs']==check_host_compile_boundary(build),'host compile boundary changed')
        receipt['compileCommands']=identity(build/'compile_commands.json')
        host=build/target;proxy=out/'warvk-d3d9-proxy-dev.dll';parent=out/'d3d9-api-parent32.exe'
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror',
            '-Wno-unused-parameter','-Wno-microsoft-exception-spec','-static','-pthread','-shared',
            '-Wl,--kill-at','-DNOMINMAX','-D_WIN32_WINNT=0x0a00',ROOT/'tools/render_host/d3d9_proxy.cpp',
            ROOT/'tools/render_host/d3d9_proxy.def',*common,ROOT/'tools/render_host/process_memory_probe.cpp',
            '-luser32','-lbcrypt','-ladvapi32','-luuid','-lpsapi','-o',proxy],
            ROOT,out/'proxy-build',240,env))
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
            '-municode',ROOT/'AutoTest/test_render_host_d3d9_parent.cpp','-luser32','-o',parent],
            ROOT,out/'parent-build',180,env))
        init_fixture=out/'d3d9-init-parent32.exe'
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
            '-municode','-D_WIN32_WINNT=0x0a00',ROOT/'AutoTest/test_render_host_d3d9_init.cpp',*common,
            '-luser32','-lbcrypt','-ladvapi32','-o',init_fixture],ROOT,out/'init-parent-build',180,env))
        fault_fixture=out/'d3d9-presenter-fault32.exe'
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
            '-municode','-D_WIN32_WINNT=0x0a00',ROOT/'AutoTest/test_render_host_presenter_fault.cpp',
            '-luser32','-o',fault_fixture],ROOT,out/'fault-parent-build',180,env))
        require(pe_identity(parent)==pe_identity(proxy)==pe_identity(ref)==(0x14c,0x10b),'x86 inputs')
        require(pe_identity(host)==(0x8664,0x20b),'x64 host')
        receipt['binaries']=[identity(p) for p in (parent,proxy,host,init_fixture,fault_fixture,
            *(out/f'desktop-probe-{bits}.exe' for bits in (32,64)))]
        # This is an actual separate Win32 desktop, not CREATE_NO_WINDOW.
        with IsolatedRenderDesktop(restricted_medium=args.restricted_medium) as desktop:
            receipt['desktopName']=desktop.desktop_name
            receipt['desktopSafety']=desktop.receipt
            probe=desktop.run([out/'desktop-probe-32.exe','--expect-desktop',desktop.desktop_name,
                '--expect-station','WinSta0','--input-desktop',desktop.input_before,
                '--child',out/'desktop-probe-64.exe','--child-log',out/'desktop-child.log'],
                cwd=out,stdout_path=out/'probe.stdout',stderr_path=out/'probe.stderr',timeout_s=30,env=env)
            receipt['desktopProbe']=probe
            receipt['probeRawArtifacts']=capture_raw_artifacts(out,['probe.stdout','probe.stderr','desktop-child.log'])
            require(probe['ok'] is True,'non-GPU desktop probe failed')
            # Native probe validates all actual thread names plus child exit.
            receipt['probeReports']=json_lines(out/'probe.stdout')+json_lines(out/'desktop-child.log')
            require(len(receipt['probeReports'])==2,'native actual desktop reports missing')
            validate_probe_receipt(receipt['probeReports'][0],desktop=desktop.desktop_name,input_desktop=desktop.input_before,
                station='WinSta0',bits=32,child=receipt['probeReports'][1],launch=probe)
            for bits in (32,64):
                row=desktop.run([out/f'wire-{bits}.exe'],cwd=out,stdout_path=out/f'wire-{bits}.stdout',
                    stderr_path=out/f'wire-{bits}.stderr',timeout_s=30,env=env)
                receipt['stepRawArtifacts'].append(capture_raw_artifacts(out,[f'wire-{bits}.stdout',f'wire-{bits}.stderr']))
                require(row['ok'] is True,'CPU wire failed')
                receipt['steps'].append(row)
                row=desktop.run([out/f'resource-registry-{bits}.exe'],cwd=out,
                    stdout_path=out/f'resource-registry-{bits}.stdout',stderr_path=out/f'resource-registry-{bits}.stderr',
                    timeout_s=30,env=env)
                receipt['stepRawArtifacts'].append(capture_raw_artifacts(out,
                    [f'resource-registry-{bits}.stdout',f'resource-registry-{bits}.stderr']))
                require(row['ok'] is True,'CPU shared resource registry failed')
                receipt['steps'].append(row)
            for scenario in ('no-init','invalid-init','window-reject'):
                directory=out/('init-'+scenario);directory.mkdir()
                local=env|dict(WARVK_D3D9_HOST_EXE=str(host),WARVK_D3D9_HOST_LOG=str(directory/'host.log'),
                               DXVK_LOG_PATH=str(directory),DXVK_CONFIG_FILE=str(directory/'absent.conf'))
                case=dict(label='init-'+scenario,ok=False);receipt['cases'].append(case)
                case['process']=desktop.run([init_fixture,'--scenario',scenario],cwd=directory,
                    stdout_path=directory/'case.stdout',stderr_path=directory/'case.stderr',timeout_s=30,env=local)
                case['initialRawArtifacts']=capture_raw_artifacts(directory,['case.stdout','case.stderr','host.log'],True)
                case['reports']=json_lines(directory/'case.stdout');case['hostReports']=json_lines(directory/'host.log')
                require(case['process']['ok'] is True,'rejected init did not close normally')
                rows,host_rows=check_case_records(case['reports'],case['hostReports'],
                    'warvk-d3d9-init/v1','warvk-d3d9-host/v1',init_cases=True)
                pr=rows['warvk-d3d9-proxy/v1'];hs=host_rows['warvk-d3d9-host/v1'];fi=rows['warvk-d3d9-init/v1']
                require(fi['ok'] is True and fi['scenario']==scenario and fi['bits']==32 and fi['error']=='',
                        'initialization fixture did not complete')
                require(fi['finishSucceeded'] is True and fi['connectionDestroyed'] is True
                        and fi['x86VulkanAbsent'] is True,'initialization cleanup scope')
                count=0 if scenario=='no-init' else 34 if scenario=='invalid-init' else 6+int(fi['foreignWindowOwnerTested'])
                require(native_uint(fi['invalidCallChecks'],32) and fi['invalidCallChecks']==count
                        and fi['expectedExchanges']==count+1 and hs['accepted']==1 and hs['rejected']==count,
                        'actual rejected initialization population')
                if scenario=='window-reject':
                    require(fi['windowClosed'] is True and fi['windowChecks']==count,'invalid window lifetime')
                require(pr['childExit']==0 and pr['transportFailed'] is False and pr['deviceLost'] is False,'init peer failure')
                require(hs['ok'] is True and hs['normalTeardown'] is True and hs['gpuInitialized'] is False
                        and hs['shutdownQueueDrained'] is False,'init CPU close without GPU queue drain')
                for key in ('backendCalls','submitted','gpuCompleted','retired','draws','presentAttempts',
                            'presentQueued','liveResources','liveLogicalBytes'):
                    require(native_uint(hs[key]) and hs[key]==0,'rejected init touched GPU: '+key)
                require(pr['exchanges']==hs['accepted']+hs['rejected'],'init command balance')
                case['recordMutations']=check_case_record_mutations(case['reports'],case['hostReports'],
                    'warvk-d3d9-init/v1','warvk-d3d9-host/v1',init_cases=True)
                case['childSettlement']=child_settled(pr['childPid'],pr['childCreation']);case['ok']=True
            # Same owner and swapchain across 256 Present operations. No source
            # readback or query is allowed to mask deferred-clear handoff.
            case_dir=out/'clear-soak';case_dir.mkdir()
            local_parent=case_dir/'d3d9-presenter-fault32.exe';copy_artifact(fault_fixture,local_parent)
            local_dll=case_dir/'proxy.dll';copy_artifact(proxy,local_dll)
            local_host=case_dir/'warvk-d3d9-host-dev.exe';copy_artifact(host,local_host)
            local=env|dict(DXVK_LOG_PATH=str(case_dir),DXVK_CONFIG_FILE=str(case_dir/'absent.conf'),
                WARVK_D3D9_PROFILE='presenter-readback-isolated-v1',WARVK_D3D9_HOST_EXE=str(local_host),
                WARVK_D3D9_HOST_LOG=str(case_dir/'host.log'))
            case=dict(label='clear-soak',ok=False);receipt['cases'].append(case)
            case['process']=desktop.run([local_parent,'--dll',local_dll,'--scenario','clear-soak',
                '--diagnostic-readback','1','--expect-desktop',desktop.desktop_name,'--input-desktop',desktop.input_before],
                cwd=case_dir,stdout_path=case_dir/'case.stdout',stderr_path=case_dir/'case.stderr',timeout_s=45,env=local)
            case['initialRawArtifacts']=capture_raw_artifacts(case_dir,['case.stdout','case.stderr','host.log'],True)
            case['reports']=json_lines(case_dir/'case.stdout');case['hostReports']=json_lines(case_dir/'host.log')
            rows,hosts=check_case_records(case['reports'],case['hostReports'],
                'warvk-d3d9-presenter-clear-soak/v1','warvk-d3d9-host/v1',host_desktop=True)
            fi=rows['warvk-d3d9-presenter-clear-soak/v1'];pr=rows['warvk-d3d9-proxy/v1']
            hs=hosts['warvk-d3d9-host/v1'];ds=hosts['warvk-d3d9-desktop/v1']
            check_soak(fi,pr,hs,ds,case['process'],desktop.desktop_name,desktop.input_before)
            tested=killed=0
            for index,key,value in ((0,'frames',255),(0,'frameSum',0),(0,'sourceReadbacks',1),
                    (0,'pixelsChecked',0),(0,'pixelFailures',1),(0,'alphaFailures',1),
                    (0,'parentVulkanAbsent',False),(1,'childNaturalExit',False),(1,'childSelfFailStop',True),
                    (2,'submitted',512),(2,'presentedReadbacks',0),(2,'queryIssues',1),
                    (2,'shutdownQueueDrained',False),(3,'peerPid',0),(4,'forcedTermination',True)):
                values=copy.deepcopy([fi,pr,hs,ds,case['process']]);values[index][key]=value;tested+=1
                try:check_soak(*values,desktop.desktop_name,desktop.input_before)
                except (ValueError,KeyError,TypeError):killed+=1
            require(killed==tested,'soak reader mutation survived')
            case['readerMutations']=dict(tested=tested,killed=killed,source='actual-passing-clear-soak',gpuRerun=False)
            case['recordMutations']=check_case_record_mutations(case['reports'],case['hostReports'],
                'warvk-d3d9-presenter-clear-soak/v1','warvk-d3d9-host/v1',host_desktop=True)
            case['childSettlement']=child_settled(pr['childPid'],pr['childCreation']);case['ok']=True
            for label,reference,scenario,wsi,readback in (
                ('remote-normal',False,'normal',False,False),('remote-invalid',False,'invalid',False,False),
                ('reference-normal',True,'normal',False,False),('reference-invalid',True,'invalid',False,False),
                ('presenter-eight-frames',False,'normal',True,False),
                ('presenter-readback-eight-frames',False,'normal',True,True)):
                case_dir=out/label;case_dir.mkdir()
                # Real EXE/DLL copies avoid EXE-relative crash-handler outputs
                # mutating preserved reference/build evidence.
                local_parent=case_dir/'d3d9-api-parent32.exe';copy_artifact(parent,local_parent)
                local_dll=case_dir/('reference.dll' if reference else 'proxy.dll')
                copy_artifact(ref if reference else proxy,local_dll)
                local=env|dict(DXVK_LOG_PATH=str(case_dir),DXVK_CONFIG_FILE=str(case_dir/'absent.conf'))
                cmd=[local_parent,'--dll',local_dll,'--scenario',scenario]
                if reference:cmd+=['--reference']
                else:
                    local_host=case_dir/'warvk-d3d9-host-dev.exe';copy_artifact(host,local_host)
                    local.update(WARVK_D3D9_HOST_EXE=str(local_host),WARVK_D3D9_HOST_LOG=str(case_dir/'host.log'))
                    cmd+=['--wsi-readback' if readback else '--wsi' if wsi else '--present']
                if wsi:local['WARVK_D3D9_PROFILE']='presenter-readback-isolated-v1' if readback else 'presenter-isolated-v1'
                case=dict(label=label,ok=False);receipt['cases'].append(case)
                case['process']=desktop.run(cmd,cwd=case_dir,stdout_path=case_dir/'case.stdout',
                    stderr_path=case_dir/'case.stderr',timeout_s=90,env=local)
                case['initialRawArtifacts']=capture_raw_artifacts(case_dir,
                    ['case.stdout','case.stderr']+([] if reference else ['host.log']),True)
                case['reports']=json_lines(case_dir/'case.stdout')
                if not reference:case['hostReports']=json_lines(case_dir/'host.log')
                require(case['process']['ok'] is True,label+' native failure')
                fixture_schema='warvk-d3d9-wsi-fixture/v1' if wsi else 'warvk-d3d9-api-fixture/v1'
                host_schema=None if reference else 'warvk-d3d9-host/v1'
                rows,host_rows=check_case_records(case['reports'],case.get('hostReports',[]),
                                                fixture_schema,host_schema,host_desktop=not reference)
                fi=rows[fixture_schema]
                if reference:
                    check_fixture(fi,scenario,True)
                else:
                    pr=rows['warvk-d3d9-proxy/v1'];hs=host_rows[host_schema]
                    require(hs['shutdownQueueDrained'] is True,'normal GPU shutdown queue was not drained')
                    if wsi:
                        ds=host_rows['warvk-d3d9-desktop/v1']
                        check_wsi(fi,pr,hs,ds,desktop.desktop_name,readback,
                                  case['process'],desktop.input_before)
                        require(ds['peerPid']==case['process']['pid'],'WSI exact launch owner')
                        case['readerMutations']=check_reader_mutations(fi,pr,hs,ds,desktop.desktop_name,readback,
                                                                      case['process'],desktop.input_before)
                    else:
                        check_remote(scenario,fi,pr,hs)
                        ds=host_rows['warvk-d3d9-desktop/v1'];check_peer_identity(pr,ds,False)
                        check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                        require(ds['pid']==pr['childPid'] and ds['peerPid']==case['process']['pid']
                            and ds['windowThreadId']==0 and type(ds['windowThreadId']) is int
                            and ds['beforeGpu'] is True and ds['windowStation']=='WinSta0'
                            and ds['desktop']==desktop.desktop_name and ds['inputDesktop']==desktop.input_before,
                            'offscreen actual host desktop mismatch')
                    case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                case['recordMutations']=check_case_record_mutations(case['reports'],case.get('hostReports',[]),
                    fixture_schema,host_schema,host_desktop=not reference)
                case['ok']=True
            fault_cases=(('control',False,False),('marker',False,False),('extent',False,False),
                         ('destroy',False,False),('marker',True,False),('control',True,False),
                         ('marker',True,True))
            for iteration,scenario,readback,live_resource in ((i,s,r,l) for i in range(args.fault_repetitions) for s,r,l in fault_cases):
                label='fault-'+scenario+('-readback' if readback else '')+('-live' if live_resource else '')
                if args.fault_repetitions>1:label+='-'+str(iteration+1)
                case_dir=out/label;case_dir.mkdir()
                local_parent=case_dir/'d3d9-presenter-fault32.exe';copy_artifact(fault_fixture,local_parent)
                local_dll=case_dir/'proxy.dll';copy_artifact(proxy,local_dll)
                local_host=case_dir/'warvk-d3d9-host-dev.exe';copy_artifact(host,local_host)
                local=env|dict(DXVK_LOG_PATH=str(case_dir),DXVK_CONFIG_FILE=str(case_dir/'absent.conf'),
                    WARVK_D3D9_PROFILE='presenter-isolated-v1',WARVK_D3D9_HOST_EXE=str(local_host),
                    WARVK_D3D9_HOST_LOG=str(case_dir/'host.log'))
                if readback:local['WARVK_D3D9_PROFILE']='presenter-readback-isolated-v1'
                cmd=[local_parent,'--dll',local_dll,'--scenario',scenario,
                     '--expect-desktop',desktop.desktop_name,'--input-desktop',desktop.input_before]
                if readback:cmd+=['--diagnostic-readback','1']
                if live_resource:cmd+=['--live-resource','1']
                case=dict(label=label,ok=False);receipt['cases'].append(case)
                case['process']=desktop.run(cmd,
                    cwd=case_dir,stdout_path=case_dir/'case.stdout',stderr_path=case_dir/'case.stderr',timeout_s=45,env=local)
                case['initialRawArtifacts']=capture_raw_artifacts(case_dir,['case.stdout','case.stderr','host.log'],True)
                case['reports']=json_lines(case_dir/'case.stdout');case['hostReports']=json_lines(case_dir/'host.log')
                require(case['process']['ok'] is True,'fault fixture did not settle normally')
                host_schema='warvk-d3d9-host/v1' if scenario=='control' else 'warvk-d3d9-host-fault/v1'
                rows,host_rows=check_case_records(case['reports'],case['hostReports'],
                    'warvk-d3d9-presenter-fault-fixture/v1',host_schema,host_desktop=True)
                pr=rows['warvk-d3d9-proxy/v1'];fi=rows['warvk-d3d9-presenter-fault-fixture/v1']
                hs=host_rows[host_schema];ds=host_rows['warvk-d3d9-desktop/v1']
                check_fault(fi,pr,hs,ds,case['process'],scenario,desktop.desktop_name,desktop.input_before,readback,live_resource)
                case['readerMutations']=check_fault_mutations(fi,pr,hs,ds,case['process'],scenario,
                                                              desktop.desktop_name,desktop.input_before,readback,live_resource)
                case['recordMutations']=check_case_record_mutations(case['reports'],case['hostReports'],
                    'warvk-d3d9-presenter-fault-fixture/v1',host_schema,host_desktop=True)
                case['childSettlement']=child_settled(pr['childPid'],pr['childCreation']);case['ok']=True
        receipt['referenceAfter']=identity(ref);receipt['playerAfter']=identity(player)
        require(receipt['referenceBefore']==receipt['referenceAfter'] and receipt['playerBefore']==receipt['playerAfter'],
                'preserved DLL mutated')
        require(paths==source_set() and before==[identity(p) for p in sorted(paths)],'source changed during gate')
        require(receipt['compileCommands']==identity(build/'compile_commands.json'),'compile database changed during run')
        require(receipt['binaries']==[identity(Path(row['path'])) for row in receipt['binaries']],'binary changed during run')
        require(all(row['copy']==identity(Path(row['copy']['path'])) for row in receipt['caseCopies']),
                'executed case copy changed during run')
        receipt['sourceAndReferencePreserved']=True;receipt['ok']=True
    except Exception as error:
        receipt['error']=str(error)
    finally:
        # Preservation is measured even for a failed/partial run. A prefilled
        # false change flag is not evidence that a failed test left bytes alone.
        try:
            if 'referenceBefore' in receipt:
                receipt['referenceAfter']=identity(Path(receipt['referenceBefore']['path']))
                receipt['playerAfter']=identity(Path(receipt['playerBefore']['path']))
                receipt['playerDllChanged']=receipt['playerBefore']!=receipt['playerAfter']
                require(receipt['referenceBefore']==receipt['referenceAfter'] and not receipt['playerDllChanged'],
                        'preserved DLL changed')
            if 'sources' in receipt:
                receipt['sourcesPreserved']=paths==source_set() and before==[identity(p) for p in sorted(paths)]
                require(receipt['sourcesPreserved'],'source changed during gate')
            captured=list(raw_manifests(receipt))
            for manifest in captured:verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved']=bool(captured)
        except Exception as error:
            receipt['ok']=False;receipt['preservationError']=str(error)
        (out/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in ('ok','error','scope','sourceAndReferencePreserved')},ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':
    raise SystemExit(main())
