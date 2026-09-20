"""Player-side local watcher: no disk streaming while armed; export after trigger.

Does not launch/stop games, replace DLLs, change graphics settings or send input.
Requires Python+Pillow+numpy installed. Ctrl+Shift+C is handled by the game window.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import math
from pathlib import Path
import time
from shadow_pose_full_trace_control import _request
from analyze_frame_evidence import load,analyze
from analyze_frame_history import analyze_history
from analyze_frame_inputs import analyze_inputs
import shutil
import hashlib

def call(pid,plane,payload):
    r=_request(pid,plane,payload,12)
    if not r.get('ok') or not r.get('result',{}).get('ok'):raise RuntimeError(r.get('error') or str(r))
    return r['result']

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--pid',required=True,type=int)
    p.add_argument('--pre',type=int,default=256);p.add_argument('--post',type=int,default=4)
    p.add_argument('--minimum-seconds',type=float,default=1.0)
    p.add_argument('--auto-trigger-after',type=float,default=0,help='development canary only; 0 means manual hotkey')
    p.add_argument('--identity-file',type=Path)
    a=p.parse_args()
    if not (4<=a.pre<=256 and 1<=a.post<=16 and math.isfinite(a.minimum_seconds) and 0<=a.minimum_seconds<=2):
        p.error('pre/post/time budget outside supported bounds')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD];kernel.WaitForSingleObject.restype=wintypes.DWORD
    kernel.CloseHandle.argtypes=[wintypes.HANDLE];kernel.CloseHandle.restype=wintypes.BOOL
    handle=kernel.OpenProcess(0x100000,False,a.pid)
    if not handle:print('Native wait handle unavailable (error %d); using exact recorder process nonce/session.'%ctypes.get_last_error(),flush=True)
    def alive():return kernel.WaitForSingleObject(handle,0)==258 if handle else True
    try:return watch(a,alive)
    finally:
        if handle:kernel.CloseHandle(handle)

def watch(a,alive):
    # Calling before entering a map would bind the menu epoch. Wait for readiness.
    deadline=time.monotonic()+600
    while time.monotonic()<deadline:
        if not alive():print('Game exited before recorder armed.',flush=True);return
        response=_request(a.pid,'get_runtime_status',{},5)
        if response.get('ok') and response.get('result',{}).get('runtime',{}).get('gameStarted'):break
        time.sleep(1)
    else:raise RuntimeError('map readiness timeout')
    ready_snapshot=response.get('result',{})
    cpu=call(a.pid,'frame_evidence',{'action':'arm','capacity':262144});session=cpu['session']
    history=call(a.pid,'frame_history',{'action':'arm','session':session,'preFrames':a.pre,'postFrames':a.post,
                                     'preMilliseconds':round(a.minimum_seconds*1000)})
    print('Arming frame history; Ctrl+Shift+C freezes one incident. Session '+session,flush=True)
    start=time.monotonic();triggered=False
    last_state=None;announced_ready=False;last_reply=time.monotonic()
    while time.monotonic()-start<7200:
        if not alive():print('Game exited; no completed incident was claimed.',flush=True);return
        response=_request(a.pid,'frame_history',{'action':'peek'},5)
        if not response.get('ok'):
            if not alive():print('Game exited; watcher closed normally.',flush=True);return
            # A live retained process may briefly restart a pipe instance. Retry
            # this read only; never re-arm or replace a session implicitly.
            if time.monotonic()-last_reply>5:
                print('Recorder connection ended or unavailable; no completed package claimed.',flush=True);return
            time.sleep(.2);continue
        peek=response['result']
        if peek.get('processNonce')!=cpu['processNonce'] or peek.get('session')!=session:
            raise RuntimeError('Recorder process/session identity changed')
        last_reply=time.monotonic()
        state=peek['state']
        if state!=last_state:print('Recorder state: '+str(state),flush=True);last_state=state
        if state==2 and not announced_ready and peek.get('bufferedMs',0)>=a.minimum_seconds*1000:
            print('Ready: at least %.2f seconds buffered. Press Ctrl+Shift+C after the anomaly.'%a.minimum_seconds,flush=True);announced_ready=True
        if a.auto_trigger_after>0 and not triggered and state==2 and announced_ready and time.monotonic()-start>=a.auto_trigger_after:
            call(a.pid,'frame_history',{'action':'trigger','session':session});triggered=True
        if state==6:break
        if state==7:raise RuntimeError('history fault: '+str(call(a.pid,'frame_history',{'action':'status'})))
        time.sleep(.5)
    else:raise RuntimeError('bounded watcher timeout; no capture claimed')
    history=call(a.pid,'frame_history',{'action':'export_manifest','session':session})
    print('Images saved; exporting frozen CPU and GPU inputs.',flush=True)
    cpu=call(a.pid,'frame_evidence',{'action':'export','session':session})
    hp,cp=Path(history['manifest']),Path(cpu['path'])
    history_data=load(hp);cpu_data=load(cp)
    root=hp.parent
    identities={'processId':a.pid,'processNonce':cpu['processNonce'],'session':session,
                'runtimeReady':ready_snapshot,'loadedModuleHashVerified':False}
    if getattr(a,'identity_file',None):
        initial=load(a.identity_file);identities['launcherPreflight']=initial;after=[]
        for pin in initial.get('files',[]):
            file=Path(pin['path'])
            with file.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest().upper()
            after.append(dict(path=str(file),size=file.stat().st_size,sha256=digest))
            if digest!=pin['sha256'] or file.stat().st_size!=pin['size']:raise RuntimeError('launcher identity changed: '+str(file))
        identities['after']=after
    with (root/'capture-identities.json').open('x',encoding='utf-8') as f:json.dump(identities,f,indent=2,allow_nan=False)
    with (root/'cpu-events.json').open('x',encoding='utf-8') as f:json.dump(cpu_data,f)
    with (root/'cpu-analysis.json').open('x',encoding='utf-8') as f:json.dump(analyze(cpu_data),f,indent=2)
    result=analyze_history(history_data,cpu_data)
    with (root/'history-analysis.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print('Checking frozen raw-input coverage offline; no new GPU capture.',flush=True)
    inputs=cpu.get('inputs',{})
    if not inputs.get('ok'):raise RuntimeError('input export failed: '+str(inputs))
    if inputs.get('manifest'):
        mp,bp=Path(inputs['manifest']),Path(inputs['binary'])
        if mp.parent.resolve()!=cp.parent.resolve() or bp.parent.resolve()!=cp.parent.resolve():raise RuntimeError('input output root mismatch')
        for src,name in ((mp,'inputs.json'),(bp,'inputs.bin')):
            with src.open('rb') as f,(root/name).open('xb') as g:shutil.copyfileobj(f,g)
        input_result=analyze_inputs(load(mp),bp.read_bytes(),cpu_data)
        with (root/'inputs-analysis.json').open('x',encoding='utf-8') as f:json.dump(input_result,f,indent=2,allow_nan=False)
        print('Raw-input draws reconstructed: %d; focused %d/%d; omitted %d; capture drops %d.'%
              (input_result['reconstructedDraws'],input_result['focusReconstructed'],input_result['focusDraws'],
               input_result['omittedDraws'],input_result['captureDrops']),flush=True)
    print('Incident saved: '+str(root),flush=True)
    call(a.pid,'frame_history',{'action':'acknowledge','session':session})
    print('Bounded Stage11 geometry and matrices included when enabled. Pixel draw-ID, alpha texture pixels and intermediate attachments remain unavailable; no cause is claimed.',flush=True)
    # Deliberately retain frozen state. Do not delete/overwrite/re-arm automatically.
if __name__=='__main__':
    try:main()
    except Exception as exc:
        print('Recorder error: '+str(exc),flush=True)
        raise SystemExit(1)
