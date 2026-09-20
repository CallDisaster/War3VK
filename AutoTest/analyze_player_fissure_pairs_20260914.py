"""Offline pixel evidence only; changed components are not a GPU root cause."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DESKTOP = Path('C:/Users/Administrator/Desktop')
OUT = ROOT / 'AutoTest/artifacts/issue8_player_pairs_20260914'
PAIRS = {
    'color': ('Warcraft III 2026.09.14 - 21.57.16.02.DVR.mp4.00_00_16_58.Still001.png',
              'Warcraft III 2026.09.14 - 21.57.16.02.DVR.mp4.00_00_16_59.Still002.png'),
    'factor': ('Warcraft III 2026.09.14 - 22.15.27.05.DVR.mp4.00_02_50_58.Still001.png',
               'Warcraft III 2026.09.14 - 22.15.27.05.DVR.mp4.00_02_50_59.Still002.png'),
}

def identity(p):
    return dict(path=str(p), bytes=p.stat().st_size,
                sha256=hashlib.sha256(p.read_bytes()).hexdigest().upper())

def components(mask, delta):
    kernel = np.ones((3, 3), np.uint8)
    cleaned = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_OPEN, kernel)
    n, labels, stats, centers = cv2.connectedComponentsWithStats(cleaned, 8)
    rows = []
    for i in range(1, n):
        x,y,w,h,area = (int(v) for v in stats[i])
        if area < 80:
            continue
        values = delta[labels == i]
        rows.append(dict(bbox=[x,y,x+w,y+h], area=area,
                         meanSignedGrayDelta=round(float(values.mean()), 3)))
    return sorted(rows, key=lambda r:r['area'], reverse=True)[:15]

def details():
    target = OUT / 'detail'
    target.mkdir(exist_ok=False)
    all_rows=[]
    canvas=Image.new('RGB',(1250,630),(24,24,24))
    for row,(name,box) in enumerate((('color',(635,865,975,1075)),('factor',(1920,355,2220,620)))):
        ims=[Image.open(OUT/f'{name}-{n}.png').convert('RGB') for n in (58,59)]
        crops=[im.crop(box) for im in ims]
        gray=[cv2.cvtColor(np.asarray(im),cv2.COLOR_RGB2GRAY).astype(np.float32) for im in crops]
        delta=gray[1]-gray[0]
        heat=np.zeros((*delta.shape,3),dtype=np.uint8)
        heat[delta>32]=(0,210,255)
        heat[delta<-32]=(255,50,50)
        panels=crops+[Image.fromarray(heat)]
        for col,(panel,label) in enumerate(zip(panels,(f'{name}: file 58',f'{name}: file 59','Red: darker in 59; cyan: lighter in 59'))):
            panel.thumbnail((400,275),Image.Resampling.NEAREST)
            x,y=10+col*415,10+row*310
            canvas.paste(panel,(x,y+22))
            ImageDraw.Draw(canvas).text((x,y),label,fill='white')
        dark=components(delta<-32,delta); light=components(delta>32,delta)
        for group in (dark,light):
            for r in group:
                r['bbox']=[r['bbox'][0]+box[0],r['bbox'][1]+box[1],r['bbox'][2]+box[0],r['bbox'][3]+box[1]]
        all_rows.append(dict(pair=name,roi=box,darkerIn59=dark[:3],lighterIn59=light[:3]))
    canvas.save(target/'pair-comparison.png')
    context=dict(note='Current disk identity only; not proof of DLL loaded in either historical video.',files=[])
    for label,path in (('player-current.log',Path('E:/Work/Warcraft III/war3_d3d9.log')),
                       ('player-current.dll',Path('E:/Work/Warcraft III/d3d9.dll'))):
        before=identity(path)
        if label.endswith('.log'):
            with path.open('rb') as src,(target/label).open('xb') as dst:
                shutil.copyfileobj(src,dst)
            if identity(target/label)['sha256'] != before['sha256']:
                raise RuntimeError('Live log changing during read; preserve partial copy, do not certify')
        if identity(path)!=before:
            raise RuntimeError('Current identity changed')
        context['files'].append(before)
    with (target/'detail.json').open('x',encoding='utf-8') as f:
        json.dump(dict(regions=all_rows,currentContext=context),f,indent=2)
    print(json.dumps(all_rows,indent=2))

def main():
    OUT.mkdir(parents=True, exist_ok=False)
    report = dict(rootCauseEstablished=False, coordinates='original PNG pixels; right/bottom exclusive',
                  method='unregistered 8-bit grayscale delta; 32 threshold; 3x3 opening; UI excluded',
                  warning='Animation, compression and camera motion also cause changes. No frame-to-log identity.', pairs={})
    for name, paths in PAIRS.items():
        sources = [DESKTOP / p for p in paths]
        identities = [identity(p) for p in sources]
        ims = [Image.open(p).convert('RGB') for p in sources]
        if ims[0].size != ims[1].size:
            raise RuntimeError('Pair extent mismatch')
        for index,p in enumerate(sources):
            dest = OUT / f'{name}-{58+index}.png'
            with p.open('rb') as src, dest.open('xb') as dst:
                shutil.copyfileobj(src,dst)
            if identity(dest)['sha256'] != identities[index]['sha256'] or identity(p) != identities[index]:
                raise RuntimeError('Source changed or copy mismatch')
        a,b = [np.asarray(im) for im in ims]
        gray = [cv2.cvtColor(im,cv2.COLOR_RGB2GRAY).astype(np.float32) for im in (a,b)]
        delta = gray[1]-gray[0]
        height,width = delta.shape
        world = np.zeros(delta.shape,dtype=bool)
        world[90:1005,145:width-12] = True
        changed = (np.abs(delta)>32)&world
        darker = components((delta < -32)&world,delta)
        lighter = components((delta > 32)&world,delta)
        heat = np.zeros(a.shape,dtype=np.uint8)
        heat[(delta < -32)&world] = (255,50,50)
        heat[(delta > 32)&world] = (0,210,255)
        Image.fromarray(heat).save(OUT / f'{name}-signed-change.png')
        # Registration estimates are diagnostic only; output differences remain raw.
        shifts = []
        for box in ((350,450,1000,900),(350,90,1000,400),(1400,100,2200,550)):
            x0,y0,x1,y1=box
            shift,response=cv2.phaseCorrelate(gray[0][y0:y1,x0:x1].copy(),gray[1][y0:y1,x0:x1].copy())
            shifts.append(dict(roi=box,dx=round(shift[0],3),dy=round(shift[1],3),response=round(response,3)))
        preview=ims[0].copy()
        draw=ImageDraw.Draw(preview)
        for direction,rows,color in (('darker59',darker,(255,40,40)),('lighter59',lighter,(0,220,255))):
            for j,row in enumerate(rows[:5]):
                x0,y0,x1,y1=row['bbox']
                draw.rectangle((x0,y0,x1-1,y1-1),outline=color,width=3)
                draw.text((x0,y0),f'{direction}:{j+1}',fill=color,stroke_width=1,stroke_fill=(0,0,0))
        preview.save(OUT / f'{name}-component-locations.png')
        report['pairs'][name]=dict(inputs=identities,size=ims[0].size,changedPixels=int(changed.sum()),
                                  darkerIn59=darker,lighterIn59=lighter,phaseEstimates=shifts)
    with (OUT / 'analysis.json').open('x',encoding='utf-8') as f:
        json.dump(report,f,indent=2,ensure_ascii=False)
    print(json.dumps({name:{'size':r['size'],'topDarker59':r['darkerIn59'][:4],
                           'topLighter59':r['lighterIn59'][:4],'shift':r['phaseEstimates']}
                      for name,r in report['pairs'].items()},indent=2))

if __name__ == '__main__':
    details() if '--details' in sys.argv else main()
