"""Lifetime census for two player-identified anomaly signatures, not pixel attribution."""
from collections import defaultdict,Counter
import json
from pathlib import Path
from analyze_frame_evidence import load,analyze

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path('E:/Work/Warcraft III/WarVK/Log/FrameEvidence/history-34548-4617011687416-1')
OUT=ROOT/'AutoTest/artifacts/issue8_localized_20260915_history34548'
HFOO=int.from_bytes(b'hfoo','big')
def runs(values):
    result=[]
    for value in sorted(set(values)):
        if result and value==result[-1][-1]+1:result[-1][1]=value
        else:result.append([value,value])
    return result
def signature(e):
    w=e['words32'];d=e['data']
    return dict(flags=w[16],alphaTest=bool(w[16]&4),useBlend=bool(w[16]&1),indexedBlend=bool(w[16]&2),
                indexCount=w[22],stride=w[28],alphaView=d[9],geometry=d[10],jHandle=w[26],rawcode=d[11],
                vb=d[3],vbSize=d[5],ib=d[6],ibSize=d[8],paletteOffset=w[19],blendCount=w[20],
                paletteIndex=w[32],directSkin=w[33])
def main():
    cpu=load(SOURCE/'cpu-events.json');validation=analyze(cpu);m=load(SOURCE/'manifest.json')
    video={int(row['frame']):i for i,row in enumerate(m['frames'])}
    at=defaultdict(list);bt=defaultdict(lambda:defaultdict(list));snapshot=[]
    for e in cpu['events']:
        f=int(e['frame']);w=e['words32']
        if f not in video or e['kind']!=18 or w[34]!=0 or e['data'][1]!='0':continue
        i=video[f];s=signature(e)
        if w[27]==11 and w[22]==1587 and e['data'][11]=='0':
            label='fallback12' if w[28]==12 and w[16]==3 else 'other'
            at[label].append(i)
            if i in (68,69,70,71,72):snapshot.append(dict(video=i,render=f,case='A',**s))
        if w[27]==11 and w[22]==105 and int(e['data'][11])==HFOO:
            label='fallback12-noalpha' if w[28]==12 and w[16]==3 else 'native32-alpha' if w[28]==32 and w[16]&4 else 'other'
            bt[str(w[26])][label].append(i)
            if i in (102,103,105,106,107):snapshot.append(dict(video=i,render=f,case='B',**s))
    result=dict(scope='one recorded process, surface cascade0, indexCount+handle signatures; no pixel draw-ID',
                videoToRenderOffset=3927,caseA={k:runs(v) for k,v in at.items()},
                footman={h:{k:runs(v) for k,v in states.items()} for h,states in bt.items()},
                selected=snapshot,producerLosses=cpu['producerLosses'],schema=cpu['schema'],
                sourceCapabilities=cpu['capabilities'],rootCauseReady=False)
    with (OUT/'route-lifetimes.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k!='selected'},indent=2))
if __name__=='__main__':main()
