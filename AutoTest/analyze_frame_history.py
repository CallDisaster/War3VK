"""Validate retained history and join every image to its exact CPU copy/frame.

No visual/root-cause acceptance. Computes local change scores for triage only.
"""
import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from analyze_frame_evidence import load,analyze,require,u64

def analyze_history(manifest,cpu,image_root=None):
    analyze(cpu)
    require(manifest['schema'] in (1,2) and manifest['state']==6,'history not complete')
    require(manifest['session']==cpu['session'],'history CPU session mismatch')
    require(manifest['captureComplete'] is False and manifest['rootCauseReady'] is False,'unsupported root acceptance')
    require(manifest['failed']==0 and int(manifest['lockDrops'])==0,'history loss/failure')
    rows=manifest['frames'];require(len(rows)==manifest['retained']==manifest['saved'],'image population')
    pre=manifest['retainedPreFrames'] if manifest['schema']==2 else min(manifest['preFrames'],int(manifest['triggerOrdinal']))
    require(0<pre<=manifest['preFrames'],'invalid retained prefix')
    require(len(rows)==pre+manifest['postFrames'],'history window length')
    expected=int(manifest['triggerOrdinal'])-pre+1
    linked=[];previous=None;lastpresent=None;lastqpc=0;seen=set()
    has_draw_records=any(e['kind']==18 for e in cpu['events'])
    def key(e):return tuple(e[k] for k in ('owner','frame','mapEpoch','deviceEpoch','thread'))
    copies_by_id=defaultdict(list);pipelines_by_key=defaultdict(list);shadows_by_key=defaultdict(list);draws_by_key=defaultdict(list)
    for e in cpu['events']:
        if e['kind']==16:copies_by_id[(e['data'][0],e['data'][1])].append(e)
        elif e['kind']==3:pipelines_by_key[key(e)].append(e)
        elif e['kind']==12:shadows_by_key[key(e)].append(e)
        elif e['kind']==18 and e['words32'][34]==0:draws_by_key[key(e)].append(e)
    for row in rows:
        require(row['saved'] is True and u64(row['ordinal'])==expected,'history ordinal discontinuity')
        expected+=1;present=u64(row['present']);qpc=u64(row['qpc'])
        require(lastpresent is None or present==lastpresent+1,'missing Present image')
        require(qpc>lastqpc,'image QPC regression');lastpresent=present;lastqpc=qpc
        target=u64(row['targetValue']);require(target>0 and u64(row['observedValue'])>=target,'readback completion')
        p=(Path(image_root)/Path(row['file']).name) if image_root else Path(row['file'])
        require(p.name not in seen,'duplicate image path');seen.add(p.name)
        copies=[e for e in copies_by_id[(row['ordinal'],row['present'])]
                if all(e[k]==row[k] for k in ('owner','frame','mapEpoch','deviceEpoch'))]
        require(len(copies)==1,'history copy event missing/duplicate')
        copy=copies[0]
        pipelines=[e for e in pipelines_by_key[key(copy)] if u64(e['sequence'])<u64(copy['sequence'])]
        require(len(pipelines)==1,'history pipeline mapping missing/duplicate')
        shadow=shadows_by_key[key(copy)]
        require(len(shadow)==1,'shadow state missing/duplicate')
        draw_info={}
        if has_draw_records:
            draws=draws_by_key[key(copy)]
            expected_draws=u64(shadow[0]['data'][3])
            require(len(draws)==expected_draws,'actual directional draw population mismatch')
            keys={(e['data'][0],e['data'][1]) for e in draws}
            require(len(keys)==len(draws),'duplicate directional draw/cascade')
            draw_info=dict(directionalDraws=len(draws),alphaTestDraws=sum(bool(e['words32'][16]&4) for e in draws),
                           actualDrawStateLinked=True)
        with Image.open(p) as im:
            im.load();require(im.size==(manifest['width'],manifest['height']),'history dimensions')
            rgb=np.asarray(im.convert('RGB').resize((320,180)),dtype=np.int16)
        score=None if previous is None else float(np.abs(rgb-previous).mean())
        linked.append(dict(ordinal=row['ordinal'],present=row['present'],frame=row['frame'],file=str(p),
                           sha256=hashlib.sha256(p.read_bytes()).hexdigest().upper(),
                           copyEvent=copy['sequence'],shadowEvent=shadow[0]['sequence'],meanAbsChange=score,**draw_info))
        previous=rgb
    seconds=(u64(rows[-1]['qpc'])-u64(rows[0]['qpc']))/u64(manifest['qpcFrequency']) if rows else 0
    pre_seconds=(u64(rows[pre-1]['qpc'])-u64(rows[0]['qpc']))/u64(manifest['qpcFrequency'])
    if manifest['schema']==2:
        require(u64(manifest['preSpanTicks'])==u64(rows[pre-1]['qpc'])-u64(rows[0]['qpc']),'prefix duration proof')
        require(manifest['durationSatisfied'] is True and pre_seconds>=manifest['preMilliseconds']/1000,'requested history duration unavailable')
    return dict(historyWindowLinked=True,captureComplete=False,rootCauseReady=False,frames=linked,
                retainedSeconds=seconds,preTriggerSeconds=pre_seconds,note='Final color only; normal animation also changes pixels. Not a fissure verdict.')

def main():
    p=argparse.ArgumentParser();p.add_argument('manifest',type=Path);p.add_argument('cpu',type=Path)
    p.add_argument('--image-root',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--sheet',type=Path)
    a=p.parse_args();result=analyze_history(load(a.manifest),load(a.cpu),a.image_root)
    with a.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    if a.sheet:
        require(not a.sheet.exists(),'sheet exists')
        rows=result['frames'];indices=sorted(set([0,len(rows)//4,len(rows)//2,3*len(rows)//4,len(rows)-1]))
        sheet=Image.new('RGB',(640*len(indices),385),(20,20,20));d=ImageDraw.Draw(sheet)
        for x,index in enumerate(indices):
            row=rows[index]
            with Image.open(row['file']) as im:sheet.paste(im.convert('RGB').resize((640,360)),(x*640,25))
            d.text((x*640+8,6),'frame '+row['frame']+' / Present '+row['present'],fill='white')
        sheet.save(a.sheet)
    print(json.dumps({k:v for k,v in result.items() if k!='frames'},indent=2))
if __name__=='__main__':main()
