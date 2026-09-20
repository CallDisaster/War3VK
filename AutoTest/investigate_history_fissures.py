"""Read-only correlation of player-labelled video intervals; never a root-cause gate."""
import argparse
from collections import Counter,defaultdict
import hashlib
import json
from pathlib import Path
import struct
import cv2
import numpy as np
from PIL import Image,ImageDraw
from analyze_frame_evidence import load,analyze

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest().upper()
def digest(x):return hashlib.sha256(json.dumps(x,separators=(',',':')).encode()).hexdigest()
def components(delta):
    out={}
    for name,mask in (('darker',delta<-10),('lighter',delta>10)):
        mask[:85]=0;mask[1080:]=0;mask[:,:135]=0
        mask=cv2.morphologyEx(mask.astype('uint8'),cv2.MORPH_OPEN,np.ones((3,3),np.uint8))
        count,labels,stats,_=cv2.connectedComponentsWithStats(mask,8)
        rows=[]
        for i in range(1,count):
            x,y,w,h,n=map(int,stats[i])
            if n<30:continue
            rows.append(dict(box=[x,y,x+w,y+h],pixels=n,mean=float(delta[labels==i].mean())))
        out[name]=sorted(rows,key=lambda r:r['pixels'],reverse=True)[:8]
    return out
def draw_key(e):return (e['words32'][34],int(e['data'][1]),int(e['data'][0]))
def draw_parts(e):
    d=e['data'];w=e['words32']
    return {'identity':d[:3]+d[9:12]+[w[26],w[27],w[34]],
            'alpha':d[9:10]+w[16:19]+w[30:32],
            'drawState':d[10:12]+w[21:34],
            'buffers':d[3:9], 'lightVp':w[:16], 'paletteAddressing':w[19:21]+w[32:34],
            'target':w[34:38]}

def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    m=load(root/'manifest.json');cpu=load(root/'cpu-events.json');validation=analyze(cpu)
    rows=m['frames'];selected=sorted(set(range(66,75))|set(range(100,110)))
    frames={int(rows[i]['frame']) for i in selected};by_frame=defaultdict(list)
    for e in cpu['events']:
        f=int(e['frame'])
        if f in frames:by_frame[f].append(e)
    pins={name:sha(root/name) for name in ('manifest.json','cpu-events.json','history-analysis.json')}
    images={i:np.asarray(Image.open(root/Path(rows[i]['file']).name).convert('RGB')) for i in selected}
    for i in selected:
        pin=sha(root/Path(rows[i]['file']).name)
        linked=load(root/'history-analysis.json')['frames'][i]
        if pin!=linked['sha256']:raise ValueError('changed image '+str(i))
    summary=[];draws={}
    for i in selected:
        frame=int(rows[i]['frame']);events=by_frame[frame];ds=[e for e in events if e['kind']==18]
        draws[i]={draw_key(e):e for e in ds}
        if len(draws[i])!=len(ds):raise ValueError('duplicate draw key')
        camera=[e for e in events if e['kind']==7];csm=[e for e in events if e['kind']==17];state=[e for e in events if e['kind']==12]
        parts={k:digest([draw_parts(e)[k] for _,e in sorted(draws[i].items())]) for k in draw_parts(ds[0])}
        summary.append(dict(video=i,render=frame,sourceFile=Path(rows[i]['file']).name,qpc=rows[i]['qpc'],
            cameraHash=digest([e['words32'] for e in camera]),cameraConfig=[e['data'] for e in camera],
            csmHashes=[digest(e['words32']) for e in csm],csmData=[e['data'] for e in csm],
            shadowStates=[dict(data=e['data'],words=e['words32']) for e in state],drawPartsHash=parts,
            surfaceDraws=sum(e['words32'][34]==0 for e in ds),volumeDraws=sum(e['words32'][34]!=0 for e in ds),
            alphaCounts=dict(Counter(str((e['words32'][34],bool(e['words32'][16]&4))) for e in ds))))
    comparisons=[]
    for i in selected:
        if i-1 not in images:continue
        gray=[cv2.cvtColor(images[j],cv2.COLOR_RGB2GRAY).astype(np.int16) for j in (i-1,i)]
        delta=gray[1]-gray[0];parts=components(delta)
        common=set(draws[i-1])&set(draws[i]);changes={}
        for part in ('identity','alpha','drawState','buffers','lightVp','paletteAddressing','target'):
            changes[part]=[list(k) for k in sorted(common) if draw_parts(draws[i-1][k])[part]!=draw_parts(draws[i][k])[part]]
        comparisons.append(dict(before=i-1,after=i,pixels=parts,added=[list(k) for k in sorted(set(draws[i])-common)],
                                removed=[list(k) for k in sorted(set(draws[i-1])-common)],changes=changes))
    for label,indices in (('A',[68,69,70,71,72]),('B',[102,103,104,106,107])):
        sheet=Image.new('RGB',(640*len(indices),390),(20,20,20));paint=ImageDraw.Draw(sheet)
        for col,i in enumerate(indices):
            im=Image.fromarray(images[i]);im.thumbnail((640,360))
            sheet.paste(im,(col*640,30));paint.text((col*640+8,8),f'VIDEO {i} / RENDER {rows[i]["frame"]}',fill='white')
        sheet.save(out/f'{label}-overview.png')
    result=dict(source=str(root),sourcePins=pins,frameNumbering='VIDEO zero-based; includes +/- neighbourhood for ambiguity',
                cpuValidation={k:v for k,v in validation.items() if k!='cpuRecordedSpans'},frames=summary,comparisons=comparisons,
                rootCauseEstablished=False)
    with (out/'analysis.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    with (out/'selected-events.json').open('x',encoding='utf-8') as f:json.dump(dict(by_frame),f)
    for name,pin in pins.items():
        if sha(root/name)!=pin:raise ValueError('source changed '+name)
    print(json.dumps({'sourcePins':pins,'frames':[(r['video'],r['render'],r['surfaceDraws'],r['volumeDraws']) for r in summary],
      'transitions':[{'pair':[r['before'],r['after']], 'topDark':r['pixels']['darker'][:2],
                     'topLight':r['pixels']['lighter'][:2], 'changes':{k:len(v) for k,v in r['changes'].items()}}
                     for r in comparisons]},indent=2))
if __name__=='__main__':main()
