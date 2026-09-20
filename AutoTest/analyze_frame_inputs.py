"""Strict bounded input reader and CPU reconstruction, not GPU/pixel causality proof.

Consumes actual post-resolution bytes and the recorded draw push constants.
Floating point reconstruction is diagnostic (numpy float64), not bit-exact GPU replay.
"""
import argparse
import hashlib
from pathlib import Path
import numpy as np
import copy
from analyze_frame_evidence import load, require, u64, uint

WORDS=('stage handle rawcode layer positionStride positionOffset positionFormat indexType indexCount firstIndex '
       'vertexOffset vertexCount firstVertex minVertexIndex numVertices blendEnabled blendIndexed blendCount paletteIndex '
       'blendBinding blendStride blendWeightOffset blendWeightFormat blendIndexOffset blendIndexFormat uvBinding uvStride '
       'uvOffset uvFormat alphaTest alphaBlend lifecycle alphaComplete indexed topology resolved gpuSkinValid focus '
       'indexDomainKnown boundsProvenance').split()
SPAN_NAMES=('position','index','blend','uv','matrices','directSource','directPalette')
STATUSES=('absent','copied','missing-owner','missing-transfer-source-usage','range-invalid','capacity','unsupported','matrix-range')

def sha(data): return hashlib.sha256(data).hexdigest().upper()
def signed32(v): return v if v < 2**31 else v-2**32

def attribute(data, fmt, stride, offset, vertices):
    # Vulkan defaults: unprovided y/z=0, w=1. USCALED indices become float,
    # not normalized; preserve that distinction for indexed blending.
    formats={100:('<f4',1,1),103:('<f4',2,1),106:('<f4',3,1),109:('<f4',4,1),
             39:('u1',4,1),41:('u1',4,1),37:('u1',4,255),
             72:('<u2',1,1),74:('<u2',1,1),93:('<u2',4,1),95:('<u2',4,1)}
    require(fmt in formats,'unsupported vertex format '+str(fmt))
    dtype,count,scale=formats[fmt];size=np.dtype(dtype).itemsize*count
    require(stride>0 and offset+size<=stride,'attribute layout')
    require(len(vertices)>0 and int(vertices.min())>=0 and int(vertices.max())*stride+offset+size<=len(data),'attribute range')
    out=np.zeros((len(vertices),4));out[:,3]=1
    available=(len(data)-offset-size)//stride+1
    view=np.ndarray((available,count),dtype=dtype,buffer=data,offset=offset,
                    strides=(stride,np.dtype(dtype).itemsize))
    out[:,:count]=view[vertices]/scale
    require(np.isfinite(out).all(),'nonfinite vertex attribute')
    return out

def reconstruct(draw, spans, event):
    w=dict(zip(WORDS,draw['words']));pc=event['words32'];flags=pc[16]
    if draw['spans'][0].get('encoding',0)==1:
        count=w['indexCount'] if w['indexed'] else w['vertexCount']
        require(0<count<=1000000,'gather count')
        if w['indexed']:
            require(w['indexType'] in (0,1),'gather index type')
            dtype='<u2' if w['indexType']==0 else '<u4';size=np.dtype(dtype).itemsize
            start=w['firstIndex']*size
            require(start+count*size<=len(spans[1]),'gather raw index range')
            raw=np.frombuffer(spans[1],dtype=dtype,count=count,offset=start).astype(np.int64)
            actual=raw+signed32(w['vertexOffset'])
        else:
            raw=np.arange(w['firstVertex'],w['firstVertex']+count,dtype=np.int64);actual=raw
        require((actual>=0).all() and (actual<2**31).all(),'gather base-vertex range')
        normalized=copy.deepcopy(draw);decoded=list(spans)
        for role,stride in ((0,w['positionStride']),(2,w['blendStride']),(3,w['uvStride'])):
            if draw['spans'][role].get('encoding',0)!=1:continue
            require(stride and stride%4==0 and len(spans[role])==count*(16+stride),'gather byte extent')
            records=np.frombuffer(spans[role],dtype='<u4').reshape(count,4+stride//4)
            require(np.array_equal(records[:,0],raw) and np.array_equal(records[:,1],actual),'gather index witness mismatch')
            require((records[:,2]==1).all() and (records[:,3]==0).all(),'GPU gather rejected a source address')
            _,first,inverse=np.unique(actual,return_index=True,return_inverse=True)
            require(np.array_equal(records[:,4:],records[first[inverse],4:]),'repeated vertex changed inside gather')
            decoded[role]=records[:,4:].copy().tobytes();normalized['spans'][role]['encoding']=0
        decoded[1]=np.arange(count,dtype='<u4').tobytes()
        for idx,value in ((7,1),(8,count),(9,0),(10,0),(11,count),(12,0),(14,count),(33,1)):
            normalized['words'][idx]=value
        return reconstruct(normalized,decoded,event)
    if w['indexed']:
        require(w['indexType'] in (0,1),'unsupported index type')
        dtype='<u2' if w['indexType']==0 else '<u4';size=np.dtype(dtype).itemsize
        start=w['firstIndex']*size;count=w['indexCount']
        require(0<count<=1000000 and start+count*size<=len(spans[1]),'index range')
        raw=np.frombuffer(spans[1],dtype=dtype,count=count,offset=start).astype(np.int64)
        indices=raw+signed32(w['vertexOffset'])
    else:
        require(0<w['vertexCount']<=1000000,'vertex count')
        indices=np.arange(w['firstVertex'],w['firstVertex']+w['vertexCount'],dtype=np.int64)
    vertices,inverse=np.unique(indices,return_inverse=True)
    pos=attribute(spans[0],w['positionFormat'],w['positionStride'],w['positionOffset'],vertices)
    require(not flags&0x40,'direct GPU-skin format captured but reconstruction not yet supported')
    require(len(spans[4]) in (64,16384),'matrix extent')
    matrices=np.frombuffer(spans[4],dtype='<f4').astype(np.float64).reshape((-1,4,4))
    require(np.isfinite(matrices).all(),'nonfinite matrix')
    if flags&1:
        require(len(matrices)==256 and pc[19]==w['paletteIndex']*256,'palette offset binding')
        count=min(pc[20],3)
        stream=spans[2] if w['blendBinding']==1 else spans[0]
        stride=w['blendStride'] if w['blendBinding']==1 else w['positionStride']
        bone=attribute(stream,w['blendIndexFormat'],stride,w['blendIndexOffset'],vertices) if flags&2 else None
        weights=attribute(stream,w['blendWeightFormat'],stride,w['blendWeightOffset'],vertices) if count else None
        world=np.zeros_like(pos);remaining=np.ones(len(pos));max_bone=0
        for i in range(count+1):
            bi=np.rint(bone[:,i]).astype(np.int64) if bone is not None else np.full(len(pos),i)
            require((bi>=0).all() and (bi<=255).all(),'bone index outside proven palette')
            max_bone=max(max_bone,int(bi.max()))
            weight=remaining.copy() if i==count else weights[:,i]
            if i!=count:remaining-=weight
            world+=np.einsum('ni,nij->nj',pos,matrices[bi])*weight[:,None]
    else:
        require(len(matrices)==1,'static matrix extent');world=pos@matrices[0];max_bone=None
    require(np.isfinite(world).all(),'nonfinite world vertices')
    light=np.asarray(pc[:16],dtype='<u4').view('<f4').astype(np.float64).reshape((4,4))
    clip=world@light
    require(np.isfinite(clip).all(),'nonfinite clip vertices')
    return {'vertices':vertices,'indices':inverse,'world':world,'clip':clip,'maxBoneIndex':max_bone}

def analyze_inputs(manifest,binary,cpu):
    d=manifest
    require(type(d.get('schema')) is int and d['schema'] in (1,2) and d.get('rootCauseReady') is False,'input schema or completeness')
    require(d['session']==cpu['session'] and d['processNonce']==cpu['processNonce'],'input session/nonce')
    slots=uint(d['slots'],576)
    require(slots in (224,576) and (d['bytesPerSlot'],d['drawsPerSlot'])==(262144,128),'capacity contract')
    if 'maxSlots' in d:require(d['maxSlots']==576,'maximum slot contract')
    require(u64(d['binaryBytes'])==len(binary),'binary length')
    require(type(d['batches']) is list and len(d['batches'])<=slots,'batch capacity')
    events={}
    for e in cpu['events']:
        if e['kind']==18:
            b=e['words32'];batch=b[40]|(b[41]<<32)
            if batch:events.setdefault((batch,int(e['data'][0])),[]).append(e)
    rows=[];last=0;file_end=0;all_status={};identities=set()
    for batch in d['batches']:
        bid=u64(batch['id']);require(bid>last,'batch order');last=bid
        offset=u64(batch['fileOffset']);size=uint(batch['bytes'],262144)
        require(offset==file_end and size<=len(binary)-offset,'batch binary range');file_end+=size
        require(type(batch['gpuComplete']) is bool,'completion type')
        require(not batch['gpuComplete'] or 0<u64(batch['fenceValue'])<=u64(batch['observedValue']),'fence completion')
        require(batch['gpuComplete'] or size==0,'uncompleted bytes were exported')
        require(len(batch['draws'])<=128 and len(batch['draws'])+batch['omitted']==batch['eligible'],'draw accounting')
        for draw in batch['draws']:
            idx=uint(draw['index'],2**32-1);require((bid,idx) not in identities,'duplicate draw');identities.add((bid,idx))
            require(len(draw['words'])==40 and len(draw['provenance'])==16 and len(draw['worldMatrixBits'])==16,'record layout')
            for v in draw['words']+draw['worldMatrixBits']:uint(v,2**32-1)
            for v in draw['provenance']:u64(v)
            require(len(draw['spans'])==7,'span layout');spans=[];missing=[];hashes={}
            for name,s in zip(SPAN_NAMES,draw['spans']):
                status=uint(s['status'],7);all_status[STATUSES[status]]=all_status.get(STATUSES[status],0)+1
                n=u64(s['bytes']);p=uint(s['offset'],262144)
                if status==1 and batch['gpuComplete']:
                    require(0<n<=size and p<=size-n,'copied span range')
                    data=binary[offset+p:offset+p+n];hashes[name]=sha(data);spans.append(data)
                else:
                    spans.append(b'')
                    if status not in (0,1):missing.append(name+':'+STATUSES[status])
            match=events.get((bid,idx),[])
            row={'batch':str(bid),'frame':batch['frame'],'volume':batch['volume'],'draw':idx,
                 'part':draw['part'],'handle':draw['words'][1],'rawcode':draw['words'][2],'layer':draw['words'][3],
                 'stride':draw['words'][4],'indices':draw['words'][8],'vertices':draw['words'][14],
                 'producer':draw['provenance'][0],'cacheOutcome':draw['provenance'][2],
                 'focus':bool(draw['words'][37]),'hashes':hashes,'missing':missing,'actualDraws':len(match),'reconstructed':False}
            try:
                require(batch['gpuComplete'],'GPU input copy incomplete');require(match,'no actual draw event in retained CPU window')
                e=match[0];pc=e['words32']
                for e in match:
                    require(all(e[k]==batch[k] for k in ('owner','frame','mapEpoch','deviceEpoch')),'draw identity mismatch')
                    require(bool(e['words32'][34])==batch['volume'],'surface/volume mismatch')
                    for role,h,o,n in ((0,3,4,5),(1,6,7,8)):
                        if role==1 and not draw['words'][33]:continue
                        s=draw['spans'][role]
                        size_key='sourceBytes' if d['schema']>=2 else 'bytes'
                        require([u64(s[k]) for k in ('buffer','sourceOffset',size_key)]==[int(e['data'][i]) for i in (h,o,n)],'actual physical binding mismatch')
                    require(e['words32'][28]==draw['words'][4] and e['words32'][29]==draw['words'][5],'actual vertex layout mismatch')
                rebuilt=reconstruct(draw,spans,match[0]);world=rebuilt['world']
                row.update(reconstructed=True,worldMin=world[:,:3].min(axis=0).tolist(),worldMax=world[:,:3].max(axis=0).tolist(),
                           maxBoneIndex=rebuilt['maxBoneIndex'],worldSha256=sha(world.astype('<f8').tobytes()),flags=pc[16])
            except ValueError as exc:row['unavailableReason']=str(exc)
            rows.append(row)
    require(file_end==len(binary),'trailing binary bytes')
    focus=[r for r in rows if r['focus'] and r['actualDraws']]
    return {'schema':1,'manifestValid':True,'rootCauseReady':False,'binarySha256':sha(binary),
            'captureDrops':u64(d['captureDrops']),'batchCount':len(d['batches']),'spanStatuses':all_status,
            'omittedDraws':sum(b['omitted'] for b in d['batches']),
            'reconstructedDraws':sum(r['reconstructed'] for r in rows),'focusDraws':len(focus),
            'focusReconstructed':sum(r['reconstructed'] for r in focus),'draws':rows,
            'limitations':['No pixel draw-ID, alpha texture pixels or intermediate GPU attachments.',
                           'CPU reconstruction is numeric diagnostic, not bit-identical shader execution.',
                           'A matching signature is not proof of model identity; pointers require epoch/frame.']}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('manifest','binary','cpu','output'):p.add_argument('--'+name,required=True,type=Path)
    a=p.parse_args();result=analyze_inputs(load(a.manifest),a.binary.read_bytes(),load(a.cpu))
    import json
    with a.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2,allow_nan=False)
    print({k:v for k,v in result.items() if k!='draws'})
if __name__=='__main__':main()
