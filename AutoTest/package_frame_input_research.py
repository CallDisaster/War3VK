"""CreateNew local research zip, selected marked frames plus adjacent controls.

Does not upload, start games, edit source evidence or label a cause as proven.
All frame numbers use zero-based manifest/video ordering, not TGA slot numbers.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import zipfile
import copy
from PIL import Image
from analyze_frame_evidence import load,require
from analyze_frame_inputs import analyze_inputs

def parse_frames(text,count):
    result=set()
    for part in text.split(','):
        values=part.split('-');require(1<=len(values)<=2 and all(v.isdecimal() for v in values),'bad frame syntax')
        lo=int(values[0]);hi=int(values[-1]);require(0<=lo<=hi<count,'bad frame range')
        result.update(range(lo,hi+1))
    require(result,'mark at least one frame');return result

def select_evidence(cpu,inputs,binary,frame_ids):
    """Explicit derivative, never impersonates a full schema-6 CPU export."""
    events=[e for e in cpu['events'] if e.get('frame') in frame_ids or e.get('kind')==8]
    selected_cpu=dict(selectionSchema=1,session=cpu['session'],processNonce=cpu['processNonce'],
                      originalHeader={k:v for k,v in cpu.items() if k!='events'},events=events)
    selected=copy.deepcopy({k:v for k,v in inputs.items() if k!='batches'})
    selected['batches']=[];payload=bytearray()
    for original in inputs['batches']:
        if original['frame'] not in frame_ids:continue
        b=copy.deepcopy(original);offset=int(b['fileOffset']);n=b['bytes']
        require(0<=offset<=len(binary) and 0<=n<=len(binary)-offset,'selected binary range')
        b['originalFileOffset']=b['fileOffset'];b['fileOffset']=str(len(payload))
        payload.extend(binary[offset:offset+n]);selected['batches'].append(b)
    selected['binaryBytes']=str(len(payload));selected['binary']='inputs-selected.bin'
    selected['derivation']={'kind':'selected-frame-window','frames':sorted(frame_ids,key=int),
                            'allRecordsWithinSelectedBatchesPreserved':True}
    return selected_cpu,selected,bytes(payload)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('history',type=Path);p.add_argument('--bad-frames',required=True)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    a=p.parse_args();root=a.history.resolve();source=a.source_root.resolve()
    manifest=load(root/'manifest.json');cpu=load(root/'cpu-events.json');inputs=load(root/'inputs.json')
    binary=(root/'inputs.bin').read_bytes();analysis=analyze_inputs(inputs,binary,cpu)
    frames=manifest['frames'];require(manifest['state']==6 and manifest['saved']==len(frames) and not manifest['failed'],'unfinished history')
    bad=parse_frames(a.bad_frames,len(frames))
    selected=sorted({j for i in bad for j in range(max(0,i-4),min(len(frames),i+5))})
    require(len(selected)<=80,'use separate packages for more than 80 selected neighboring frames')
    files={};mapping=[];source_binding=False
    package_pin=load(source/'package-info.json') if (source/'package-info.json').is_file() else None
    if package_pin and (root/'capture-identities.json').is_file():
        captured=load(root/'capture-identities.json')
        dlls=[f for f in captured.get('launcherPreflight',{}).get('files',[]) if Path(f['path']).name.lower()=='d3d9.dll']
        if dlls:
            require(len(dlls)==1 and dlls[0]['sha256']==package_pin['dllSha256'],'candidate/source package identity mismatch')
            source_binding=True
    source_pins={}
    for name in ('cpu-events.json','inputs.json','inputs.bin','manifest.json'):
        path=root/name
        with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest().upper()
        source_pins[name]=dict(size=path.stat().st_size,sha256=digest)
    sc,si,sb=select_evidence(cpu,inputs,binary,{frames[i]['frame'] for i in selected})
    derived_analysis=analyze_inputs(si,sb,sc)
    si['sourcePins']=source_pins;sc['sourcePins']=source_pins
    def add(name,data):
        require(name not in files,'duplicate package member')
        files[name]=dict(size=len(data),sha256=hashlib.sha256(data).hexdigest().upper());archive.writestr(name,data)
    # No overwrite, including partial archives from failed validation.
    with zipfile.ZipFile(a.output,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for name in ('manifest.json','cpu-analysis.json','history-analysis.json','capture-identities.json'):
            path=root/name
            if path.is_file():add('evidence/'+name,path.read_bytes())
        add('evidence/cpu-events-selected.json',json.dumps(sc,separators=(',',':')).encode())
        add('evidence/inputs-selected.json',json.dumps(si,separators=(',',':')).encode())
        add('evidence/inputs-selected.bin',sb)
        add('evidence/inputs-selected-analysis.json',json.dumps(derived_analysis,separators=(',',':')).encode())
        for i in selected:
            f=frames[i];path=root/Path(f['file']).name
            require(path.resolve().parent==root and f['saved'],'invalid image reference')
            original=path.read_bytes();require(len(original)==18+manifest['width']*manifest['height']*4,'image size')
            with Image.open(io.BytesIO(original)) as img:
                require(img.size==(manifest['width'],manifest['height']),'image dimensions')
                encoded=io.BytesIO();img.save(encoded,format='PNG');pixels=img.tobytes()
                with Image.open(io.BytesIO(encoded.getvalue())) as check:require(check.tobytes()==pixels,'PNG pixel roundtrip')
            name=f'frames/video-{i:04d}-render-{f["frame"]}.png';add(name,encoded.getvalue())
            mapping.append(dict(videoFrame=i,markedBad=i in bad,renderFrame=f['frame'],present=f['present'],qpc=f['qpc'],
                                image=name,sourceTga=path.name,sourceSha256=hashlib.sha256(original).hexdigest().upper()))
        add('FRAME_MAPPING.json',json.dumps(mapping,indent=2).encode())
        marked_renders={frames[i]['frame'] for i in bad}
        focused=[r for r in analysis['draws'] if r['frame'] in marked_renders and r['focus']]
        add('MARKED_INPUT_SUMMARY.json',json.dumps(focused,indent=2).encode())
        sources=(
            'src/d3d9/d3d9_device.cpp','src/d3d9/d3d9_war3_shadow.cpp','src/d3d9/d3d9_war3_shadow_resources.cpp',
            'src/d3d9/d3d9_war3_scene.h','src/d3d9/d3d9_war3_pipeline.cpp','src/util/util_matrix.h',
            'src/d3d9/war3/tools/war3_frame_inputs.cpp','src/d3d9/war3/tools/war3_frame_inputs_core.h',
            'src/d3d9/shaders/war3_frame_input_gather.comp','src/dxvk/dxvk_memory.cpp','src/dxvk/dxvk_memory.h','src/dxvk/dxvk_buffer.cpp',
            'src/d3d9/war3/render/war3_shadow_drawtime_cache_policy.h','src/d3d9/war3/render/war3_canonical_draw.h',
            'src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp','src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp',
            'subprojects/war3fx/shaders/war3_shadow_caster_vert.vert','subprojects/war3fx/shaders/war3_shadow_caster_frag.frag',
            'AutoTest/analyze_frame_inputs.py','AutoTest/analyze_frame_evidence.py','AutoTest/test_frame_inputs.py',
            'docs/research/2026-09-15-frame-input-evidence.md','docs/research/2026-09-15-fissure-localized-route-evidence.md')
        missing=[]
        for rel in sources:
            path=source/rel
            if path.is_file():
                data=path.read_bytes()
                if package_pin:
                    pin=package_pin['files'].get(rel)
                    require(pin and hashlib.sha256(data).hexdigest().upper()==pin['sha256'],'frozen source changed: '+rel)
                add('source/'+rel,data)
            else:missing.append(rel)
        prompt='''# WarVK transient shadow fissure research request

Investigate the marked zero-based video frames in FRAME_MAPPING.json. Adjacent
unmarked frames are controls, not guaranteed universally artifact-free. Use
render/Present/QPC mapping, not file slots or 60fps playback timing.

The selected files are explicit derivatives, not whole-session exports. Original
headers, full-source SHA/size and original binary offsets are retained. All records
inside each selected batch are preserved, including missing/capacity statuses.
Use scripts to filter the raw data; do not dump all JSON into model context.

First audit evidence coverage and schema. Recompute binary span hashes, validate
fence completion and match batch/draw physical bindings. Then reconstruct the
indexed vertices with negative baseVertex, selected palette and actual lightVP.
Compare before/during/after representations by epoch + part/layer + identity;
never treat drawIndex, zero hash or a 52/105 signature alone as object identity.

Previous sample implicated stride32 drawn snapshots changing to stride12 indexed
semantic skinning. That is a hypothesis, not the answer. Test wrong palette/group,
coordinate space/double transform, freshness, index rebasing, material/alpha and
resource ownership alternatives. Separate measured facts, source-derived findings
and speculation; cite exact files/records/fields for each claim.

Read limitations in frame-input-evidence.md. The package does NOT contain pixel
draw-ID, texture alpha pixels or intermediate GPU attachments. A CPU reconstruction
does not prove actual GPU output. Do not claim a definite pixel cause without that
link, do not assume original MDX asset identity, and do not propose disabling all
dynamic casters as an accepted fix. Explain which hypothesis is falsified, which
remain, and the smallest safe discriminating test or source correction. Provide
rollback conditions, correctness tests and expected performance risk. If information
is missing, enumerate specific fields/events, not another blanket request to log
everything. No release acceptance or upstream issue response is requested.
'''
        add('RESEARCH_PROMPT.md',prompt.encode())
        add('PACKAGE.json',json.dumps(dict(rootCauseReady=False,markedFrames=sorted(bad),
            files=files.copy(),missingSourceFiles=missing,sourceIdentity='bundled source snapshot; validate against candidate pin',
            launcherDllMatchedFrozenSourcePackage=source_binding,
            binarySha256=analysis['binarySha256']),indent=2).encode())
    with zipfile.ZipFile(a.output) as z:
        require(z.testzip() is None,'archive CRC failure')
        for name,pin in files.items():require(hashlib.sha256(z.read(name)).hexdigest().upper()==pin['sha256'],'archive hash')
    print(json.dumps(dict(path=str(a.output.resolve()),bytes=a.output.stat().st_size,sha256=hashlib.sha256(a.output.read_bytes()).hexdigest().upper(),
                         selectedFrames=len(selected),rootCauseReady=False),indent=2))
if __name__=='__main__':main()
