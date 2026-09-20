"""Read-only marked-frame reconstruction using the delivered frozen reader.
No full-frame pixel blame, no production mutation. CreateNew derived output only.
"""
import collections
import hashlib
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[1]
HISTORY=Path('E:/Work/Warcraft III/WarVK/Log/FrameEvidence/history-53372-4805636754173-1')
PACKAGE=Path('C:/Users/Administrator/Desktop/WarVK-v1.22-FrameInputs-20260915')
OUT=ROOT/'AutoTest/artifacts/marked_skinning_53372_25_29_20260915'
sys.path.insert(0,str(PACKAGE/'AutoTest'))
from analyze_frame_evidence import load,require
from analyze_frame_inputs import analyze_inputs,reconstruct
from package_frame_input_research import select_evidence

def sha(data):return hashlib.sha256(data).hexdigest().upper()
def span_data(batch,draw,binary):
    return [binary[int(batch['fileOffset'])+s['offset']:int(batch['fileOffset'])+s['offset']+int(s['bytes'])]
            if s['status']==1 and batch['gpuComplete'] else b'' for s in draw['spans']]
def main():
    require(not OUT.exists(),'output exists')
    pins=load(ROOT/'AutoTest/artifacts/player_input_audit_53372_20260915/audit.json')['files']
    for name,pin in pins.items():
        p=HISTORY/name
        with p.open('rb') as f:require(hashlib.file_digest(f,'sha256').hexdigest().upper()==pin['sha256'],'source hash '+name)
    cpu=load(HISTORY/'cpu-events.json');inputs=load(HISTORY/'inputs.json');binary=(HISTORY/'inputs.bin').read_bytes()
    history=load(HISTORY/'manifest.json');first=int(history['frames'][0]['frame']);bad={first+i for i in range(25,30)}
    # Include the baseline BEFORE early fallback population starts at VIDEO21.
    selected_frames={str(first+i) for i in range(17,35)}
    sc,si,sb=select_evidence(cpu,inputs,binary,selected_frames)
    derived=analyze_inputs(si,sb,sc)
    events=collections.defaultdict(list)
    for e in cpu['events']:
        if e['kind']==18:
            words=e['words32'];bid=words[40]|words[41]<<32
            if bid:events[(bid,int(e['data'][0]))].append(e)
    rows=[];arrays={};identities={};draws={}
    for b in inputs['batches']:
        frame=int(b['frame']);video=frame-first
        if not 0<=video<=34:continue
        for d in b['draws']:
            es=events[(int(b['id']),d['index'])]
            if not d['words'][37] or not es:continue
            parts=span_data(b,d,binary)
            try:r=reconstruct(d,parts,es[0])
            except ValueError:continue
            world=r['world'][r['indices']] # expanded triangle-corner order
            corners=world[:,:3].reshape(-1,3,3)
            lengths=np.concatenate([np.linalg.norm(corners[:,i]-corners[:,(i+1)%3],axis=1) for i in range(3)])
            ident=(b['mapEpoch'],b['deviceEpoch'],d['part'],d['words'][1],d['words'][3],d['words'][8])
            key=(video,bool(b['volume']),ident)
            raw=parts[1];indexsize=2 if d['words'][7]==0 else 4
            ix=np.frombuffer(raw,dtype='<u2' if indexsize==2 else '<u4',count=d['words'][8],offset=d['words'][9]*indexsize).astype(np.int64)
            normalized=ix-ix.min()
            row=dict(video=video,render=frame,markedBad=frame in bad,volume=b['volume'],batch=b['id'],draw=d['index'],
                part=d['part'],handle=d['words'][1],layer=d['words'][3],indexCount=d['words'][8],
                stride=d['words'][4],flags=es[0]['words32'][16],actualCascades=[int(e['data'][1]) for e in es],
                uniqueReferencedVertices=len(np.unique(ix)),maxBoneIndex=r['maxBoneIndex'],
                maxTriangleEdge=float(lengths.max()),worldMin=world[:,:3].min(axis=0).tolist(),worldMax=world[:,:3].max(axis=0).tolist(),
                provenance=d['provenance'],spanStatus=[s['status'] for s in d['spans']],
                spanSha256=[sha(p) if p else None for p in parts],relativeTopologySha256=sha(normalized.astype('<i8').tobytes()))
            arrays[key]=world;identities[key]=normalized;draws[key]=(b,d,parts,es)
            rows.append(row)
    comparisons=[];pair_comparisons=[]
    for row in rows:
        if not row['markedBad']:continue
        ident=next(k[2] for k in arrays if k[0]==row['video'] and k[1]==row['volume'] and k[2][2]==row['part'] and k[2][3]==row['handle'] and k[2][4]==row['layer'] and k[2][5]==row['indexCount'])
        key=(row['video'],row['volume'],ident)
        if row['stride']==12:
            baselines=[r for r in rows if not r['volume'] and r['video']<row['video'] and r['stride']==32 and
                       (r['part'],r['handle'],r['layer'],r['indexCount'])==(row['part'],row['handle'],row['layer'],row['indexCount'])]
            if baselines:
                base=max(baselines,key=lambda r:r['video']);basekey=(base['video'],False,ident)
                topology=np.array_equal(identities[key],identities[basekey]);diff=arrays[key][:,:3]-arrays[basekey][:,:3]
                distances=np.linalg.norm(diff,axis=1)
                comparisons.append(dict(video=row['video'],volume=row['volume'],part=row['part'],handle=row['handle'],
                    baselineVideo=base['video'],relativeIndexSequenceEqual=topology,maxCornerDistance=float(distances.max()),
                    medianCornerDistance=float(np.median(distances)),cornersOver1=int((distances>1).sum()),
                    cornersOver10=int((distances>10).sum()),maxEdgeBefore=base['maxTriangleEdge'],maxEdgeAfter=row['maxTriangleEdge']))
        other=(row['video'],not row['volume'],ident)
        if not row['volume'] and other in arrays:
            pair_comparisons.append(dict(video=row['video'],part=row['part'],handle=row['handle'],
                exactWorldCornersEqual=bool(np.array_equal(arrays[key],arrays[other])),
                maxWorldCornerDistance=float(np.linalg.norm(arrays[key]-arrays[other],axis=1).max())))
    OUT.mkdir(parents=True)
    # Self-contained exact selected raw bytes/metadata, no repeated full raw dump.
    for name,data in [('cpu-events-selected.json',sc),('inputs-selected.json',si),('inputs-selected-analysis.json',derived),
                      ('focused-timeline.json',rows),('skinning-comparisons.json',comparisons),('surface-volume-comparisons.json',pair_comparisons)]:
        with (OUT/name).open('x',encoding='utf-8') as f:json.dump(data,f,indent=2,allow_nan=False)
    with (OUT/'inputs-selected.bin').open('xb') as f:f.write(sb)
    npz={}
    for key,world in arrays.items():
        video,volume,ident=key
        if 17<=video<=34:
            name=f'v{video}_vol{int(volume)}_part{ident[2]}_h{ident[3]}_layer{ident[4]}_n{ident[5]}'
            npz[name]=world
    np.savez_compressed(OUT/'reconstructed-triangle-corners.npz',**npz)
    # A few relevant original views, no inference that all unmarked views are good.
    views=(24,25,29,30);sheet=Image.new('RGB',(1200,720),(18,18,18));paint=ImageDraw.Draw(sheet)
    for pos,index in enumerate(views):
        path=HISTORY/Path(history['frames'][index]['file']).name
        with Image.open(path) as im:
            sheet.paste(im.convert('RGB').crop((700,100,1500,560)).resize((600,345)),((pos%2)*600,(pos//2)*360+15))
        paint.text(((pos%2)*600+8,(pos//2)*360),f'VIDEO {index} / RENDER {first+index}',fill='white')
    sheet.save(OUT/'roi-24-25-29-30.png')
    attachments=[Path('D:/tmp/AppData/ADMINI~1/Local/Temp/codex-clipboard-9c662ec2-39df-4b5e-a430-4578f0ab9b83.png'),
                 Path('D:/tmp/AppData/ADMINI~1/Local/Temp/codex-clipboard-c4e7900e-d1ff-4713-9a5c-9fd5a28fff80.png')]
    small=[]
    for f in history['frames']:
        with Image.open(HISTORY/Path(f['file']).name) as im:small.append(np.asarray(im.convert('RGB').resize((320,180)),dtype=np.int16))
    matches=[]
    for path in attachments:
        with Image.open(path) as im:target=np.asarray(im.convert('RGB').resize((320,180)),dtype=np.int16)
        scores=[float(np.abs(x-target).mean()) for x in small];order=np.argsort(scores)[:3]
        matches.append(dict(file=str(path),sha256=sha(path.read_bytes()),candidates=[dict(video=int(i),meanAbsRgb=scores[i]) for i in order]))
    per_frame=[]
    for video in range(17,35):
        a=[r for r in rows if r['video']==video]
        per_frame.append(dict(video=video,render=first+video,markedBad=25<=video<=29,focus=len(a),
                             surface=sum(not r['volume'] for r in a),volume=sum(r['volume'] for r in a),
                             stride12=sum(r['stride']==12 for r in a)))
    result=dict(markedVideos=list(range(25,30)),selectedVideos=list(range(17,35)),frameSummary=per_frame,
        badFirstToFirstRecoveryMs=(int(history['frames'][30]['qpc'])-int(history['frames'][25]['qpc']))*1000/int(history['qpcFrequency']),
        comparisons=comparisons,pairComparisons=pair_comparisons,attachmentMatches=matches,rootCauseReady=False)
    with (OUT/'summary.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2,allow_nan=False)
    print(json.dumps(dict(path=str(OUT),durationMs=result['badFirstToFirstRecoveryMs'],frames=per_frame,
        largestComparisons=sorted(comparisons,key=lambda x:x['maxCornerDistance'],reverse=True)[:6],
        surfaceVolumeUnequal=sum(not x['exactWorldCornersEqual'] for x in pair_comparisons),attachmentMatches=matches),indent=2))
if __name__=='__main__':main()
