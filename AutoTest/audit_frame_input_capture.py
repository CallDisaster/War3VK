"""Revalidate immutable player inputs against the frozen delivered readers.
Writes a new audit directory only. Does not deploy, record or repair evidence.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import sys

def require(ok,message):
    if not ok:raise ValueError(message)
def identity(path):
    with path.open('rb') as f:h=hashlib.file_digest(f,'sha256').hexdigest().upper()
    return dict(size=path.stat().st_size,sha256=h)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('history',type=Path);p.add_argument('--package',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();root=a.history.resolve();package=a.package.resolve()
    require(not a.output.exists(),'CreateNew audit output required')
    info=json.loads((package/'package-info.json').read_text(encoding='utf-8'))
    for rel,pin in info['files'].items():require(identity(package/rel)==pin,'frozen package changed: '+rel)
    sys.path.insert(0,str(package/'AutoTest'))
    from analyze_frame_evidence import load,analyze
    from analyze_frame_history import analyze_history
    from analyze_frame_inputs import analyze_inputs
    from export_frame_history_video import validate
    names=('manifest.json','cpu-events.json','inputs.json','inputs.bin','capture-identities.json',
           'history-analysis.json','cpu-analysis.json','inputs-analysis.json')
    pins={name:identity(root/name) for name in names}
    print('Frozen reader/source pins verified; checking original capture.',flush=True)
    manifest=load(root/'manifest.json');cpu=load(root/'cpu-events.json')
    inputs=load(root/'inputs.json');binary=(root/'inputs.bin').read_bytes()
    identities=load(root/'capture-identities.json')
    session=cpu['session'];pid=cpu['processId'];nonce=cpu['processNonce']
    require((identities['session'],identities['processId'],identities['processNonce'])==(session,pid,nonce),'capture identity mismatch')
    prefix=root.parent/f'cpu-{pid}-{nonce}-{session}.json'
    require(load(prefix)==cpu,'original CPU differs from watcher copy')
    require(identity(prefix.with_name(prefix.name+'.inputs.bin'))==pins['inputs.bin'],'original input binary differs')
    require(identity(prefix.with_name(prefix.name+'.inputs.json'))==pins['inputs.json'],'original input manifest differs')
    original_pins={str(path):identity(path) for path in (prefix,prefix.with_name(prefix.name+'.inputs.bin'),prefix.with_name(prefix.name+'.inputs.json'))}
    launch=identities.get('launcherPreflight',{});files=launch.get('files',[])
    dll=[f for f in files if Path(f['path']).name.lower()=='d3d9.dll']
    require(len(dll)==1 and dll[0]['sha256']==info['dllSha256'],'capture DLL does not match source package')
    require(files==identities['after'],'capture-time file identities changed')
    env=launch['environment']
    for key,value in {'DXVK_WAR3_FRAME_EVIDENCE':'1','DXVK_WAR3_FRAME_EVIDENCE_INPUTS':'1',
                      'DXVK_WAR3_FRAME_EVIDENCE_DRAWS':'1'}.items():require(env.get(key)==value,'capture gate mismatch: '+key)
    ca=analyze(cpu)
    valid,paths,image_pins,manifest_pin=validate(root)
    require(valid==manifest,'manifest reread differs')
    ha=analyze_history(manifest,cpu,root)
    print('All images and CPU links validated; reconstructing GPU inputs.',flush=True)
    ia=analyze_inputs(inputs,binary,cpu)
    require(ha==load(root/'history-analysis.json'),'fresh image analysis differs')
    require(ca==load(root/'cpu-analysis.json'),'fresh CPU analysis differs')
    require(ia==load(root/'inputs-analysis.json'),'fresh input analysis differs')
    frames={f['frame'] for f in manifest['frames']}
    selected=[r for r in ia['draws'] if r['frame'] in frames]
    focus=[r for r in selected if r['focus'] and r['actualDraws']]
    counts=collections.Counter((r['stride'],r.get('flags',-1)) for r in focus if r['reconstructed'])
    linked={r['frame'] for r in selected if r['reconstructed']}
    focus_linked={r['frame'] for r in focus if r['reconstructed']}
    unmatched=set(ca['unmatchedSpanEvents'])
    internal_unmatched=[e['sequence'] for e in cpu['events'] if int(e['sequence']) in unmatched and e['frame'] in frames]
    missing=sorted(frames-linked,key=int);missing_focus=sorted(frames-focus_linked,key=int)
    by_frame=[]
    for i,f in enumerate(manifest['frames']):
        rows=[r for r in selected if r['frame']==f['frame']]
        targets=[r for r in rows if r['focus'] and r['actualDraws']]
        by_frame.append(dict(videoFrame=i,renderFrame=f['frame'],present=f['present'],qpc=f['qpc'],
            focused=len(targets),focusedReconstructed=sum(r['reconstructed'] for r in targets),
            reconstructed=sum(r['reconstructed'] for r in rows),
            missingSpans=sum(len(r['missing']) for r in rows),
            stride12Focused=sum(r['reconstructed'] and r['stride']==12 for r in targets)))
    ready=(not missing and not missing_focus and not internal_unmatched and
           int(cpu['producerLosses'])==0 and ia['captureDrops']==0 and
           len(focus)>0 and all(r['reconstructed'] for r in focus))
    result=dict(schema=1,usableForBoundedInputResearch=ready,rootCauseReady=False,
        needsUserBadFrameLabels=True,processId=pid,session=session,sourceHistory=str(root),
        sourceUnchanged=True,files=pins,externalOriginals=original_pins,images=len(paths),
        preTriggerSeconds=ha['preTriggerSeconds'],retainedSeconds=ha['retainedSeconds'],
        firstRenderFrame=manifest['frames'][0]['frame'],lastRenderFrame=manifest['frames'][-1]['frame'],
        width=manifest['width'],height=manifest['height'],cpuProducerLosses=cpu['producerLosses'],
        inputCaptureDrops=ia['captureDrops'],inputBatchCount=ia['batchCount'],
        wholeRetainedInputSummary={k:v for k,v in ia.items() if k not in ('draws','limitations')},
        imageWindowFocusDraws=len(focus),imageWindowFocusReconstructed=sum(r['reconstructed'] for r in focus),
        imageWindowFocusRoutes={f'stride{s}/flags{f}':n for (s,f),n in counts.items()},
        missingInputFrames=missing,missingFocusFrames=missing_focus,unmatchedCpuSpansInsideImageWindow=internal_unmatched,
        dllFileMatchesFrozenSourcePackage=True,loadedModuleHashVerified=identities['loadedModuleHashVerified'],
        mapPathSpecified=launch.get('mapPathSpecified',False),mapLoadedVerified=launch.get('mapLoadedVerified',False),
        inheritedWaterRenderEnvironment=env.get('DXVK_WAR3_WATER_RENDER'),
        limitations=['Full capture remains false: nonfocused spans hit capacity limits.',
          'No pixel draw-ID, alpha texture pixels or intermediate depth/shadow/volume attachments.',
          'File pins do not independently hash loaded memory; loaded map identity was not captured.',
          'Water environment value is not proof a Water implementation ran.',
          'This is not visual fault identification, GPU event certification or performance acceptance.'])
    for name,pin in pins.items():require(identity(root/name)==pin,'source changed during audit: '+name)
    for path,pin in original_pins.items():require(identity(Path(path))==pin,'external source changed during audit')
    for path,pin in zip(paths,image_pins):require(identity(path)=={'size':pin['bytes'],'sha256':pin['sha256']},'image changed during audit')
    a.output.mkdir(parents=True)
    for name,value in [('audit.json',result),('frame-coverage.json',by_frame),('image-identities.json',image_pins)]:
        with (a.output/name).open('x',encoding='utf-8') as f:json.dump(value,f,indent=2,allow_nan=False)
    print(json.dumps({k:v for k,v in result.items() if k not in ('files','externalOriginals','wholeRetainedInputSummary','limitations')},indent=2))
if __name__=='__main__':main()
