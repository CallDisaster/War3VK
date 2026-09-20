"""Counter-based caster identity matching: draw indices are not stable identities."""
from collections import Counter,defaultdict
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'AutoTest/artifacts/issue8_localized_20260915_history34548'
SOURCE=Path('E:/Work/Warcraft III/WarVK/Log/FrameEvidence/history-34548-4617011687416-1')
def rawcode(n):
    n=int(n);return ''.join(chr(b) if 32<=b<127 else '.' for b in n.to_bytes(4,'big'))
def summary(e):
    d=e['data'];w=e['words32']
    return dict(index=int(d[0]),geometry=d[10],rawcode=d[11],fourCC=rawcode(d[11]),handle=w[26],stage=w[27],
                flags=hex(w[16]),alphaRef=struct.unpack('<f',struct.pack('<I',w[17]))[0],
                count=w[22],indexed=w[21],first=w[23],vertexOffset=w[24],stride=w[28],positionOffset=w[29],
                uvOffset=w[30],uvFormat=w[31],palette=w[32],paletteOffset=w[19],pipeline=d[2],alphaView=d[9],
                vb=d[3],vbOffset=d[4],vbSize=d[5],ib=d[6],ibOffset=d[7],ibSize=d[8])
def signature(e):
    d=e['data'];w=e['words32']
    return tuple(d[9:12]+[w[26],w[27],w[16],w[17],w[18],w[21],w[22],w[25],w[28],w[29],w[30],w[31]])

def main():
    events=json.loads((OUT/'selected-events.json').read_text())
    by_video={int(k)-3927:[e for e in es if e['kind']==18 and e['words32'][34]==0 and e['data'][1]=='0'] for k,es in events.items()}
    result={}
    for a,b in ((68,69),(71,72),(102,103),(105,106),(106,107)):
        left,right=by_video[a],by_video[b]
        ca,cb=Counter(map(signature,left)),Counter(map(signature,right))
        changes={}
        for name,counts,es in (('added',cb-ca,right),('removed',ca-cb,left)):
            rows=[]
            for key,n in counts.items():
                matches=[summary(e) for e in es if signature(e)==key]
                rows.append(dict(multiplicity=n,matches=matches))
            changes[name]=rows
        result[f'{a}-{b}']=changes
    with (OUT/'caster-set-changes.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    for pair,changes in result.items():
        print(pair)
        for k,rows in changes.items():
            print(k,[(r['multiplicity'],[{q:s[q] for q in ('index','geometry','fourCC','handle','stage','flags','count','stride','vbSize','ibSize')} for s in r['matches']]) for r in rows])
    m=json.loads((SOURCE/'manifest.json').read_text())
    for name,indices,box in (('A',[68,69,71,72],(320,70,1100,360)),('B',[102,103,106,107],(560,480,1030,690))):
        w,h=box[2]-box[0],box[3]-box[1]
        sheet=Image.new('RGB',(w*2,(h+25)*2),(20,20,20));d=ImageDraw.Draw(sheet)
        for j,i in enumerate(indices):
            with Image.open(SOURCE/Path(m['frames'][i]['file']).name) as im:
                sheet.paste(im.crop(box).convert('RGB'),((j%2)*w,(j//2)*(h+25)+25))
            d.text(((j%2)*w+6,(j//2)*(h+25)+6),f'VIDEO {i} / RENDER {3927+i}',fill='white')
        sheet.save(OUT/f'{name}-local.png')
if __name__=='__main__':main()
