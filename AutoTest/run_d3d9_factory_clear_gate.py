"""Real adapter, Clear, vertex input and fixed-function GPU execution, without memory measurements.

Public COM local/remote equivalence on a restricted non-input Desktop. This is
one compatibility gate, not complete Warcraft integration or migration savings.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys
import render_lab_vvl365 as vvl
from run_render_host_validation_gate import check_loader
from render_lab_layer_source import create_empty_source, empty_directory_witness
from run_d3d9_draw_batch_gate import (ROOT, COMPILERS, PROBES, PLAYER, REFERENCE,
    identity, pe_identity, require, strict_json, child_settled, execute, sources,
    IsolatedRenderDesktop, validate_probe_receipt, check_launch, json_lines,
    singleton, ENV_ALLOW, capture_raw_artifacts, verify_raw_artifacts, prefixed, check_stream_metrics,
    FIXED_BATCH_CAPACITY, fixed_batch_rows)


def loader_file_version(path):
    """major.minor.patch from the frozen loader's own VS_FIXEDFILEINFO, not from its log."""
    import ctypes
    from ctypes import wintypes
    ver=ctypes.WinDLL('version')
    ver.GetFileVersionInfoSizeW.argtypes=[wintypes.LPCWSTR,ctypes.POINTER(wintypes.DWORD)]
    ver.GetFileVersionInfoSizeW.restype=wintypes.DWORD
    ver.GetFileVersionInfoW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p]
    ver.GetFileVersionInfoW.restype=wintypes.BOOL
    ver.VerQueryValueW.argtypes=[ctypes.c_void_p,wintypes.LPCWSTR,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(wintypes.UINT)]
    ver.VerQueryValueW.restype=wintypes.BOOL
    size=ver.GetFileVersionInfoSizeW(str(path),None);require(size,'loader version resource missing')
    buf=ctypes.create_string_buffer(size)
    require(ver.GetFileVersionInfoW(str(path),0,size,buf),'loader version resource unreadable')
    ptr=ctypes.c_void_p();length=wintypes.UINT()
    require(ver.VerQueryValueW(buf,'\\',ctypes.byref(ptr),ctypes.byref(length)) and length.value>=52,'loader fixed version missing')
    info=ctypes.cast(ptr,ctypes.POINTER(ctypes.c_uint32*13)).contents
    require(info[0]==0xFEEF04BD,'loader VS_FIXEDFILEINFO signature')
    return f'{info[2]>>16}.{info[2]&0xFFFF}.{info[3]>>16}'


def guarded_identity(path):
    """Frozen inputs are compared, never created: an absent player DLL must stay absent."""
    if not path.exists():
        return {'path': str(path), 'exists': False}
    return identity(path)

STREAM_ENV=dict(WARVK_D3D9_BATCH_DRAWS='1',WARVK_D3D9_ASYNC_BATCH='1',WARVK_D3D9_ASYNC_EVENTS='1',WARVK_D3D9_STREAM='1')
BACKING_ENV='WARVK_D3D9_X86_BACKING_KIB'
WINDOW_ENV='WARVK_D3D9_UPLOAD_WINDOW_KIB'
# The product x86 memory profile (d3d9_upload_window.h, pinned by the wire
# test): the residency cap and the idle write-only Lock window, in KiB.
DEFAULT_BACKING_KIB,DEFAULT_WINDOW_KIB=32768,1024
# label -> (in-process reference, x86 budget KiB or None, stream profile, Lock window KiB or None)
BACKING_CASES=(('backing-reference',True,None,False,None),('backing-remote',False,None,False,None),
    ('backing-remote-0',False,0,False,None),('backing-stream-8192',False,8192,True,None),
    ('backing-remote-0-window',False,0,False,DEFAULT_WINDOW_KIB),
    ('backing-stream-default',False,DEFAULT_BACKING_KIB,True,DEFAULT_WINDOW_KIB))
# The memory workload's Locks that hold no defined byte: every texture level's
# first LockRect and the first Lock of the vertex buffer.
BACKING_WINDOW_LOCKS=(1,12*11)
BACKING_SAMPLES=('device','uploaded','drawn','relocked','released','closed')
BACKING_TEXTURE_BYTES=12*4*sum((1024>>i)**2 for i in range(11))
# Fixture annotation exercise: 8 draws (7 annotated), one scene, five stages
# (two of them the world scene pair), one record dropped with its refused draw,
# two scene markers the host refuses, and one session reset request that is
# never sent because the offscreen fixture issues no Present. Then content
# payloads: a 150,000-byte geometry blob (3 chunks), 96 bytes of bone matrices
# and a 40-byte replacement; three more draws carrying 5 records, 4 of them
# content references; one read, one release, 16 local refusals. The batched
# arm sends two more AnnotateDraws: the read and the release each flush a batch.
ANNOTATION_DRAWS=11
ANNOTATED_DRAWS,ANNOTATION_RECORDS=10,12
ANNOTATION_CONTENT=dict(contentUploads=3,contentChunks=5,contentBytes=150000+96+40,contentReleases=1,contentReads=1,
    contentReadBytes=8,contentRefs=4,contentRefusals=16)
# Real back-buffer extents (S1 G3): label, reference, extent, present, zero extent.
# 1902x963 is the census window; 257x129 is odd-sized and banded; 64x64 keeps
# the inline presented reply. Offscreen arms of one extent share a pixel hash.
EXTENT_CASES=(('extent-reference-1902x963',True,'1902x963',False,False),
    ('extent-remote-1902x963',False,'1902x963',False,False),
    ('extent-remote-zero-1902x963',False,'1902x963',False,True),
    ('extent-present-1902x963',False,'1902x963',True,False),
    ('extent-reference-257x129',True,'257x129',False,False),
    ('extent-remote-257x129',False,'257x129',False,False),
    ('extent-present-64x64',False,'64x64',True,False))
EXTENT_IMAGES,EXTENT_DRAWS,EXTENT_FRAMES=8,6,3
# D3D9 Reset (S1 G4), same fixture: label, reference, extent, reset extent,
# present, reset with a zero extent, swap chain options. 'game' is Game.dll's
# own swap chain (X8R8G8B8, D16, IMMEDIATE); 'a8' goes from a banded extent to
# an inline one. Offscreen arms of one group share a pixel hash.
GAME_SWAPCHAIN=('--format','x8','--depth','d16','--interval','immediate')
RESET_CASES=(('reset-reference-game',True,'1902x963','1280x720',False,False,GAME_SWAPCHAIN),
    ('reset-remote-game',False,'1902x963','1280x720',False,False,GAME_SWAPCHAIN),
    ('reset-present-game',False,'1902x963','1280x720',True,False,GAME_SWAPCHAIN),
    ('reset-reference-a8',True,'257x129','64x64',False,True,()),
    ('reset-remote-a8',False,'257x129','64x64',False,True,()),
    ('reset-present-a8',False,'257x129','64x64',True,True,()))
# Per extent: 8 extent images and 6 draws, then the managed texture and the
# destination-alpha image (1 draw each); one image of the resized window
# before the Reset; checks in the fixture's own order.
RESET_PHASE_IMAGES,RESET_PHASE_DRAWS,RESET_CHECKS=EXTENT_IMAGES+2,EXTENT_DRAWS+2,27
# The window's own WM_SIZE (S1 G7), same fixture with --resize: label,
# reference, extent, resized client extent, present, swap chain options, extra
# environment. WarVK's window hook resets the swap chain without a Reset; the
# 'game' group grows the window, 'a8' shrinks it to the inline extent. The
# resize adds a cleared image and one of a kept clip quad (one draw) between
# the two reset-phase workloads. Offscreen arms of one group share a pixel hash.
RESIZE_CASES=(('resize-reference-game',True,'1280x720','1902x963',False,GAME_SWAPCHAIN,{}),
    ('resize-remote-game',False,'1280x720','1902x963',False,GAME_SWAPCHAIN,{}),
    ('resize-remote-stream-game',False,'1280x720','1902x963',False,GAME_SWAPCHAIN,STREAM_ENV|dict(WARVK_D3D9_FIXED_BATCH='1')),
    ('resize-present-game',False,'1280x720','1902x963',True,GAME_SWAPCHAIN,{}),
    ('resize-reference-a8',True,'257x129','64x64',False,(),{}),
    ('resize-remote-a8',False,'257x129','64x64',False,(),{}),
    ('resize-present-a8',False,'257x129','64x64',True,(),{}))
RESIZE_IMAGES,RESIZE_DRAWS,RESIZE_CHECKS=2,1,13
# EndScene emits no FlushDraws without GPU grouping or streaming: the
# stale-viewport arm's flushPublished witnesses its partial ClearRegions.
WINDOW_LIFE_RING=dict(WARVK_D3D9_BATCH_DRAWS='1',WARVK_D3D9_ASYNC_BATCH='1',WARVK_D3D9_COMMAND_RING='1',
    WARVK_D3D9_PROFILE='presenter-isolated-v1')
WINDOW_LIFE_P2=dict(WARVK_D3D9_ASYNC_UPLOADS='1',WARVK_D3D9_ASYNC_FLUSH='1',WARVK_D3D9_PRESENT_AHEAD='1')
WINDOW_LIFE_CASES=(('destroy-ahead',('--extent','640x480','--destroy-ahead'),EXTENT_FRAMES+1),
    ('stale-viewport',('--extent','640x480','--resize','320x240','--stale-viewport'),2))
# Game.dll's dynamic buffers (S1 G5), same fixture with --ring at 64x64 and
# the game's swap chain: label, reference, extra environment. All arms share a
# pixel hash. RING_* mirror the fixture's rings and lock sizes. The arms also
# run --depth-bias (S1 G2b): one image of five draws from a static VB.
RING_CASES=(('ring-reference',True,{}),('ring-remote',False,{}),('ring-remote-stream',False,STREAM_ENV),
    ('ring-remote-stream-fixed',False,STREAM_ENV|dict(WARVK_D3D9_FIXED_BATCH='1')),
    # The product x86 memory profile on the game's ring pattern.
    ('ring-remote-stream-default',False,STREAM_ENV|{BACKING_ENV:str(DEFAULT_BACKING_KIB),WINDOW_ENV:str(DEFAULT_WINDOW_KIB)}))
RING_EXTENT,RING_FRAMES,RING_DRAWS=64,8,8
RING_SIZES=dict(plain=524288,colored=589824,index=98304)
RING_STATIC_UPLOADS=3 # Prepare's screen and clip quads and the depth-bias quads: plain Locks on static buffers.
RING_BIAS_IMAGES,RING_BIAS_DRAWS=1,5
# Ring FVFs and the depth-bias FVF: one host declaration each (native m_fvfTable).
RING_DECLARATIONS=3
# Draw-time state syncs: the first sampler state and two DEPTHBIAS changes.
RING_STATE_SYNCS=3
UPLOAD_CHUNK=65408 # d9::MaxPayload: Unlock sends its range in chunks; only the first DISCARDs.
def ring_expectation():
    # The fixture's Reserve(): NOOVERWRITE appends, DISCARD at 0 when a Lock does not fit.
    rings={k:dict(size=v,offset=0,written=0,used=False) for k,v in RING_SIZES.items()}
    e=dict(locks=0,discards=0,wraps=0,bytes=0,carried=0,chunks=0)
    def reserve(name,size):
        r=rings[name]
        if not r['used'] or r['offset']+size>r['size']:
            e['wraps']+=r['used'];e['discards']+=1;e['carried']+=r['written'];r['offset']=0
        r['used']=True;r['offset']+=size;r['written']=max(r['written'],r['offset'])
        e['locks']+=1;e['bytes']+=size;e['chunks']+=-(-size//UPLOAD_CHUNK)
    for _ in range(RING_FRAMES):
        for draw in range(RING_DRAWS):
            if draw%2==0:reserve('plain',5120*32)
            else:reserve('colored',88*36)
            reserve('index',4096*2)
            if draw==3:reserve('colored',3500*36)
    return e
def ring_fixed_batches():
    # S1 G6, asynchronous profile: staged fixed draws stay staged across
    # NOOVERWRITE chunks and flush before a DISCARD chunk, at EndScene and at
    # capacity. The depth-bias image then flushes at each DEPTHBIAS change.
    rings={k:dict(size=v,offset=0,used=False) for k,v in RING_SIZES.items()}
    batches=[];state=dict(staged=0,passed=0)
    def flush():
        if state['staged']:batches.append(state['staged']);state['staged']=0
    def lock(name,size):
        r=rings[name];chunks=-(-size//UPLOAD_CHUNK)
        if not r['used'] or r['offset']+size>r['size']:
            r['offset']=0;flush();chunks-=1
        r['used']=True;r['offset']+=size
        if state['staged']:state['passed']+=chunks
    for _ in range(RING_FRAMES):
        for draw in range(RING_DRAWS):
            if draw%2==0:lock('plain',5120*32)
            else:lock('colored',88*36)
            lock('index',4096*2)
            state['staged']+=1
            if state['staged']==FIXED_BATCH_CAPACITY:flush()
            if draw==3:lock('colored',3500*36)
        flush()
    return batches+[2,1,2],state['passed']
def readback_bands(width,height):
    return 1 if width*height*4<=65440 else -(-height//(65440//(width*4)))
# ExerciseTopologies: 8 membership images + 2 hash-only line images, 13 draws.
TOPOLOGY_DRAWS=13
# Shared-memory command ring (tools/render_host/d3d9_command_ring.h): default capacity.
COMMAND_RING_ENV=dict(WARVK_D3D9_COMMAND_RING='1')
COMMAND_RING_BYTES=4*1024*1024
# Real-host ring sessions (test_render_host_ring_faults.cpp): case, the terminal
# cause x86 must report (kind, detail, result), whether the host names the fault
# itself, the records it executed before stopping, and its exit code (4: self
# fail-stop; 77: killed by the fixture; None: recorded, not pinned).
RING_FAULTS=(
    ('healthy','none','',0,False,2003,0),
    ('record-header','ring-corrupt','record-header',0,True,1,4),
    ('truncated','ring-corrupt','truncated',0,True,1,4),
    ('tail-invalid','ring-corrupt','tail-invalid',0,True,1,4),
    ('checksum','ring-corrupt','checksum',0,True,1,4),
    ('message-header','ring-corrupt','message-header',0,True,1,4),
    ('scope','ring-corrupt','scope',0,True,1,4),
    ('sequence','ring-corrupt','sequence',0,True,1,4),
    ('async-refused','host-rejected','async-refused',0x8876086c,True,2,4),
    # A refused scene record is advice lost, not a fault: the host counts it, says so in its ring
    # block and executes everything else to a clean Finish (13 records: barrier, two markers,
    # eight clears, barrier, Finish).
    ('scene-refused','none','',0,False,13,0),
    # The same inside an open world scene, annotations negotiated: enter, a nested enter (refused),
    # the exit (ignored), then a map change's session reset, which must still go through (7 records:
    # barrier, three markers, barrier, SessionReset, Finish).
    ('scene-stopped-reset','none','',0,False,7,0),
    ('resource-reuse','none','',0,False,13,0),
    ('resource-unnegotiated-create','host-rejected','async-refused',0x8876086c,True,2,4),
    ('resource-unnegotiated-destroy','host-rejected','async-refused',0x8876086c,True,3,4),
    ('resource-full','host-rejected','async-refused',0x8876017c,True,3,4),
    ('resource-allocation','host-exited','host process exited',0,False,None,4),
    ('resource-after-apply','host-exited','host process exited',0,False,None,4),
    ('resource-device-lost','host-device-lost','device-lost',0x88760868,False,None,4),
    ('resource-hung','host-hung','no progress',0,False,None,None),
    ('resource-format','host-rejected','async-refused',0x8876086c,True,2,4),
    ('resource-duplicate','host-rejected','async-refused',0x8876086c,True,3,4),
    ('resource-wrong-type','host-rejected','async-refused',0x8876086c,True,3,4),
    ('resource-stale','host-rejected','async-refused',0x8876086c,True,5,4),
    ('host-killed','host-exited','host process exited',0,False,None,77),
    ('host-suspended','host-hung','no progress',0,False,None,None),
)
# The views the ring adds to x86: control page + ring, control page + reply slot.
RING_VIEW_BYTES=(4096+65536,4096+COMMAND_RING_BYTES)


def annotation_rows(fixture_rows,proxy_rows,host_rows,proxy_content,host_content,annotate,batched):
    """Fixture-folded annotation and content sequence against the proxy and host accounts."""
    af=singleton(fixture_rows,'warvk-d3d9-annotation-fixture/v1')
    require(af['annotate'] is annotate and af['localRefusals']==(13 if annotate else 4)
        and af['draws']==(ANNOTATION_DRAWS if annotate else 0) and af['annotatedDraws']==(ANNOTATED_DRAWS if annotate else 0)
        and af['records']==(ANNOTATION_RECORDS if annotate else 0)
        and af['frameScenes']==int(annotate) and af['stageMarkers']==(5 if annotate else 0)
        and af['remoteRefusals']==(2 if annotate else 0) and af['sessionResetRequests']==int(annotate),
        'fixture annotation coverage')
    if not annotate:
        # Not negotiated: the three content exports refuse locally.
        require(af['contentRefusals']==3 and not any(af[k] for k in ANNOTATION_CONTENT if k!='contentRefusals'),
            'content without negotiation')
        require(not proxy_rows and not host_rows and not proxy_content and not host_content,'annotation account without negotiation')
        return dict(fixture=af)
    require(all(af[k]==v for k,v in ANNOTATION_CONTENT.items()),'fixture content coverage')
    ap=singleton(proxy_rows,'warvk-d3d9-annotation-proxy/v1');ah=singleton(host_rows,'warvk-d3d9-annotation-host/v1')
    # Unbatched: one AnnotateDraws per annotated draw. Batched: BeforeUi splits
    # two batches, and the content read and release each flush one more.
    require(ap['annotatedDraws']==af['annotatedDraws'] and ap['records']==af['records']
        and ap['messages']==(4 if batched else af['annotatedDraws'])
        and ap['frameScenes']==af['frameScenes'] and ap['stageMarkers']==af['stageMarkers'] and ap['discardedRecords']==1
        and ap['sessionResetsRequested']==af['sessionResetRequests'] and ap['sessionResetsSent']==ap['sessionResetsRefused']==0,
        'proxy annotation transport')
    require(ah['annotatedDraws']==af['annotatedDraws'] and ah['drawRecords']==af['records'] and ah['frameScenes']==af['frameScenes']
        and ah['stageMarkers']==af['stageMarkers'] and ah['digest']==af['digest']
        and ah['worldSceneEnters']==ah['worldSceneExits']==1 and ah['worldSceneActive'] is False
        and ah['sessionResets']==ah['sessionGeneration']==0,'host received the exact annotated sequence')
    cp=singleton(proxy_content,'warvk-d3d9-content-proxy/v1');ch=singleton(host_content,'warvk-d3d9-content-host/v1')
    # x86 holds identities only; the host holds the one blob left live (the 40-byte replacement).
    require(cp['uploads']==ch['uploads']==af['contentUploads'] and cp['chunks']==ch['chunks']==af['contentChunks']
        and cp['uploadedBytes']==ch['uploadedBytes']==af['contentBytes'] and cp['releases']==ch['released']==af['contentReleases']
        and cp['reads']==ch['reads']==af['contentReads'] and cp['readBytes']==ch['readBytes']==af['contentReadBytes']
        and cp['refs']==ch['boundRefs']==af['contentRefs'] and cp['localRefusals']==af['contentRefusals']
        and cp['blobs']==ch['blobs']==1 and cp['bytes']==ch['bytes']==40 and cp['lastRevision']==ch['lastRevision']==3
        and cp['cleared']==ch['cleared']==ch['aborted']==ch['refusedRefs']==0 and ch['replaced']==1
        and ch['peakBlobs']==2 and ch['peakBytes']==150000+96,'content payloads uploaded once and named by draws')
    return dict(fixture=af,proxy=ap,host=ah,contentProxy=cp,contentHost=ch)


def window_rows(proxy_rows,window_kib):
    """Write-only Lock windows: all returned, and the idle side within its budget."""
    if window_kib is None:
        require(not proxy_rows,'upload window account without the setting')
        return None
    pw=singleton(proxy_rows,'warvk-d3d9-upload-window-proxy/v1')
    require(pw['budgetBytes']==window_kib*1024 and pw['acquires']==pw['releases']==pw['allocations']+pw['reuses']
        ==pw['bufferLocks']+pw['textureLocks']>0 and pw['outstanding']==pw['outstandingBytes']==0
        and pw['idleBytes']<=pw['budgetBytes'] and pw['peakIdleBytes']<=pw['budgetBytes']
        and pw['peakOutstandingBytes']>=65536,'upload windows returned and bounded')
    return pw


def command_ring_rows(proxy_rows,terminal_rows,host_rows,enabled,proxy,host):
    """Both ends' accounts of the command ring, or no account at all on the pipe."""
    if not enabled:
        require(not proxy_rows and not terminal_rows and not host_rows,'command ring account without the profile')
        return None
    rp=singleton(proxy_rows,'warvk-d3d9-command-ring-proxy/v1');rt=singleton(terminal_rows,'warvk-d3d9-terminal/v1')
    rh=singleton(host_rows,'warvk-d3d9-command-ring-host/v1')
    # Factory queries, Initialize and AttachRing travel on the pipe; everything after them is a record.
    require(rp['attached'] is True and rp['attachSequence']>=2 and rp['capacity']==rh['capacity']==COMMAND_RING_BYTES,
        'command ring attached after Initialize')
    require(rp['records']==rh['records']>0 and rp['syncCalls']==rh['syncRecords']>0 and rp['finishExecuted'] is True
        and rp['published']==rp['applied']==rp['attachSequence']+rp['records'],'every ring record executed in order')
    require(proxy['exchanges']==rp['published']==host['accepted']+host['rejected'],'ring command balance')
    require(rt['kind']=='none' and rt['result']==0 and rt['hostExit']==0 and rt['published']==rt['applied']==rp['published'],
        'no terminal cause')
    return dict(proxy=rp,host=rh,terminal=rt)


def raw_annotation_rows(report,host_rows,host_content,annotate,asynchronous):
    """Raw peer's folded sequence; refusals terminal under async batches are skipped."""
    # Scene marker and session reset refusals are synchronous: 11 in either profile.
    if annotate:rejected,draws=(20,5) if asynchronous else (38,23)
    else:rejected,draws=(3 if asynchronous else 4),0
    # Content payloads (same fixture). Synchronous in every profile: 11 read
    # refusals and 8 draws refused for a reference that names nothing. Only
    # without async batches: 13 first chunks, 2 dropped uploads (2 refusals
    # each), 5 releases, and one more draw that binds after the drops.
    extra=0 if asynchronous else 1
    if annotate:
        payload=dict(payloadRejected=19+22*extra,payloadDraws=11+extra,payloadUploads=4,payloadChunks=6+2*extra,
            payloadChunkBytes=150000+96+40+96+80*extra,payloadReads=2,payloadReadBytes=8+UPLOAD_CHUNK,payloadReleases=1,
            payloadRefs=5+extra,payloadRefusedRefs=8,payloadAborted=2*extra,payloadRevision=4+2*extra)
    else:
        payload=dict(payloadRejected=1+2*extra,payloadDraws=0,payloadUploads=0,payloadChunks=0,payloadChunkBytes=0,payloadReads=0,
            payloadReadBytes=0,payloadReleases=0,payloadRefs=0,payloadRefusedRefs=0,payloadAborted=0,payloadRevision=0)
    require(all(report[k]==v for k,v in payload.items()),'raw content payload coverage')
    bound_draws=3+extra if annotate else 0
    require(report['annotate'] is annotate and report['annotationRejected']==rejected and report['annotationDraws']==draws
        and report['annotatedDraws']==(4+bound_draws if annotate else 0)
        and report['annotationRecords']==(4+payload['payloadRefs'] if annotate else 0)
        and report['frameScenes']==int(annotate) and report['stageMarkers']==(5 if annotate else 0)
        and report['sessionResets']==(3 if annotate else 0),'raw annotation coverage')
    if not annotate:
        require(not host_rows and not host_content and report['annotationDigest']=='cbf29ce484222325',
            'raw annotations without negotiation')
        return dict()
    ah=singleton(host_rows,'warvk-d3d9-annotation-host/v1')
    require(ah['annotatedDraws']==report['annotatedDraws'] and ah['drawRecords']==report['annotationRecords']
        and ah['frameScenes']==report['frameScenes'] and ah['stageMarkers']==report['stageMarkers']
        and ah['sessionResets']==report['sessionResets'] and ah['sessionGeneration']==(1<<32)+2
        and ah['worldSceneEnters']==ah['worldSceneExits']==1 and ah['worldSceneActive'] is False
        and ah['digest']==report['annotationDigest'],'raw annotations delivered exactly')
    ch=singleton(host_content,'warvk-d3d9-content-host/v1')
    # The reset dropped the one blob left (the replacement); one upload followed it.
    require(ch['uploads']==report['payloadUploads'] and ch['chunks']==report['payloadChunks']
        and ch['uploadedBytes']==report['payloadChunkBytes'] and ch['reads']==report['payloadReads']
        and ch['readBytes']==report['payloadReadBytes'] and ch['released']==report['payloadReleases']
        and ch['boundRefs']==report['payloadRefs'] and ch['refusedRefs']==report['payloadRefusedRefs']
        and ch['aborted']==report['payloadAborted'] and ch['lastRevision']==report['payloadRevision']
        and ch['replaced']==ch['cleared']==ch['blobs']==1 and ch['bytes']==96 and ch['peakBlobs']==2
        and ch['peakBytes']==150000+96,'raw content payloads stored, bound and dropped exactly')
    return dict(host=ah,contentHost=ch)


def backing_rows(proxy_rows,host_rows,budget_kib):
    """x86 residency ledger against the host's retained copies (KiB budget or None).

    A budget at or above the product default holds these workloads whole, so
    nothing need be evicted there."""
    hb=singleton(host_rows,'warvk-d3d9-backing-host/v1');enabled=budget_kib is not None
    require(hb['retainContent'] is enabled and hb['retainedBytes']==0
        and (hb['peakRetainedBytes']>0) is enabled,'host retained content mode')
    if proxy_rows is None:
        require(hb['contentReads']==hb['contentReadBytes']==0,'content read without a device')
        return dict(host=hb)
    pb=singleton(proxy_rows,'warvk-d3d9-backing-proxy/v1')
    require(pb['enabled'] is enabled and pb['budgetBytes']==(budget_kib*1024 if enabled else 0)
        and pb['residentBytes']==pb['listedBytes']==0 and pb['allocations']>0 and pb['peakResidentBytes']>0,
        'x86 content copies released')
    require(pb['fetchCalls']==hb['contentReads'] and pb['fetchedBytes']==hb['contentReadBytes']
        and pb['fetches']<=pb['fetchCalls'],'every x86 restore came from the host copy')
    if enabled:
        require(pb['maxListedAfterTrim']<=pb['budgetBytes']
            and (budget_kib>=DEFAULT_BACKING_KIB or (pb['evictions']>0 and pb['evictedBytes']>0)),
            'unlocked x86 copies within budget')
        if budget_kib==0:require(pb['fetches']>0,'zero budget restored no content')
    else:
        require(pb['evictions']==pb['evictedBytes']==pb['fetches']==pb['fetchCalls']==pb['discardMisses']
            ==pb['maxListedAfterTrim']==pb['peakPinnedBytes']==0,'legacy full x86 copies')
    return dict(proxy=pb,host=hb)


def remote_environment(pairs):
    """--remote-env NAME=VALUE: x64 switch variables for the remote arms only."""
    result = {}
    for pair in pairs:
        name, separator, value = pair.partition('=')
        if not separator or not name.startswith('WARVK_D3D9_') or not value:
            raise ValueError('--remote-env takes WARVK_D3D9_NAME=VALUE')
        result[name] = value
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-receipt',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--validation',action='store_true',help='pinned VVL365 for remote GPU cases; no performance or memory measurement')
    ap.add_argument('--repin-loader',action='store_true',help='with --validation: accept the current system vulkan-1.dll '
                    '(driver updates replace it) and record both identities; the VVL layer manifest and library stay pinned')
    ap.add_argument('--reference-dll',type=Path,help='fresh isolated x86 product candidate; default is the preserved reference')
    ap.add_argument('--remote-env',action='append',default=[],metavar='WARVK_D3D9_NAME=VALUE',
        help='extra variable for the remote (proxy and host) arms only, e.g. a switch turned off for a control round')
    ap.add_argument('--ring-env',action='append',default=[],metavar='WARVK_D3D9_NAME=VALUE',
        help='extra variable for the remote arms that run asynchronous batches over the command ring only '
             '(the ordered asynchronous switches are refused anywhere else)')
    ap.add_argument('--fixed-env',action='append',default=[],metavar='WARVK_D3D9_NAME=VALUE',
        help='extra variable for the remote arms that run fixed-function batches only '
             '(the delta encoding of those batches is refused anywhere else)')
    ap.add_argument('--optimized-pipelines',action='store_true',help='disable GPL in both owners to require the optimized specialization path')
    ap.add_argument('--stream',action='store_true',help='add the host stream profile: full-image case and a second raw fault case')
    ap.add_argument('--fixed-batch',action='store_true',help='add grouped/stream fixed-function batch arms and a synchronous stream raw fault case')
    ap.add_argument('--backing',action='store_true',help='budgeted x86 content arms, the x86 memory workload and a retained-content raw fault case')
    ap.add_argument('--annotations',action='store_true',help='data-layer annotation arms (sync and stream fixed batch) and a raw annotation fault case')
    ap.add_argument('--extent',action='store_true',help='real back-buffer extents: offscreen arms vs the original renderer, and presented frames')
    ap.add_argument('--reset',action='store_true',help="D3D9 Reset and the game's swap chain: offscreen arms vs the original renderer, and presented frames")
    ap.add_argument('--resize',action='store_true',help="the window's own WM_SIZE: WarVK's swap chain reset without the application's Reset")
    ap.add_argument('--window-life',action='store_true',help='only the isolated presenter window lifetime regressions and P2 resize witness')
    ap.add_argument('--ring',action='store_true',help="Game.dll's dynamic VB/IB rings (NOOVERWRITE, DISCARD): sync and stream arms vs the original renderer")
    ap.add_argument('--command-ring',action='store_true',help='shared-memory command ring: its CPU test, one ring arm beside each '
                    'pipe arm (sync, grouped, and the stream/fixed/annotate arms when enabled) and ring raw fault cases')
    args=ap.parse_args();out=args.output.resolve()
    if args.window_life and any((args.stream,args.fixed_batch,args.backing,args.annotations,args.extent,args.reset,
            args.resize,args.ring,args.command_ring,args.remote_env,args.ring_env,args.fixed_env)):
        ap.error('--window-life supplies its own workloads and ring environments')
    if args.repin_loader and not args.validation:ap.error('--repin-loader needs --validation')
    require(not args.fixed_batch or args.stream,'fixed batch arms extend the stream comparison')
    reference_dll=args.reference_dll.resolve() if args.reference_dll else REFERENCE
    require(reference_dll.is_relative_to(ROOT) and reference_dll.suffix.lower()=='.dll','isolated reference DLL below repository')
    require(out.parent==ROOT/'AutoTest/artifacts' and not out.exists(),'fresh output only');out.mkdir()
    receipt=dict(schema='warvk-d3d9-factory-clear-gate/v1',ok=False,gameTouched=False,
        fullRendererMigrated=False,memoryMeasured=False,steps=[],cases=[],raw=[],copies=[])
    remote_extra=remote_environment(args.remote_env);receipt['remoteEnvironment']=remote_extra
    ring_extra=remote_environment(args.ring_env);receipt['ringEnvironment']=ring_extra
    fixed_extra=remote_environment(args.fixed_env);receipt['fixedEnvironment']=fixed_extra
    frozen=[];env={k:v for k,v in os.environ.items() if k.upper() in ENV_ALLOW}
    env.update(DXVK_LOG_LEVEL='info',DXVK_SHADER_CACHE='0')
    def step(argv,label):
        try:receipt['steps'].append(execute(argv,ROOT,out/label,180,env))
        finally:receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']))
    def copy(a,b):
        before=identity(a);shutil.copy2(a,b);after=identity(b)
        require((before['bytes'],before['sha256'])==(after['bytes'],after['sha256']),'exact copy')
        receipt['copies'].append(after)
    def config(folder):
        path=folder/'pipeline.conf'
        path.write_text('dxvk.enableGraphicsPipelineLibrary = False\n' if args.optimized_pipelines else '',encoding='utf-8')
        frozen.append(identity(path))
        return str(path)
    try:
        build=strict_json(args.build_receipt.read_text('utf-8'))
        require(build['ok'] and build['sourcesPreserved'],'frozen host build')
        require(all(identity(Path(r['path']))==r for r in build['sources']),'host source drift')
        host=Path(build['host']['path']);require(identity(host)==build['host'],'host artifact drift')
        frozen=[identity(p) for p in sources()]
        frozen += [guarded_identity(p) for p in (Path(__file__).resolve(),ROOT/'AutoTest/test_render_host_d3d9_factory_clear.cpp',
            ROOT/'AutoTest/test_render_host_d3d9_parent.cpp',ROOT/'AutoTest/test_render_host_d3d9_wire.cpp',ROOT/'AutoTest/test_render_host_fvf.h',
            ROOT/'AutoTest/test_render_host_fixed_rejection.cpp',ROOT/'AutoTest/test_render_host_d3d9_resource_lifetime.cpp',
            ROOT/'AutoTest/test_d3d9_fixed_specialization.cpp',
            ROOT/'AutoTest/test_render_host_texture.h',
            ROOT/'AutoTest/test_render_host_texture_formats.h',
            ROOT/'AutoTest/test_render_host_sampler.h',ROOT/'AutoTest/test_render_host_sampler_gpu.h',
            ROOT/'AutoTest/test_render_host_effects.h',ROOT/'AutoTest/test_render_host_effects_gpu.h',
            ROOT/'AutoTest/test_render_host_texture_stages.h',ROOT/'AutoTest/test_render_host_texture_stages_gpu.h',
            ROOT/'AutoTest/test_render_host_stateblock_gpu.h',
            ROOT/'AutoTest/test_render_host_d3d9_shader_tokens.cpp',ROOT/'AutoTest/test_render_host_d3d9_backing.cpp',
            ROOT/'AutoTest/test_d3d9_sampler_key.cpp',ROOT/'AutoTest/test_render_host_d3d9_extent.cpp',
            *((PLAYER,reference_dll) if not args.window_life else ()),host,PROBES/'desktop-probe-32.exe',PROBES/'desktop-probe-64.exe')]
        if not args.window_life:receipt['referenceDll']=identity(reference_dll)
        frozen += [identity(Path(m.__file__).resolve()) for m in list(sys.modules.values())
            if getattr(m,'__file__',None) and Path(m.__file__).suffix=='.py'
            and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest')]
        receipt['validationEnabled']=args.validation
        receipt['optimizedPipelinesRequired']=args.optimized_pipelines
        if args.validation:
            pinned=ROOT/'AutoTest/artifacts/x64-dispatch-vvl365-candidate-20261023-r1/receipt.json'
            layer=strict_json(pinned.read_text('utf-8'))['layer']
            if args.repin_loader:
                current=identity(Path(layer['loader']['path']))
                receipt['loaderRepin']=dict(pinned=layer['loader'],current=current,changed=current!=layer['loader'],
                    version=loader_file_version(Path(current['path'])))
                layer=dict(layer,loader=current)
            for kind in ('manifest','library','loader'):
                require(identity(Path(layer[kind]['path']))==layer[kind],'pinned VVL identity '+kind)
                frozen.append(layer[kind])
            frozen.append(identity(pinned));receipt['layer']=layer
            manifest=Path(layer['manifest']['path']);library=Path(layer['library']['path']);loader=Path(layer['loader']['path'])
            loader_version=receipt['loaderRepin']['version'] if args.repin_loader else None
            vvl.check_manifest(strict_json(manifest.read_text('utf-8')))
            receipt['implicitSourceBefore']=create_empty_source(out)
            empty=Path(receipt['implicitSourceBefore']['path'])
        receipt['inputs']=frozen.copy()
        cc=COMPILERS/'i686-w64-mingw32-clang++.exe'
        common=[ROOT/'tools/render_host'/p for p in ('protocol.cpp','win32_transport.cpp','win32_security.cpp','owned_child.cpp')]
        flags=['-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-pthread','-D_WIN32_WINNT=0x0a00']
        proxy=out/'proxy.dll';fixture=out/'fixture32.exe';fault_fixture=out/'fixed-rejection32.exe';backing_fixture=out/'backing32.exe'
        extent_fixture=out/'extent32.exe';ring_fault_fixture=out/'ring-faults32.exe'
        step([cc,*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec','-shared','-Wl,--kill-at','-DNOMINMAX',
            ROOT/'tools/render_host/d3d9_proxy.cpp',ROOT/'tools/render_host/d3d9_proxy.def',*common,
            ROOT/'tools/render_host/process_memory_probe.cpp','-luser32','-lbcrypt','-ladvapi32','-luuid','-lpsapi','-o',proxy],'build-proxy')
        if not args.window_life:
            step([cc,*flags,'-municode',ROOT/'AutoTest/test_render_host_d3d9_factory_clear.cpp','-luser32','-o',fixture],'build-fixture')
            step([cc,*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec','-DNOMINMAX','-municode',
                ROOT/'AutoTest/test_render_host_fixed_rejection.cpp',*common,
                ROOT/'tools/render_host/process_memory_probe.cpp','-luser32','-lbcrypt','-ladvapi32','-luuid','-lpsapi','-o',fault_fixture],'build-fixed-rejection')
        if args.extent or args.reset or args.resize or args.ring or args.window_life:
            step([cc,*flags,'-municode',ROOT/'AutoTest/test_render_host_d3d9_extent.cpp','-luser32','-o',extent_fixture],'build-extent')
        if args.command_ring:
            step([cc,*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec','-DNOMINMAX','-municode',
                ROOT/'AutoTest/test_render_host_ring_faults.cpp',*common,'-luser32','-lbcrypt','-ladvapi32','-o',ring_fault_fixture],
                'build-ring-faults')
            require(pe_identity(ring_fault_fixture)==(0x14c,0x10b),'x86 ring fault fixture')
        if args.backing:
            step([cc,*flags,'-municode',ROOT/'AutoTest/test_render_host_d3d9_backing.cpp',
                ROOT/'tools/render_host/process_memory_probe.cpp','-luser32','-lpsapi','-o',backing_fixture],'build-backing')
        wire=[];specialization=[];sampler_keys=[];shader_tokens=[];command_rings=[]
        cpu_targets=() if args.window_life else ((32,'i686'),(64,'x86_64'))
        for bits,target in cpu_targets:
            exe=out/f'wire{bits}.exe';wire.append((bits,exe))
            step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,'-DNOMINMAX',
                ROOT/'AutoTest/test_render_host_d3d9_wire.cpp',ROOT/'tools/render_host/protocol.cpp','-o',exe],f'build-wire{bits}')
            require(pe_identity(exe)==((0x14c,0x10b) if bits==32 else (0x8664,0x20b)),'actual wire width')
            token_exe=out/f'shader-tokens{bits}.exe';shader_tokens.append((bits,token_exe))
            step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,
                ROOT/'AutoTest/test_render_host_d3d9_shader_tokens.cpp','-o',token_exe],f'build-shader-tokens{bits}')
            require(pe_identity(token_exe)==pe_identity(exe),'actual shader-token width')
            if args.command_ring:
                ring_exe=out/f'command-ring{bits}.exe';command_rings.append((bits,ring_exe))
                step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,'-DNOMINMAX',
                    ROOT/'AutoTest/test_render_host_command_ring.cpp',ROOT/'tools/render_host/protocol.cpp','-o',ring_exe],
                    f'build-command-ring{bits}')
                require(pe_identity(ring_exe)==pe_identity(exe),'actual command ring width')
            spec=out/f'specialization{bits}.exe';specialization.append((bits,spec))
            step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec',
                '-Wno-missing-field-initializers','-Wno-missing-braces','-Wno-extern-c-compat','-Wno-unused-private-field',
                '-Wno-error=nontrivial-memcall', # Existing DXVK pipeline-state header warnings remain visible, like the product build.
                '-DNOMINMAX','-DDXVK_WSI_WIN32','-I'+str(ROOT/'include'),'-I'+str(ROOT/'include/vulkan/include'),
                '-I'+str(ROOT/'include/spirv/include'),'-I'+str(Path(build['build'])),
                ROOT/'AutoTest/test_d3d9_fixed_specialization.cpp','-o',spec],f'build-specialization{bits}')
            require(pe_identity(spec)==pe_identity(exe),'actual specialization width')
            sampler_key=out/f'sampler-key{bits}.exe';sampler_keys.append((bits,sampler_key))
            step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec',
                '-Wno-missing-field-initializers','-Wno-missing-braces','-Wno-extern-c-compat','-Wno-unused-private-field',
                '-Wno-error=nontrivial-memcall','-DNOMINMAX','-DDXVK_WSI_WIN32',
                '-I'+str(ROOT/'include'),'-I'+str(ROOT/'include/vulkan/include'),'-I'+str(ROOT/'include/spirv/include'),
                '-I'+str(Path(build['build'])),ROOT/'AutoTest/test_d3d9_sampler_key.cpp','-o',sampler_key],f'build-sampler-key{bits}')
            require(pe_identity(sampler_key)==pe_identity(exe),'actual sampler key width')
        if args.window_life:
            require(pe_identity(proxy)==pe_identity(extent_fixture)==(0x14c,0x10b),'x86 COM caller')
            frozen += [identity(proxy),identity(extent_fixture)]
        else:
            require(pe_identity(proxy)==pe_identity(fixture)==(0x14c,0x10b),'x86 COM caller')
            frozen += [identity(p) for p in (proxy,fixture,fault_fixture)+((backing_fixture,) if args.backing else ())
                +((extent_fixture,) if args.extent or args.reset or args.resize or args.ring else ())]
        frozen += [identity(p) for _,p in wire+specialization+sampler_keys+command_rings]
        if args.command_ring:frozen.append(identity(ring_fault_fixture))
        ring_pins=set()
        def run_cases(desktop):
            if remote_extra or ring_extra or fixed_extra:
                # Every case goes through desktop.run with its environment; the
                # remote arms are the ones that name the x64 host. --ring-env
                # reaches only those with asynchronous batches over the ring.
                launch_plain=desktop.run
                def launch_remote(argv,**kw):
                    given=kw.get('env') or {}
                    if 'WARVK_D3D9_HOST_EXE' in given:
                        kw['env']=given|remote_extra
                        if given.get('WARVK_D3D9_COMMAND_RING')=='1' and given.get('WARVK_D3D9_ASYNC_BATCH')=='1':
                            # A session that pins a switch itself (ring_pins) keeps its own setting.
                            kw['env']=kw['env']|{k:v for k,v in ring_extra.items() if k not in ring_pins}
                            receipt['ringEnvironmentCases']=receipt.get('ringEnvironmentCases',0)+1
                        if fixed_extra and given.get('WARVK_D3D9_FIXED_BATCH')=='1':
                            kw['env']=kw['env']|fixed_extra;receipt['fixedEnvironmentCases']=receipt.get('fixedEnvironmentCases',0)+1
                    return launch_plain(argv,**kw)
                desktop.run=launch_remote
            receipt['desktopSafety']=desktop.receipt
            probe=desktop.run([PROBES/'desktop-probe-32.exe','--expect-desktop',desktop.desktop_name,
                '--expect-station','WinSta0','--input-desktop',desktop.input_before,
                '--child',PROBES/'desktop-probe-64.exe','--child-log',out/'probe-child.log'],cwd=out,
                stdout_path=out/'probe.stdout',stderr_path=out/'probe.stderr',timeout_s=30,env=env)
            receipt['probe']=probe;receipt['raw'].append(capture_raw_artifacts(out,['probe.stdout','probe.stderr','probe-child.log']))
            check_launch(probe,desktop.desktop_name,desktop.input_before)
            rows=json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log');require(len(rows)==2,'both probes')
            validate_probe_receipt(rows[0],station='WinSta0',desktop=desktop.desktop_name,input_desktop=desktop.input_before,bits=32,child=rows[1],launch=probe)
            def extent_fixture_case(cases,label,reference,present,arguments,extra=None):
                folder=out/label;folder.mkdir();exe=folder/'fixture.exe';dll=folder/'renderer.dll'
                copy(extent_fixture,exe);copy(reference_dll if reference else proxy,dll)
                local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=config(folder))
                if not reference:
                    copy(host,folder/'host.exe');local.update(WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                if present:local['WARVK_D3D9_PROFILE']='presenter-readback-isolated-v1'
                if extra:local.update(extra)
                validate=args.validation and not reference
                if validate:local.update(vvl.validation_env(folder,manifest,empty))
                local['DXVK_CONFIG_FILE']=str(folder/'pipeline.conf')
                case=dict(label=label,ok=False);cases.append(case)
                command=[exe,'--dll',dll,*arguments]+(['--reference'] if reference else [])+(['--present'] if present else [])
                case['process']=desktop.run(command,cwd=folder,stdout_path=folder/'case.stdout',
                    stderr_path=folder/'case.stderr',timeout_s=120,env=local)
                receipt['raw'].append(capture_raw_artifacts(folder,['case.stdout','case.stderr']+([] if reference else ['host.log'])
                    +(['validation.raw.log'] if validate else []),True))
                if args.window_life:
                    case['environment']=extra
                    case['fixtureExit']=case['process'].get('exitCode')
                    host_log=folder/'host.log'
                    if host_log.is_file():
                        refusals=[line for line in host_log.read_text('utf-8').splitlines()
                            if 'D3D9 host asynchronous record refused' in line]
                        case['asyncRefusals']=len(refusals)
                        if refusals:raise ValueError(refusals[0])
                    if case['fixtureExit']!=0:
                        errors=(folder/'case.stderr').read_text('utf-8').splitlines()
                        reason=next((line for line in errors if line.startswith('extent fixture failed ')),'see case.stderr')
                        raise ValueError(f"fixture exitCode={case['fixtureExit']}: {reason}")
                check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                rows=json_lines(folder/'case.stdout');report=singleton(rows,'warvk-d3d9-extent-fixture/v1')
                return case,folder,rows,report,validate
            def extent_remote_closed(case,folder,rows,draws,rejected):
                pr=singleton(rows,'warvk-d3d9-proxy/v1');hs=singleton(json_lines(folder/'host.log'),'warvk-d3d9-host/v1')
                case['host']=hs;case['proxy']=pr
                require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                    and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit']==0,'extent remote shutdown')
                require(hs['ok'] and hs['bits']==64 and hs['normalTeardown'] and hs['liveResources']==0
                    and hs['draws']==draws and hs['submitted']==hs['gpuCompleted']==hs['retired']
                    and pr['exchanges']==hs['accepted']+hs['rejected'] and hs['rejected']==rejected,'extent host ownership closed')
                ds=singleton(json_lines(folder/'host.log'),'warvk-d3d9-desktop/v1')
                require(ds['pid']==pr['childPid'] and ds['peerPid']==case['process']['pid'] and ds['desktop']==desktop.desktop_name
                    and ds['inputDesktop']==desktop.input_before,'extent host/desktop')
                case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                return hs
            def extent_validation(case,folder):
                case['validation']=vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                case['loader']=check_loader((folder/'host.log').read_text('utf-8'),
                    (folder/'host_warvk-d3d9-host.log').read_text('utf-8'),library,empty,loader,loader_version)
            if args.window_life:
                cases=receipt['windowLifeCases']=[]
                arms=[(scenario,arguments,frames,arm) for scenario,arguments,frames in WINDOW_LIFE_CASES
                    for arm in ('control','p2')]
                arms.append(('resize',('--extent','257x129','--resize','64x64'),2*EXTENT_FRAMES,'p2'))
                for scenario,arguments,frames,arm in arms:
                    p2=arm=='p2';label=f'{scenario}-{arm}'
                    extra=WINDOW_LIFE_RING|(WINDOW_LIFE_P2 if p2 else {})
                    try:
                        options=arguments if scenario=='resize' else (*arguments,*GAME_SWAPCHAIN)
                        case,folder,rows,report,validate=extent_fixture_case(cases,label,False,True,options,extra)
                        case['result']=report
                        ordered=prefixed(folder/'case.stderr','D3D9 ordered-extra: ')
                        row=singleton(ordered,'warvk-d3d9-ordered-extra/v1') if ordered or p2 else {}
                        settles=row.get('presentAheadSettles') if row else 0
                        flush=row.get('flushPublished') if row else 0
                        ahead=row.get('presentsAhead') if row else 0
                        case.update(orderedExtra=row or None,presentAheadSettles=settles,flushPublished=flush,presentsAhead=ahead)
                        require(report['ok'] and report['normalTeardown'] and report['presentedFrames']==frames
                            and (report.get('scenario')==scenario if scenario!='resize' else report['resizeChecks']==RESIZE_CHECKS),
                            'window lifetime workload coverage')
                        if scenario=='destroy-ahead':
                            require(isinstance(report.get('destroyMs'),(int,float)) and report['destroyMs']>=0,
                                'missing window destruction duration')
                            if p2:require(type(settles) is int and settles>=1,f'expected presentAheadSettles >= 1, got {settles}')
                            else:require(settles==0,f'expected presentAheadSettles = 0, got {settles}')
                        if p2:
                            require(all(row.get(k) is True for k in ('asyncFlush','asyncUploads','presentAhead')),'P2 switches inactive')
                            if scenario=='stale-viewport':
                                require(type(flush) is int and flush>0,f'expected flushPublished > 0, got {flush}')
                        else:require(not any(row.get(k) for k in ('asyncFlush','asyncUploads','presentAhead')),
                            'control ran with P2 switches')
                        # Resize inserts one Present after WM_SIZE and ahead of an ordered Reset.
                        skips=int(scenario=='resize');attempts=frames+skips
                        require(ahead==(attempts if p2 else 0),f'unexpected presentsAhead={ahead} for {attempts} Present calls')
                        draws=2*RESET_PHASE_DRAWS+RESIZE_DRAWS+frames if scenario=='resize' else frames
                        hs=extent_remote_closed(case,folder,rows,draws,skips)
                        if scenario!='resize':
                            clears=report['backlogClears']+EXTENT_FRAMES if scenario=='destroy-ahead' else 2
                            require(hs['clears']==clears,f"expected host clears = {clears}, got {hs['clears']}")
                        rh=singleton(prefixed(folder/'host.log','D3D9 reset-host: '),'warvk-d3d9-reset-host/v1')
                        case['presentsSkipped']=rh['presentsSkipped']
                        require(rh['presentsSkipped']==skips,f"expected presentsSkipped = {skips}, got {rh['presentsSkipped']}")
                        case['commandRing']=command_ring_rows(prefixed(folder/'case.stderr','D3D9 command-ring-proxy: '),
                            prefixed(folder/'case.stderr','D3D9 terminal: '),prefixed(folder/'host.log','D3D9 command-ring-host: '),
                            True,case['proxy'],hs)
                        require(hs['presentImplemented'] and hs['presentAttempts']==attempts and hs['presentApiSucceeded']==frames
                            and hs['presentGpuCompleted']==frames and hs['presentedReadbacks']==0,'window lifetime host Presents')
                        if validate:extent_validation(case,folder)
                        case['ok']=True
                    except ValueError as error:
                        # Keep evidence from subsequent arms after an ordinary, fully settled fixture failure.
                        if not cases or cases[-1]['label']!=label:raise
                        case=cases[-1];case['error']=str(error)
                        if not case.get('process',{}).get('jobEmpty'):raise
                failures=[c['label']+': '+c['error'] for c in cases if not c['ok']]
                require(not failures,' | '.join(failures))
                return
            receipt['wireTests']=[]
            for bits,exe in wire:
                label=f'wire{bits}';launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),timeout_s=30,env=env)
                receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']));check_launch(launch,desktop.desktop_name,desktop.input_before)
                match=re.fullmatch(r'd3d9 wire checks=(\d+) failures=0 bits='+str(bits)+r' drawBytes=1164\s*',(out/(label+'.stdout')).read_text('utf-8'))
                require(match is not None and int(match[1])>700000,'complete wire/rectangle/scalar CPU test')
                receipt['wireTests'].append(dict(bits=bits,checks=int(match[1]),process=launch))
            if args.command_ring:
                # Both widths: the x86 producer and the x64 consumer share one layout.
                receipt['commandRingTests']=[]
                for bits,exe in command_rings:
                    label=f'command-ring{bits}'
                    launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),timeout_s=180,env=env)
                    receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']));check_launch(launch,desktop.desktop_name,desktop.input_before)
                    match=re.search(r'^d3d9 command ring checks=(\d+) failures=0 bits='+str(bits)+r'\s*\Z',(out/(label+'.stdout')).read_text('utf-8'),re.M)
                    require(match is not None and int(match[1])>250000,'complete command ring CPU test')
                    receipt['commandRingTests'].append(dict(bits=bits,checks=int(match[1]),process=launch))
            receipt['shaderTokenTests']=[]
            for bits,exe in shader_tokens:
                label=f'shader-tokens{bits}'
                launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),timeout_s=30,env=env)
                receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']));check_launch(launch,desktop.desktop_name,desktop.input_before)
                match=re.fullmatch(r'd3d9 shader tokens checks=(\d+) failures=0 bits='+str(bits)+r' maxWords=\d+ floatRegisters=32 restrictedSm3=1 boundedFraming=1 cpuOnly=1 dxsoCompileTested=0 gpuTested=0\s*',
                    (out/(label+'.stdout')).read_text('utf-8'))
                require(match is not None and int(match[1])==6560,'complete shader-token boundary test')
                receipt['shaderTokenTests'].append(dict(bits=bits,checks=int(match[1]),process=launch))
            receipt['specializationTests']=[]
            for bits,exe in specialization:
                label=f'specialization{bits}'
                launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),timeout_s=30,env=env)
                receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']));check_launch(launch,desktop.desktop_name,desktop.input_before)
                require((out/(label+'.stdout')).read_text('utf-8').strip()==f'fixed specialization checks=200000 failures=0 bits={bits}',
                    'production specialization vs preserved native oracle')
                receipt['specializationTests'].append(dict(bits=bits,checks=200000,process=launch))
            receipt['samplerKeyTests']=[]
            for bits,exe in sampler_keys:
                label=f'sampler-key{bits}'
                launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),timeout_s=30,env=env)
                receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']));check_launch(launch,desktop.desktop_name,desktop.input_before)
                require((out/(label+'.stdout')).read_text('utf-8').strip()==f'sampler key checks=200000 failures=0 bits={bits}',
                    'production sampler vs preserved native oracle')
                receipt['samplerKeyTests'].append(dict(bits=bits,checks=200000,process=launch))
            receipt['streamEnabled']=args.stream
            receipt['fixedBatchEnabled']=args.fixed_batch
            receipt['backingEnabled']=args.backing
            receipt['annotationsEnabled']=args.annotations
            receipt['extentEnabled']=args.extent
            receipt['resetEnabled']=args.reset
            receipt['resizeEnabled']=args.resize
            receipt['ringEnabled']=args.ring
            receipt['commandRingEnabled']=args.command_ring
            cases=[('metadata-reference',True,True,False),('metadata-remote',False,True,False),
                ('reference-before',True,False,False),('remote-sync',False,False,False),
                ('remote-grouped',False,False,True),('reference-after',True,False,False)]
            if args.stream:cases.insert(5,('remote-stream',False,False,False))
            if args.fixed_batch:cases[-1:-1]=[('remote-grouped-fixed',False,False,True),('remote-stream-fixed',False,False,False)]
            if args.backing:cases[-1:-1]=[('remote-backing-0',False,False,False),('remote-stream-backing-64',False,False,False),
                ('remote-backing-0-window-0',False,False,False),
                (f'remote-stream-backing-{DEFAULT_BACKING_KIB}-window-{DEFAULT_WINDOW_KIB}',False,False,False)]
            if args.annotations:cases[-1:-1]=[('remote-annotate',False,False,False),('remote-stream-fixed-annotate',False,False,False)]
            if args.command_ring:
                ring_arms=[('remote-sync-cmdring',False,False,False),('remote-grouped-cmdring',False,False,True)]
                if args.stream:ring_arms.append(('remote-stream-cmdring',False,False,False))
                if args.fixed_batch:ring_arms.append(('remote-stream-fixed-cmdring',False,False,False))
                if args.annotations:ring_arms.append(('remote-stream-fixed-annotate-cmdring',False,False,False))
                cases[-1:-1]=ring_arms
            for label,reference,metadata,grouped in cases:
                folder=out/label;folder.mkdir();exe=folder/'fixture.exe';dll=folder/'renderer.dll'
                copy(fixture,exe);copy(reference_dll if reference else proxy,dll)
                local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=config(folder))
                if metadata:local['WARVK_TEST_FACTORY_ONLY']='1'
                if not reference:
                    copy(host,folder/'host.exe');local.update(WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                if grouped:local.update(WARVK_D3D9_BATCH_DRAWS='1',WARVK_D3D9_ASYNC_BATCH='1',WARVK_D3D9_ASYNC_EVENTS='1',WARVK_D3D9_GPU_FLIGHTS='4',WARVK_D3D9_GPU_GROUP='4')
                stream=label.startswith('remote-stream')
                if stream:local.update(STREAM_ENV)
                fixed_batch='fixed' in label
                if fixed_batch:local['WARVK_D3D9_FIXED_BATCH']='1'
                found=re.search(r'backing-(\d+)',label);budget=int(found[1]) if found else None
                if budget is not None:local[BACKING_ENV]=str(budget)
                found=re.search(r'window-(\d+)',label);window=int(found[1]) if found else None
                if window is not None:local[WINDOW_ENV]=str(window)
                annotate='annotate' in label
                if annotate:local.update(WARVK_D3D9_ANNOTATIONS='1',WARVK_TEST_ANNOTATE='1')
                command_ring=label.endswith('-cmdring')
                if command_ring:local.update(COMMAND_RING_ENV)
                validate=args.validation and not reference and not metadata
                if validate:local.update(vvl.validation_env(folder,manifest,empty))
                # VVL's base recipe deliberately supplies absent.conf. Restore
                # this gate's explicit pipeline mode AFTER applying that recipe.
                local['DXVK_CONFIG_FILE']=str(folder/'pipeline.conf')
                command=[exe,'--dll',dll,'--scenario','normal']+(['--reference'] if reference else [])
                case=dict(label=label,ok=False);receipt['cases'].append(case)
                case['process']=desktop.run(command,cwd=folder,stdout_path=folder/'case.stdout',stderr_path=folder/'case.stderr',timeout_s=60,env=local)
                receipt['raw'].append(capture_raw_artifacts(folder,['case.stdout','case.stderr']+([] if reference else ['host.log'])
                    +(['validation.raw.log'] if validate else []),True))
                check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                rows=json_lines(folder/'case.stdout');report=singleton(rows,'warvk-d3d9-factory-'+('only' if metadata else 'clear')+'/v1')
                require(report['ok'] and report['normalTeardown'] and report['memoryMeasured'] is False,'complete bounded fixture')
                require(report['factoryChecks']==(12 if reference else 14),'exact factory coverage')
                case['result']=report
                if not metadata and args.optimized_pipelines:
                    log=folder/('fixture_d3d9.log' if reference else 'host_warvk-d3d9-host.log')
                    require('dxvk.enableGraphicsPipelineLibrary = False' in log.read_text('utf-8'),
                        'actual owner did not report the requested optimized-pipeline configuration')
                    receipt['raw'].append(capture_raw_artifacts(folder,[log.name]))
                    case['optimizedConfigurationObserved']=True
                if not metadata:
                    require(report['clearCases']==9 and report['fixedCases']==11
                        and report['vertexInputChecks']==4 and report['fixedStateChecks']==6
                        and report['topologyCases']==8 and report['lineImages']==2
                        and report['textureCases']==187 and report['pixelChecks']==305*64*64*4,'whole-image clear/fixed/topology/vertex/texture/sampler/effects/multistage/stateblock coverage')
                    stateblock=re.search(r'^STATEBLOCK_EVIDENCE images=11 checks=(\d+)$',(folder/'case.stdout').read_text('utf-8'),re.M)
                    require(stateblock is not None and int(stateblock[1])>200,'native/remote state block contracts')
                    case['stateBlockChecks']=int(stateblock[1])
                if not reference:
                    pr=singleton(rows,'warvk-d3d9-proxy/v1');hs=singleton(json_lines(folder/'host.log'),'warvk-d3d9-host/v1')
                    case['host']=hs;case['proxy']=pr
                    require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                        and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit']==0,'healthy exact remote shutdown')
                    require(hs['ok'] and hs['bits']==64 and hs['normalTeardown'] and hs['liveResources']==0,'host ownership closed')
                    require(hs['gpuInitialized'] is (not metadata),'factory query created a device or rendering skipped initialization')
                    require(hs['submitted']==hs['gpuCompleted']==hs['retired'],'original GPU retirement')
                    require(pr['exchanges']==hs['accepted']+hs['rejected'],'ordered command balance')
                    require(hs['draws']==(0 if metadata else 302+TOPOLOGY_DRAWS+(ANNOTATION_DRAWS if annotate else 0)),'actual expected draw count')
                    if not metadata:
                        case['annotations']=annotation_rows(rows,prefixed(folder/'case.stderr','D3D9 annotation-proxy: '),
                            prefixed(folder/'host.log','D3D9 annotation-host: '),
                            prefixed(folder/'case.stderr','D3D9 content-proxy: '),prefixed(folder/'host.log','D3D9 content-host: '),
                            annotate,fixed_batch)
                        bs=singleton(prefixed(folder/'host.log','D3D9 draw-batch-host: '),'warvk-d3d9-draw-batch-host/v1')
                        ss=check_stream_metrics(prefixed(folder/'host.log','D3D9 stream-host: '),bs,stream)
                        if ss:
                            # Fixed-function draws must actually take the stream path.
                            require(ss['streamFixedDraws']>0 and ss['latencyWaits']==0,'fixed-function stream coverage')
                            case['streamHost']=ss
                        case['fixedBatch']=fixed_batch_rows(prefixed(folder/'case.stderr','D3D9 fixed-batch-proxy: '),
                            prefixed(folder/'host.log','D3D9 fixed-batch-host: '),fixed_batch,grouped or stream)
                        if fixed_batch and ss:
                            require(ss['streamFixedDraws']==case['fixedBatch']['host']['draws'],'every stream fixed draw was batched')
                    case['commandRing']=command_ring_rows(prefixed(folder/'case.stderr','D3D9 command-ring-proxy: '),
                        prefixed(folder/'case.stderr','D3D9 terminal: '),prefixed(folder/'host.log','D3D9 command-ring-host: '),
                        command_ring,pr,hs)
                    case['backing']=backing_rows(None if metadata else prefixed(folder/'case.stderr','D3D9 backing-proxy: '),
                        prefixed(folder/'host.log','D3D9 backing-host: '),budget)
                    case['uploadWindow']=window_rows(prefixed(folder/'case.stderr','D3D9 upload-window-proxy: '),window)
                    case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                    if not metadata:
                        ds=singleton(json_lines(folder/'host.log'),'warvk-d3d9-desktop/v1')
                        require(ds['pid']==pr['childPid'] and ds['peerPid']==case['process']['pid'] and ds['desktop']==desktop.desktop_name and ds['inputDesktop']==desktop.input_before,'exact host/desktop')
                    if validate:
                        case['validation']=vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                        case['loader']=check_loader((folder/'host.log').read_text('utf-8'),
                            (folder/'host_warvk-d3d9-host.log').read_text('utf-8'),library,empty,loader,loader_version)
                case['ok']=True
            hashes=[c['result']['pixelHash'] for c in receipt['cases'] if 'pixelHash' in c['result']]
            ring_arm_count=(2+int(args.stream)+int(args.fixed_batch)+int(args.annotations)) if args.command_ring else 0
            require(len(hashes)==4+int(args.stream)+2*int(args.fixed_batch)+4*int(args.backing)+2*int(args.annotations)+ring_arm_count
                and len(set(hashes))==1,'local/remote full clear image mismatch')
            receipt['fullTestImagesEqual']=True
            if args.backing:
                # Same calls at a zero budget: every borrowed window stood in for exactly one
                # restored x86 copy, and no Lock it served had to be read back.
                arms={c['label']:c for c in receipt['cases']}
                plain,windowed=arms['remote-backing-0']['backing']['proxy'],arms['remote-backing-0-window-0']['backing']['proxy']
                require(plain['allocations']-windowed['allocations']==arms['remote-backing-0-window-0']['uploadWindow']['acquires']
                    and windowed['fetches']<=plain['fetches'] and windowed['fetchedBytes']<=plain['fetchedBytes'],
                    'each write-only Lock window replaced one x86 copy')
                receipt['windowReplacedCopies']=plain['allocations']-windowed['allocations']
            if args.backing:
                receipt['backingCases']=[]
                for label,reference,budget,stream,window in BACKING_CASES:
                    folder=out/label;folder.mkdir();exe=folder/'fixture.exe';dll=folder/'renderer.dll'
                    copy(backing_fixture,exe);copy(reference_dll if reference else proxy,dll)
                    local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=config(folder))
                    if not reference:
                        copy(host,folder/'host.exe');local.update(WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                    if stream:local.update(STREAM_ENV)
                    if budget is not None:local[BACKING_ENV]=str(budget)
                    if window is not None:local[WINDOW_ENV]=str(window)
                    validate=args.validation and not reference
                    if validate:local.update(vvl.validation_env(folder,manifest,empty))
                    local['DXVK_CONFIG_FILE']=str(folder/'pipeline.conf')
                    case=dict(label=label,ok=False);receipt['backingCases'].append(case)
                    case['process']=desktop.run([exe,'--dll',dll,'--scenario','normal']+(['--reference'] if reference else []),
                        cwd=folder,stdout_path=folder/'case.stdout',stderr_path=folder/'case.stderr',timeout_s=120,env=local)
                    receipt['raw'].append(capture_raw_artifacts(folder,['case.stdout','case.stderr']+([] if reference else ['host.log'])
                        +(['validation.raw.log'] if validate else []),True))
                    check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                    rows=json_lines(folder/'case.stdout');report=singleton(rows,'warvk-d3d9-backing-fixture/v1')
                    require(report['ok'] and report['normalTeardown'] and report['memoryMeasured'] is True
                        and report['reference'] is reference and report['textures']==12 and report['textureLevels']==11
                        and report['images']==18 and report['pixelChecks']==18*64*64 and report['texelChecks']==1024*1024
                        and report['x86VulkanAbsent'] is (not reference) and report['gameTouched'] is False,'backing workload coverage')
                    require(tuple(x['name'] for x in report['samples'])==BACKING_SAMPLES
                        and all(x['memory']['valid'] for x in report['samples']),'complete x86 memory samples')
                    case['result']=report
                    if not reference:
                        pr=singleton(rows,'warvk-d3d9-proxy/v1');hs=singleton(json_lines(folder/'host.log'),'warvk-d3d9-host/v1')
                        case['host']=hs;case['proxy']=pr
                        require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                            and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit']==0,'backing remote shutdown')
                        require(hs['ok'] and hs['normalTeardown'] and hs['liveResources']==0 and hs['draws']==18
                            and hs['submitted']==hs['gpuCompleted']==hs['retired'] and pr['exchanges']==hs['accepted']+hs['rejected'],
                            'backing host ownership closed')
                        bs=singleton(prefixed(folder/'host.log','D3D9 draw-batch-host: '),'warvk-d3d9-draw-batch-host/v1')
                        if check_stream_metrics(prefixed(folder/'host.log','D3D9 stream-host: '),bs,stream):case['streamChecked']=True
                        case['backing']=backing_rows(prefixed(folder/'case.stderr','D3D9 backing-proxy: '),
                            prefixed(folder/'host.log','D3D9 backing-host: '),budget)
                        case['uploadWindow']=window_rows(prefixed(folder/'case.stderr','D3D9 upload-window-proxy: '),window)
                        if window is not None:
                            require((case['uploadWindow']['bufferLocks'],case['uploadWindow']['textureLocks'])==BACKING_WINDOW_LOCKS,
                                'every first Lock borrowed a window')
                        if budget is None:
                            require(case['backing']['proxy']['peakResidentBytes']>=BACKING_TEXTURE_BYTES,'legacy proxy keeps every texture byte')
                        case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                    if validate:
                        case['validation']=vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                        case['loader']=check_loader((folder/'host.log').read_text('utf-8'),
                            (folder/'host_warvk-d3d9-host.log').read_text('utf-8'),library,empty,loader,loader_version)
                    memory={x['name']:x['memory'] for x in report['samples']}
                    case['x86Delta']={name:{k:memory[name][k]-memory['device'][k] for k in ('privateBytes','usedVirtualBytes','privateVirtualBytes')}
                        for name in BACKING_SAMPLES}
                    case['ok']=True
                require(len({c['result']['pixelHash'] for c in receipt['backingCases']})==1,'backing workload images differ')
                receipt['backingImagesEqual']=True
                arms={c['label']:c for c in receipt['backingCases']}
                plain,windowed=arms['backing-remote-0']['backing']['proxy'],arms['backing-remote-0-window']['backing']['proxy']
                require(plain['allocations']-windowed['allocations']==sum(BACKING_WINDOW_LOCKS)
                    and windowed['fetches']==plain['fetches'] and windowed['fetchedBytes']==plain['fetchedBytes'],
                    'the memory workload restores the same content with windows')
            if args.extent:
                receipt['extentCases']=[]
                for label,reference,extent,present,zero in EXTENT_CASES:
                    arguments=['--extent',extent]+(['--zero-extent'] if zero else [])
                    case,folder,rows,report,validate=extent_fixture_case(receipt['extentCases'],label,reference,present,arguments)
                    width,height=(int(v) for v in extent.split('x'))
                    frames=EXTENT_FRAMES if present else 0
                    require(report['ok'] and report['normalTeardown'] and report['gameTouched'] is False
                        and report['width']==width and report['height']==height and report['zeroExtent'] is zero
                        and report['reference'] is reference and report['present'] is present
                        and report['images']==EXTENT_IMAGES+frames and report['draws']==EXTENT_DRAWS+frames
                        and report['stateChecks']==5 and report['pixelChecks']==(EXTENT_IMAGES+frames)*width*height*4
                        and report['presentedFrames']==frames and report['presentedChecks']==frames*width*height*4
                        and report['x86VulkanAbsent'] is (not reference),'extent workload coverage')
                    case['result']=report
                    if not reference:
                        hs=extent_remote_closed(case,folder,rows,EXTENT_DRAWS+frames,0)
                        # A banded target pays one readback per band; inline images one per image.
                        require(hs['readbacks']==(EXTENT_IMAGES+frames)*readback_bands(width,height),'extent readback bands')
                        require(hs['presentImplemented'] is present and hs['presentApiSucceeded']==frames
                            and hs['presentedFrame']==frames and hs['presentedReadbacks']==frames
                            and hs['presentGpuCompleted']==frames and hs['acquiredImages']==frames,'extent presented frames')
                    if validate:extent_validation(case,folder)
                    case['ok']=True
                for extent in {c[2] for c in EXTENT_CASES}:
                    hashes={c['result']['pixelHash'] for c in receipt['extentCases']
                        if c['label'].endswith(extent) and not c['result']['present']}
                    require(len(hashes)<=1,'extent '+extent+' local/remote images differ')
                receipt['extentImagesEqual']=True
            if args.reset:
                receipt['resetCases']=[]
                for label,reference,extent,reset,present,zero,swapchain in RESET_CASES:
                    arguments=['--extent',extent,'--reset',reset]+(['--reset-zero'] if zero else [])+list(swapchain)
                    case,folder,rows,report,validate=extent_fixture_case(receipt['resetCases'],label,reference,present,arguments)
                    (width,height),(next_width,next_height)=((int(v) for v in e.split('x')) for e in (extent,reset))
                    first,second=width*height*4,next_width*next_height*4
                    frames=EXTENT_FRAMES if present else 0
                    game=bool(swapchain)
                    # Creation extent: its images, the resized-window image and its frames; then the Reset extent.
                    images_first,images_second=RESET_PHASE_IMAGES+1+frames,RESET_PHASE_IMAGES+frames
                    require(report['ok'] and report['normalTeardown'] and report['gameTouched'] is False
                        and report['width']==width and report['height']==height and report['zeroExtent'] is False
                        and report['resetWidth']==next_width and report['resetHeight']==next_height and report['resetZero'] is zero
                        and report['reference'] is reference and report['present'] is present
                        and report['format']==('x8r8g8b8' if game else 'a8r8g8b8') and report['depth']==('d16' if game else 'd32f')
                        and report['interval']==('immediate' if game else 'one')
                        and report['images']==images_first+images_second and report['draws']==2*RESET_PHASE_DRAWS+2*frames
                        and report['stateChecks']==5 and report['resetChecks']==RESET_CHECKS+(1 if present else 0)
                        and report['pixelChecks']==images_first*first+images_second*second
                        and report['presentedFrames']==2*frames and report['presentedChecks']==frames*(first+second)
                        and report['x86VulkanAbsent'] is (not reference),'reset workload coverage')
                    case['result']=report
                    if not reference:
                        # The only host refusal is the Present after the resize, which skips its frame.
                        hs=extent_remote_closed(case,folder,rows,2*RESET_PHASE_DRAWS+2*frames,1 if present else 0)
                        require(hs['readbacks']==images_first*readback_bands(width,height)+images_second*readback_bands(next_width,next_height),
                            'reset readback bands')
                        require(hs['presentImplemented'] is present and hs['presentApiSucceeded']==2*frames
                            and hs['presentedFrame']==2*frames and hs['presentedReadbacks']==2*frames
                            and hs['presentGpuCompleted']==2*frames and hs['acquiredImages']==2*frames
                            and hs['presentAttempts']==(2*frames+1 if present else 0),'reset presented frames')
                        rh=singleton(prefixed(folder/'host.log','D3D9 reset-host: '),'warvk-d3d9-reset-host/v1')
                        case['resetHost']=rh
                        interval=(0x80000000 if game else 1) if present else 0
                        require(rh['resets']==1 and rh['windowResets']==0 and rh['presentsSkipped']==(1 if present else 0)
                            and rh['width']==next_width and rh['height']==next_height
                            and rh['opaqueTarget'] is game and rh['syncInterval']==interval,'reset host account')
                        # The swallowed WM_WINDOWPOSCHANGED sends no WM_SIZE: nothing for the window hook.
                        wp=singleton(prefixed(folder/'case.stderr','D3D9 window-proxy: '),'warvk-d3d9-window-proxy/v1')
                        case['windowProxy']=wp
                        require(wp['hooked'] is True and wp['sizeMessages']==wp['resizes']==wp['refused']==0,'reset without a window resize')
                    if validate:extent_validation(case,folder)
                    case['ok']=True
                for group in ('game','a8'):
                    hashes={c['result']['pixelHash'] for c in receipt['resetCases']
                        if c['label'].endswith(group) and not c['result']['present']}
                    require(len(hashes)==1,'reset '+group+' local/remote images differ')
                receipt['resetImagesEqual']=True
            if args.resize:
                receipt['resizeCases']=[]
                for label,reference,extent,resize,present,swapchain,extra in RESIZE_CASES:
                    arguments=['--extent',extent,'--resize',resize]+list(swapchain)
                    case,folder,rows,report,validate=extent_fixture_case(receipt['resizeCases'],label,reference,present,arguments,extra)
                    (width,height),(next_width,next_height)=((int(v) for v in e.split('x')) for e in (extent,resize))
                    first,second=width*height*4,next_width*next_height*4
                    frames=EXTENT_FRAMES if present else 0
                    game=bool(swapchain)
                    images_first,images_second=RESET_PHASE_IMAGES+frames,RESIZE_IMAGES+RESET_PHASE_IMAGES+frames
                    draws=2*RESET_PHASE_DRAWS+RESIZE_DRAWS+2*frames
                    require(report['ok'] and report['normalTeardown'] and report['gameTouched'] is False
                        and report['width']==width and report['height']==height and report['zeroExtent'] is False
                        and report['resetWidth']==report['resetHeight']==report['resetChecks']==0
                        and report['resizeWidth']==next_width and report['resizeHeight']==next_height
                        and report['reference'] is reference and report['present'] is present
                        and report['format']==('x8r8g8b8' if game else 'a8r8g8b8') and report['depth']==('d16' if game else 'd32f')
                        and report['interval']==('immediate' if game else 'one')
                        and report['images']==images_first+images_second and report['draws']==draws
                        and report['stateChecks']==5 and report['resizeChecks']==RESIZE_CHECKS
                        and report['pixelChecks']==images_first*first+images_second*second
                        and report['presentedFrames']==2*frames and report['presentedChecks']==frames*(first+second)
                        and report['x86VulkanAbsent'] is (not reference),'resize workload coverage')
                    case['result']=report
                    if not reference:
                        hs=extent_remote_closed(case,folder,rows,draws,0)
                        require(hs['readbacks']==images_first*readback_bands(width,height)+images_second*readback_bands(next_width,next_height),
                            'resize readback bands')
                        require(hs['presentImplemented'] is present and hs['presentApiSucceeded']==2*frames
                            and hs['presentedFrame']==2*frames and hs['presentedReadbacks']==2*frames
                            and hs['presentGpuCompleted']==2*frames and hs['acquiredImages']==2*frames
                            and hs['presentAttempts']==(2*frames if present else 0),'resize presented frames')
                        rh=singleton(prefixed(folder/'host.log','D3D9 reset-host: '),'warvk-d3d9-reset-host/v1')
                        case['resetHost']=rh
                        interval=(0x80000000 if game else 1) if present else 0
                        # One swap chain reset that kept every resource; no Present was skipped.
                        require(rh['resets']==rh['windowResets']==1 and rh['presentsSkipped']==0
                            and rh['width']==next_width and rh['height']==next_height
                            and rh['opaqueTarget'] is game and rh['syncInterval']==interval,'resize host account')
                        wp=singleton(prefixed(folder/'case.stderr','D3D9 window-proxy: '),'warvk-d3d9-window-proxy/v1')
                        case['windowProxy']=wp
                        require(wp['hooked'] is True and wp['sizeMessages']>=1 and wp['resizes']==1 and wp['refused']==0,
                            'the proxy followed the window')
                        oa=singleton(prefixed(folder/'case.stderr','D3D9 ordered-async: '),'warvk-d3d9-ordered-async/v1')
                        case['orderedAsync']=oa
                        require(oa['enabled'] is bool(extra),'resize admission profile')
                    if validate:extent_validation(case,folder)
                    case['ok']=True
                for group in ('game','a8'):
                    hashes={c['result']['pixelHash'] for c in receipt['resizeCases']
                        if c['label'].endswith(group) and not c['result']['present']}
                    require(len(hashes)==1,'resize '+group+' local/remote images differ')
                receipt['resizeImagesEqual']=True
            if args.ring:
                receipt['ringCases']=[];e=ring_expectation();receipt['ringExpectation']=e
                pixels=RING_EXTENT*RING_EXTENT*4
                for label,reference,extra in RING_CASES:
                    arguments=['--extent',f'{RING_EXTENT}x{RING_EXTENT}','--ring','--depth-bias',*GAME_SWAPCHAIN]
                    case,folder,rows,report,validate=extent_fixture_case(receipt['ringCases'],label,reference,False,arguments,extra)
                    require(report['ok'] and report['normalTeardown'] and report['gameTouched'] is False
                        and report['width']==report['height']==RING_EXTENT and report['ring'] is True
                        and report['reference'] is reference and report['present'] is False
                        and report['format']=='x8r8g8b8' and report['depth']=='d16' and report['interval']=='immediate'
                        and report['depthBias'] is True
                        and report['images']==RING_FRAMES+RING_BIAS_IMAGES and report['draws']==RING_FRAMES*RING_DRAWS+RING_BIAS_DRAWS
                        and report['stateChecks']==9 and report['pixelChecks']==(RING_FRAMES+RING_BIAS_IMAGES)*pixels
                        and report['ringLocks']==e['locks'] and report['ringDiscards']==e['discards']
                        and report['ringWraps']==e['wraps'] and report['ringBytes']==e['bytes']
                        and report['ringCarriedBytes']==e['carried']
                        and report['x86VulkanAbsent'] is (not reference),'ring workload coverage')
                    require(e['wraps']>=RING_FRAMES and e['discards']==e['wraps']+len(RING_SIZES),'ring wraps every frame')
                    case['result']=report
                    if not reference:
                        hs=extent_remote_closed(case,folder,rows,RING_FRAMES*RING_DRAWS+RING_BIAS_DRAWS,0)
                        require(hs['readbacks']==(RING_FRAMES+RING_BIAS_IMAGES)*readback_bands(RING_EXTENT,RING_EXTENT),'ring readbacks')
                        require(hs['declarationsCreated']==RING_DECLARATIONS,'one host declaration per FVF')
                        fv=singleton(prefixed(folder/'case.stderr','D3D9 fvf-proxy: '),'warvk-d3d9-fvf-proxy/v1')
                        case['fvfProxy']=fv
                        require(fv['cached']==RING_DECLARATIONS and fv['evictions']==0,'proxy FVF declaration cache')
                        oa=singleton(prefixed(folder/'case.stderr','D3D9 ordered-async: '),'warvk-d3d9-ordered-async/v1')
                        case['orderedAsync']=oa
                        stream,fixed=bool(extra),'WARVK_D3D9_FIXED_BATCH' in extra
                        batches,passed=ring_fixed_batches()
                        require(oa['enabled'] is stream,'ordered admission profile')
                        if stream:
                            # Every Unlock chunk and every state sync went out without waiting for its reply.
                            # The host's uploads also count the one texture upload.
                            require(oa['uploadsPublished']==e['chunks']+RING_STATIC_UPLOADS==hs['uploads']-1
                                and oa['statesPublished']==RING_STATE_SYNCS
                                and oa['remoteSucceeded']==oa['uploadsPublished']+oa['statesPublished']
                                and oa['passedStaged']==(passed if fixed else 0),'ordered state admitted without replies')
                        else:
                            require(oa['uploadsPublished']==oa['statesPublished']==oa['passedStaged']==oa['remoteSucceeded']==0,
                                'synchronous profile waits for every reply')
                        case['fixedBatch']=fixed_batch_rows(prefixed(folder/'case.stderr','D3D9 fixed-batch-proxy: '),
                            prefixed(folder/'host.log','D3D9 fixed-batch-host: '),fixed,stream)
                        if fixed:
                            fh=case['fixedBatch']['host']
                            require(fh['draws']==sum(batches) and fh['batches']==len(batches)
                                and fh['largestBatch']==max(batches),'fixed batches across NOOVERWRITE uploads')
                        dh=singleton(prefixed(folder/'host.log','D3D9 dynamic-buffer-host: '),'warvk-d3d9-dynamic-buffer-host/v1')
                        case['dynamicHost']=dh
                        require(dh['dynamicBuffers']==len(RING_SIZES) and dh['orderedUploads']==0
                            and dh['discardUploads']==e['discards'] and dh['noOverwriteUploads']==e['chunks']-e['discards'],
                            'ring host upload modes')
                        # Stream renames on DISCARD and carries the initialized prefix;
                        # the other owners wait for the GPU before writing in place.
                        if stream:
                            require(dh['discardRenames']==e['discards'] and dh['discardCarriedBytes']==e['carried']
                                and dh['uploadSettles']==RING_STATIC_UPLOADS,'ring stream renames')
                        else:
                            require(dh['discardRenames']==0 and dh['discardCarriedBytes']==0
                                and dh['uploadSettles']==RING_STATIC_UPLOADS+e['discards'],'ring sync settles')
                        budget=int(extra[BACKING_ENV]) if BACKING_ENV in extra else None
                        window=int(extra[WINDOW_ENV]) if WINDOW_ENV in extra else None
                        case['uploadWindow']=window_rows(prefixed(folder/'case.stderr','D3D9 upload-window-proxy: '),window)
                        if budget is not None:
                            # The product profile on the game's pattern: every vertex Lock (ring appends,
                            # DISCARD wraps, the static first Locks) and the texture borrow a window; only
                            # the index ring is resident, for local index validation; nothing is read back.
                            case['backing']=backing_rows(prefixed(folder/'case.stderr','D3D9 backing-proxy: '),
                                prefixed(folder/'host.log','D3D9 backing-host: '),budget)
                            pb,pw=case['backing']['proxy'],case['uploadWindow']
                            require(pw['bufferLocks']==e['locks']-RING_FRAMES*RING_DRAWS+RING_STATIC_UPLOADS and pw['textureLocks']==1,
                                'every vertex and texture Lock borrowed a window')
                            require(pb['allocations']==1 and pb['peakResidentBytes']==RING_SIZES['index']
                                and pb['fetches']==pb['fetchCalls']==pb['evictions']==pb['discardMisses']==0,
                                'only the index ring is resident and nothing is read back')
                    if validate:extent_validation(case,folder)
                    case['ok']=True
                require(len({c['result']['pixelHash'] for c in receipt['ringCases']})==1,'ring local/remote images differ')
                receipt['ringImagesEqual']=True
            if args.fixed_batch:
                fixed_draws={c['fixedBatch']['host']['draws'] for c in receipt['cases'] if 'fixed' in c['label'] and 'annotate' not in c['label']}
                require(len(fixed_draws)==1,'grouped and stream fixed batch arms batched the same fixed draws')
                if args.annotations:
                    for annotated in (c for c in receipt['cases'] if c['label'].startswith('remote-stream-fixed-annotate')):
                        require(annotated['fixedBatch']['host']['draws']==min(fixed_draws)+ANNOTATION_DRAWS,'annotated draws batched like the rest')
            faults=[('fixed-rejection',{},'faultCase')]
            if args.stream:faults.append(('fixed-rejection-stream',STREAM_ENV,'streamFaultCase'))
            # Synchronous host stream: atomic fixed batch refusal under the stream owner.
            if args.fixed_batch:faults.append(('fixed-rejection-stream-sync',dict(WARVK_D3D9_STREAM='1'),'streamSyncFaultCase'))
            # Host content authority negotiated by the raw peer itself.
            if args.backing:faults.append(('fixed-rejection-retain',dict(WARVK_TEST_RAW_RETAIN='1'),'retainFaultCase'))
            # Annotation binding and refusal, negotiated by the raw peer itself.
            if args.annotations:faults.append(('fixed-rejection-annotate',dict(WARVK_TEST_RAW_ANNOTATE='1'),'annotateFaultCase'))
            if args.command_ring:
                # The same raw traffic over the ring: every synchronous refusal stays recoverable.
                faults.append(('fixed-rejection-cmdring',COMMAND_RING_ENV,'ringFaultCase'))
                if args.stream:faults.append(('fixed-rejection-stream-cmdring',STREAM_ENV|COMMAND_RING_ENV,'ringStreamFaultCase'))
                if args.annotations:
                    faults.append(('fixed-rejection-annotate-cmdring',dict(WARVK_TEST_RAW_ANNOTATE='1')|COMMAND_RING_ENV,'ringAnnotateFaultCase'))
            for fault_label,fault_env,fault_key in faults:
                folder=out/fault_label;folder.mkdir();exe=folder/'fixture.exe'
                copy(fault_fixture,exe);copy(host,folder/'host.exe')
                local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=config(folder),
                    WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                local.update(fault_env)
                if args.validation:local.update(vvl.validation_env(folder,manifest,empty))
                local['DXVK_CONFIG_FILE']=str(folder/'pipeline.conf')
                fault=dict(ok=False);receipt[fault_key]=fault
                fault['process']=desktop.run([exe,'--mode','raw','--dll',proxy,
                    '--expect-desktop',desktop.desktop_name,'--input-desktop',desktop.input_before],cwd=folder,
                    stdout_path=folder/'case.stdout',stderr_path=folder/'case.stderr',timeout_s=60,env=local)
                receipt['raw'].append(capture_raw_artifacts(folder,['case.stdout','case.stderr','host.log']
                    +(['validation.raw.log'] if args.validation else []),True))
                check_launch(fault['process'],desktop.desktop_name,desktop.input_before)
                rows=json_lines(folder/'case.stdout');report=singleton(rows,'warvk-fixed-rejection/v1')
                require(report['ok'] and report['normalBeforeRejection']==36 and report['rejected']==24 and report['recoveries']==24
                    and report['textureRejected']==17 and report['textureRecoveries']==5
                    and report['samplerRejected']==11 and report['samplerRecoveries']==11
                    and report['effectsRejected']==12 and report['effectsRecoveries']==12
                    and report['stagesRejected']==12 and report['stagesDraws']==13
                    and report['unchangedPixels']==24*64*64 and report['pixelFailures']==0
                    and report['memoryMeasured'] is False and report['gameTouched'] is False,'fixed rejection/recovery coverage')
                fault_async='WARVK_D3D9_ASYNC_BATCH' in fault_env
                batch_refusals=0 if fault_async else 12
                require(report['fixedBatchAsync'] is fault_async and report['fixedBatchRejected']==report['fixedBatchRecoveries']==batch_refusals
                    and report['fixedBatchDraws']==FIXED_BATCH_CAPACITY+3*batch_refusals
                    and report['fixedBatchUnchangedPixels']==batch_refusals*64*64,'atomic fixed batch refusal and recovery')
                retain='WARVK_TEST_RAW_RETAIN' in fault_env
                require(report['contentRetain'] is retain and report['contentRejected']==(12 if retain else 1)
                    and report['contentReads']==(4 if retain else 0) and (report['contentReadBytes']>4096) is retain,
                    'content read refusal and retained copies')
                hb=singleton(prefixed(folder/'host.log','D3D9 backing-host: '),'warvk-d3d9-backing-host/v1')
                require(hb['retainContent'] is retain and hb['retainedBytes']==0 and (hb['peakRetainedBytes']>0) is retain
                    and hb['contentReads']==report['contentReads'] and hb['contentReadBytes']==report['contentReadBytes'],
                    'raw retained content reads')
                fault['backingHost']=hb
                fault['annotations']=raw_annotation_rows(report,prefixed(folder/'host.log','D3D9 annotation-host: '),
                    prefixed(folder/'host.log','D3D9 content-host: '),'WARVK_TEST_RAW_ANNOTATE' in fault_env,fault_async)
                pr=singleton(rows,'warvk-d3d9-proxy/v1');hs=singleton(json_lines(folder/'host.log'),'warvk-d3d9-host/v1')
                fault.update(result=report,proxy=pr,host=hs)
                if args.optimized_pipelines:
                    log=folder/'host_warvk-d3d9-host.log'
                    require('dxvk.enableGraphicsPipelineLibrary = False' in log.read_text('utf-8'),'fault owner optimized configuration missing')
                    receipt['raw'].append(capture_raw_artifacts(folder,[log.name]))
                    fault['optimizedConfigurationObserved']=True
                require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                    and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit']==0,'fault child shutdown')
                require(hs['ok'] and hs['bits']==64 and hs['normalTeardown'] and hs['liveResources']==0
                    and hs['draws']==101+report['fixedBatchDraws']+report['annotationDraws']+report['payloadDraws']
                    and hs['rejected']==76+report['fixedBatchRejected']+report['contentRejected']+report['annotationRejected']
                        +report['payloadRejected']
                    and hs['submitted']==hs['gpuCompleted']==hs['retired'],
                    'rejected draw mutated submission or failed retirement')
                require(pr['exchanges']==hs['accepted']+hs['rejected'],'fault command balance')
                fault['commandRing']=command_ring_rows(prefixed(folder/'case.stderr','D3D9 command-ring-proxy: '),
                    prefixed(folder/'case.stderr','D3D9 terminal: '),prefixed(folder/'host.log','D3D9 command-ring-host: '),
                    'WARVK_D3D9_COMMAND_RING' in fault_env,pr,hs)
                fh=singleton(prefixed(folder/'host.log','D3D9 fixed-batch-host: '),'warvk-d3d9-fixed-batch-host/v1')
                # The annotated raw arm adds one accepted DrawFixedBatch(3) carrying records for draws 0 and 2.
                annotated_batch='WARVK_TEST_RAW_ANNOTATE' in fault_env
                require(fh['batches']==1+report['fixedBatchRecoveries']+int(annotated_batch)
                    and fh['draws']==report['fixedBatchDraws']+3*int(annotated_batch)
                    and fh['largestBatch']==FIXED_BATCH_CAPACITY,'raw fixed batches accepted by the host')
                fault['fixedBatchHost']=fh
                fault['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                ds=singleton(json_lines(folder/'host.log'),'warvk-d3d9-desktop/v1')
                require(ds['pid']==pr['childPid'] and ds['peerPid']==fault['process']['pid']
                    and ds['desktop']==desktop.desktop_name and ds['inputDesktop']==desktop.input_before,'fault exact host/desktop')
                if args.validation:
                    fault['validation']=vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                    fault['loader']=check_loader((folder/'host.log').read_text('utf-8'),
                        (folder/'host_warvk-d3d9-host.log').read_text('utf-8'),library,empty,loader,loader_version)
                fault['ok']=True
            if args.command_ring:
                # One staged terminal cause per session, against the real host.
                receipt['ringFaults']=[]
                for name,kind,detail,result,host_names_fault,host_records,host_exit in RING_FAULTS:
                    folder=out/('ring-fault-'+name);folder.mkdir();exe=folder/'fixture.exe'
                    copy(ring_fault_fixture,exe);copy(host,folder/'host.exe')
                    local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=config(folder),
                        WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))|COMMAND_RING_ENV
                    local['DXVK_CONFIG_FILE']=str(folder/'pipeline.conf')
                    ring_pins.clear()
                    if name.startswith('resource-'):
                        # These sessions own the two resource switches: the unnegotiated cases need one absent.
                        ring_pins.update(('WARVK_D3D9_ASYNC_DESTROY','WARVK_D3D9_ASYNC_CREATE_TEXTURE'))
                        local.update(WARVK_D3D9_BATCH_DRAWS='1',WARVK_D3D9_ASYNC_BATCH='1')
                        for var,enabled in [('WARVK_D3D9_ASYNC_DESTROY',name!='resource-unnegotiated-destroy'),
                                ('WARVK_D3D9_ASYNC_CREATE_TEXTURE',name!='resource-unnegotiated-create')]:
                            if enabled:local[var]='1'
                            else:local.pop(var,None)
                    if name=='scene-stopped-reset':local['WARVK_D3D9_ANNOTATIONS']='1'
                    case=dict(name=name,ok=False);receipt['ringFaults'].append(case)
                    case['process']=desktop.run([exe,'--case',name,'--expect-desktop',desktop.desktop_name,
                        '--input-desktop',desktop.input_before],cwd=folder,stdout_path=folder/'case.stdout',
                        stderr_path=folder/'case.stderr',timeout_s=90,env=local)
                    ring_pins.clear()
                    receipt['raw'].append(capture_raw_artifacts(folder,['case.stdout','case.stderr','host.log'],True))
                    check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                    rows=json_lines(folder/'case.stdout');report=singleton(rows,'warvk-d3d9-ring-fault/v1')
                    pr=singleton(rows,'warvk-d3d9-proxy/v1');stderr=(folder/'case.stderr').read_text('utf-8')
                    rp=singleton(prefixed(folder/'case.stderr','D3D9 command-ring-proxy: '),'warvk-d3d9-command-ring-proxy/v1')
                    rt=singleton(prefixed(folder/'case.stderr','D3D9 terminal: '),'warvk-d3d9-terminal/v1')
                    case.update(result=report,proxy=pr,ring=rp,terminal=rt)
                    require(report['ok'] and report['case']==name and report['bits']==32 and pr['peerVerified'] and pr['x86VulkanAbsent']
                        and rp['attached'] is True and rp['capacity']==COMMAND_RING_BYTES,'ring fault session attached')
                    require(report['terminal']==rt['kind']==kind and report['detail']==rt['detail']==detail
                        and report['result']==rt['result']==result,'terminal cause reported by x86')
                    host_rows=json_lines(folder/'host.log')
                    faults=[r for r in host_rows if r.get('schema')=='warvk-d3d9-command-ring-fault/v1']
                    if name.startswith('resource-'):
                        account=singleton(prefixed(folder/'host.log','D3D9 async-resource-host: '),'warvk-d3d9-async-resource-host/v1')
                        destroy,create=(2,1) if name=='resource-reuse' else ((1,0) if name in
                            ('resource-wrong-type','resource-stale','resource-unnegotiated-destroy','resource-after-apply',
                             'resource-device-lost') else (0,1))
                        require(account['destroyReceived']==destroy and account['createTextureReceived']==create and
                            account['unnegotiatedRejected']==int('unnegotiated' in name),'host resource admission')
                        case['asyncResources']=account
                        if name.startswith('resource-unnegotiated'):
                            initial=int(name.endswith('destroy'))
                            require(account['logicalCreated']==account['texturesCreated']==account['liveResources']==initial
                                and account['logicalDestroyed']==0,'unnegotiated record had no resource side effects')
                        if name=='resource-after-apply':
                            require(account['logicalCreated']==account['logicalDestroyed']==1 and account['liveResources']==0,
                                'resource destroyed before its result was lost')
                            require('fixture stopped after resource apply' in (folder/'host.log').read_text('utf-8'),
                                'host completed resource apply before exit')
                    if host_names_fault:
                        # The host refused, said why, and executed nothing after the record.
                        require(len(faults)==1 and faults[0]['fault']==detail and faults[0]['records']==host_records
                            and faults[0]['expectedSequence']==rp['attachSequence']+host_records+int(detail!='async-refused'),
                            'host named the refused record')
                        case['hostFault']=faults[0]
                    else:require(not faults,'host fault row without a refused record')
                    if name in ('healthy','scene-refused','scene-stopped-reset','resource-reuse'):
                        hs=singleton(host_rows,'warvk-d3d9-host/v1')
                        rh=singleton(prefixed(folder/'host.log','D3D9 command-ring-host: '),'warvk-d3d9-command-ring-host/v1')
                        require(pr['childNaturalExit'] and pr['childProcessSettled'] and pr['childExit']==0 and not pr['transportFailed']
                            and hs['ok'] and hs['normalTeardown'] and rp['finishExecuted'] is True
                            and rp['records']==rh['records']==host_records and pr['exchanges']==hs['accepted']+hs['rejected'],
                            'healthy ring session')
                        if name=='healthy':
                            # Address space the ring costs x86: exactly its two views.
                            require(report['newViews']==2 and (report['smallestView'],report['largestView'])==RING_VIEW_BYTES
                                and report['newViewBytes']==sum(RING_VIEW_BYTES),'ring views in the x86 address space')
                        elif name!='resource-reuse':
                            # scene-refused: both markers refused (neither was negotiated), none ignored.
                            # scene-stopped-reset: the nested enter refused, the exit ignored, and the
                            # session reset after them accepted. x86 is told either way.
                            refused,ignored=(2,0) if name=='scene-refused' else (1,1)
                            refusals=re.findall(r'^D3D9 host scene-refusals: refused=(\d+) ignored=(\d+)\s*$',
                                (folder/'host.log').read_text('utf-8'),re.M)
                            require(report['sceneRefused'] is True and refusals==[(str(refused),str(ignored))]
                                and hs['rejected']==refused,'refused scene records reported without ending the session')
                            case['sceneRefusals']=dict(refused=refused,ignored=ignored)
                        case['host']=hs
                    elif name=='resource-device-lost':
                        require(pr['deviceLost'] and not pr['transportFailed'] and pr['childProcessSettled']
                            and pr['childExit']==host_exit and not rp['finishExecuted'],'acknowledged device loss')
                    else:
                        # The session failed: the worker drains the host, which must be gone.
                        require(pr['transportFailed'] and not pr['deviceLost'] and rp['finishExecuted'] is False
                            and report['elapsedMs']<10000,'failed ring session closed in time')
                        drained=re.findall(r'^D3D9 dev secondary-exit-drain pid=(\d+) exit=(\d+) ',stderr,re.M)
                        require(len(drained)==1 and int(drained[0][0])==pr['childPid'],'host drained after the failure')
                        case['hostExit']=int(drained[0][1])
                        require(case['hostExit']!=0 and (host_exit is None or case['hostExit']==host_exit),'host exit after the failure')
                    if name=='host-suspended':require(report['elapsedMs']>=1500,'hang bounded by the no-progress limit')
                    case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                    case['ok']=True
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            run_cases(desktop)
            if args.validation:
                receipt['implicitSourceAfter']=empty_directory_witness(empty)
                require(receipt['implicitSourceAfter']==receipt['implicitSourceBefore'],'private layer source drift')
        require(receipt['desktopSafety']['ok'],'isolated cleanup');receipt['ok']=True
    except Exception as error:receipt['error']=str(error)
    finally:
        try:
            receipt['inputsPreserved']=bool(frozen) and all(guarded_identity(Path(r['path']))==r for r in frozen)
            require(receipt['inputsPreserved'],'input drift')
            require(all(identity(Path(r['path']))==r for r in receipt['copies']),'case copy drift')
            for raw in receipt['raw']:verify_raw_artifacts(raw)
            receipt['rawPreserved']=True
        except Exception as error:receipt['ok']=False;receipt['preservationError']=str(error)
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    summary={k:receipt.get(k) for k in ('ok','error','preservationError','inputsPreserved')}
    if args.window_life:
        arms=receipt.get('windowLifeCases',[])
        summary.update(windowLifePassed=sum(c['ok'] for c in arms),windowLifeArms=len(arms))
    else:summary['fullTestImagesEqual']=receipt.get('fullTestImagesEqual')
    print(json.dumps(summary))
    return 0 if receipt['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
