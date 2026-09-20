"""Strict schema-1..7 evidence reader. Cannot certify pixels or a graphics cause."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from PIL import Image

CAPABILITIES={'cpuBoundaryEvents','gpuCompletion','pixelEvidence','videoFrameMapping',
              'actualCasterInputs','exactModuleMapIdentity'}
ROOT_FIELDS={'schema','state','reason','session','processId','processNonce','qpcFrequency',
             'accepted','evicted','triggerSequence','producerLosses','capacity','postRemaining',
             'captureComplete','rootCauseReady','capabilities','events'}
EVENT_FIELDS={'sequence','session','parent','qpc','thread','kind','owner','frame','mapEpoch',
              'deviceEpoch','label','data'}
# ---- 冻结根格式的**显式**扩展版本登记表（2026-09-17 上级裁定 ⑦）-----------------------
# 导出仍写 schema 7，但根对象多出了 paletteObject 块。此前本读方对根字段做**精确相等**检查，
# 于是每个真实 palette 导出都被判 "root fields mismatch"，palette 读方只能"投影掉该块再委托"，
# 而 history/watcher 入口（analyze_frame_history / frame_history_watch）根本读不了这类导出。
# 现在把扩展**显式登记**在这里（本模块同时是 history/watcher 唯一引入的读方，所以它必须能
# 自己判定扩展的存在与版本，不能依赖扩展自己的读方被 import）：
#   * 块里带 version 字段 ⇒ 必须是登记在案的整数版本，且块的字段集必须与该版本一致；
#   * 块里没有 version 字段 ⇒ 只接受登记在案的**冻结形状**（旧的实机产物），按旧合同读取；
#   * 未知版本、未知形状、未登记的根字段一律 ValueError（显式拒绝，不是静默忽略）。
SCHEMA_VERSIONS=(1,2,3,4,5,6,7)
SCHEMA=7
PALETTE_OBJECT_EXTENSION='paletteObject'
PALETTE_OBJECT_LEGACY_VERSION=1      # 无 version 字段的冻结块 = 旧实机产物
PALETTE_OBJECT_SEGMENTED_VERSION=2   # 显式 version=2：窗口/Reset 分段 + 载明身份证明
# 2026-09-18 批次 3/更正：显式 version=3 = 在上面的基础上引入**正常观察链**（阶段 FirstSight(5)）。
# 读方必须**注册**它才能接受；此前只注册了 {1,2}，导致写方一旦发 v3，两个读方都会拒绝**整份**导出。
PALETTE_OBJECT_FIRST_SIGHT_VERSION=3
# 2026-09-18 阶段 C：**链型 + 统一终态**版本。
# 引入 `ObservationClosed=7`（Observation 链「观察已结算」，**不表示**阶段齐全），
# 并（后续）把链型上 wire；v1/v2/v3 仍按各自契约解析，**不得**追溯套用 v4 的含义。
PALETTE_OBJECT_CHAIN_TYPED_VERSION=4
PALETTE_OBJECT_VERSION_FIELD='version'
PALETTE_OBJECT_LEGACY_BLOCK_FIELDS=frozenset({'watchCount','counters'})
PALETTE_OBJECT_BLOCK_FIELDS={PALETTE_OBJECT_LEGACY_VERSION:
                                 frozenset({'version','watchCount','counters'}),
                             PALETTE_OBJECT_SEGMENTED_VERSION:
                                 frozenset({'version','watchCount','counters'}),
                             PALETTE_OBJECT_FIRST_SIGHT_VERSION:
                                 frozenset({'version','watchCount','counters'}),
                            # v4 与 v3 同字段集（链型**不在**头块字段里，而在记录的 `data[4]`；
                            # 链型与尝试序号等字段**已随写方落地**，不得先登记后落空）。
                             PALETTE_OBJECT_CHAIN_TYPED_VERSION:
                                 frozenset({'version','watchCount','counters'})}
FROZEN_EXTENSIONS={PALETTE_OBJECT_EXTENSION:tuple(sorted(PALETTE_OBJECT_BLOCK_FIELDS))}
def require(ok,message):
    if not ok: raise ValueError(message)

def extension_version(name,block):
    """已登记扩展块的**显式**版本；未知版本/未知形状 ⇒ ValueError。"""
    require(name in FROZEN_EXTENSIONS,'unregistered root extension: '+str(name))
    require(type(block) is dict,'%s extension block must be an object'%name)
    if name==PALETTE_OBJECT_EXTENSION:
        if PALETTE_OBJECT_VERSION_FIELD not in block:
            require(frozenset(block)==PALETTE_OBJECT_LEGACY_BLOCK_FIELDS,
                    'unknown %s extension version: unversioned block fields %s'
                    %(name,sorted(block)))
            return PALETTE_OBJECT_LEGACY_VERSION
        declared=block[PALETTE_OBJECT_VERSION_FIELD]
        require(type(declared) is int and not isinstance(declared,bool),
                '%s.%s must be an integer extension version'
                %(name,PALETTE_OBJECT_VERSION_FIELD))
        require(declared in PALETTE_OBJECT_BLOCK_FIELDS,
                'unknown %s extension version %r (registered versions: %s)'
                %(name,declared,sorted(PALETTE_OBJECT_BLOCK_FIELDS)))
        expected=PALETTE_OBJECT_BLOCK_FIELDS[declared]
        require(frozenset(block)==expected,
                '%s extension version %d has unknown fields: %s'
                %(name,declared,sorted(frozenset(block)^expected)))
        return declared
    raise ValueError('unregistered root extension: '+str(name))

def root_extensions(d):
    """根对象里出现的已登记扩展 -> 版本（未知版本 ⇒ ValueError）。"""
    require(type(d) is dict,'root must be object')
    return {name:extension_version(name,d[name])
            for name in sorted(set(d)&set(FROZEN_EXTENSIONS))}

def strict_pairs(pairs):
    out={}
    for k,v in pairs:
        require(k not in out,'duplicate key: '+k)
        out[k]=v
    return out

def u64(v):
    require(type(v) is str and re.fullmatch(r'0|[1-9][0-9]{0,19}',v),'noncanonical uint64')
    n=int(v); require(n<2**64,'uint64 overflow'); return n

def uint(v,maximum):
    require(type(v) is int and 0<=v<=maximum,'invalid integer'); return v

def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'),object_pairs_hook=strict_pairs,
                      parse_constant=lambda x:require(False,'nonfinite JSON: '+x))

def analyze(d):
    require(type(d) is dict,'root must be object')
    require(type(d.get('schema')) is int and d['schema'] in SCHEMA_VERSIONS,'unsupported schema')
    allowed=(ROOT_FIELDS|({'reserved'} if d['schema']>=5 else set())|
             ({'effectiveConfiguration'} if d['schema']==7 else set()))
    # 已登记的扩展块只允许出现在冻结 schema 上；未登记的根字段仍是硬错误。
    extension_fields=set(d)&set(FROZEN_EXTENSIONS) if d['schema']==SCHEMA else set()
    require(set(d)==allowed|extension_fields,
            'root fields mismatch: %s'%sorted(set(d)^(allowed|extension_fields)))
    extensions=root_extensions(d)
    if d['schema']==7:
        config=d['effectiveConfiguration']
        require(type(config) is dict and set(config)=={'frameEvidence','rawInputs','skinPaletteContract','localRecorderOwner','paletteObjectEvidence'},
                'effective configuration fields')
        require(all(type(value) is bool for value in config.values()) and config['frameEvidence'] is True,
                'effective configuration values')
    words='float32Bits' if d['schema']==1 else 'words32'
    require(type(d['state']) is int and d['state']==3,'export not frozen')
    uint(d['reason'],4); require(d['reason']!=0,'missing freeze reason')
    require(d['captureComplete'] is False and d['rootCauseReady'] is False,'unsupported completeness claim')
    require(type(d['capabilities']) is dict and set(d['capabilities'])==CAPABILITIES,'capability fields')
    require(d['capabilities']['cpuBoundaryEvents'] is True,'missing CPU provider')
    for k,v in d['capabilities'].items():
        require(type(v) is bool and (k=='cpuBoundaryEvents' or v is False),'unimplemented capability claim')
    session=u64(d['session']); require(session>0,'session zero')
    require(u64(d['processNonce'])>0 and u64(d['qpcFrequency'])>0,'clock/session identity missing')
    require(uint(d['processId'],2**32-1)>0,'pid missing')
    cap=uint(d['capacity'],262144 if d['schema']>=6 else (131072 if d['schema']>=4 else 32768)); require(cap>=256,'capacity too small')
    uint(d['postRemaining'],120)
    accepted=u64(d['accepted']); evicted=u64(d['evicted']); trigger=u64(d['triggerSequence'])
    reserved=u64(d['reserved']) if d['schema']>=5 else accepted
    require(reserved>=accepted,'reservation accounting')
    losses=u64(d['producerLosses']); require(trigger<=reserved,'invalid trigger sequence')
    require(losses>=reserved-accepted,'unreported ticket loss')
    require(type(d['events']) is list and len(d['events'])<=cap,'event capacity')
    require(accepted==evicted+len(d['events']),'retention accounting')
    missing=['gpuCompletion','pixelEvidence','videoFrameMapping','actualCasterInputs','exactModuleMapIdentity']
    if losses: missing.append('producerContentionOrException')
    if d['reason']==3: missing.append('postWindowCapacityExceeded')
    if d['postRemaining']: missing.append('postWindowNotComplete')
    spans={}; unmatched=[]; durations=[]; lastq=0; caster_inputs={};lastseq=0;thread_clocks={}
    key=lambda e:tuple(e[k] for k in ('owner','frame','mapEpoch','deviceEpoch','thread','label'))
    end_to_begin={2:1,4:3,6:5}
    for i,e in enumerate(d['events'],evicted+1):
        require(type(e) is dict and set(e)==EVENT_FIELDS|{words},'event fields mismatch')
        sequence=u64(e['sequence'])
        if d['schema']>=5:require(lastseq<sequence<=reserved,'invalid reservation sequence')
        else:require(sequence==i,'sequence discontinuity')
        lastseq=sequence;i=sequence
        require(u64(e['session'])==session,'mixed session')
        q=u64(e['qpc'])
        if d['schema']>=5:
            thread=uint(e['thread'],2**32-1)
            require(q>=thread_clocks.get(thread,0),'thread QPC regression');thread_clocks[thread]=q
        else:require(q>=lastq,'QPC regression')
        lastq=q
        for k in ('owner','frame','mapEpoch','deviceEpoch'):u64(e[k])
        parent=u64(e['parent']); require(parent<i or parent==0,'invalid parent sequence')
        uint(e['thread'],2**32-1);kind=uint(e['kind'],{1:8,2:15,3:17,4:18,5:18,6:18,7:18}[d['schema']]);require(kind>0,'invalid kind')
        require(type(e['label']) is str and len(e['label'].encode('utf-8'))<=31 and '\0' not in e['label'],'label bounds')
        require(type(e['data']) is list and len(e['data'])==12,'payload length')
        data=[u64(x) for x in e['data']]
        require(type(e[words]) is list and len(e[words])==48,'word payload length')
        for v in e[words]:uint(v,2**32-1)
        if kind in (1,2,3,4,5,6) and data[11]:missing.append('truncatedLabel')
        if kind in (1,3,5):
            spans[i]=e
            if kind==5 and (parent not in spans or spans[parent]['kind']!=3):unmatched.append(i)
        elif kind in end_to_begin:
            begin=spans.pop(parent,None)
            if begin is None:unmatched.append(i);continue
            require(begin['kind']==end_to_begin[kind] and key(begin)==key(e),'span identity mismatch')
            durations.append({'begin':parent,'end':i,'kind':kind,'label':e['label'],
                              'cpuMs':(q-u64(begin['qpc']))*1000/u64(d['qpcFrequency'])})
        elif kind==7 and (parent not in spans or spans[parent]['kind']!=3):unmatched.append(i)
        elif kind==9:
            caster_inputs[i]=e
            if parent not in spans or spans[parent]['kind']!=3:unmatched.append(i)
        elif kind==10:
            source=caster_inputs.pop(parent,None)
            if source is None:missing.append('missingCasterInput')
            else:
                require(all(source[k]==e[k] for k in ('owner','frame','mapEpoch','deviceEpoch','thread')),
                        'caster binding identity')
                require(source['data'][0]==e['data'][0],'caster binding index')
        elif kind==11 and data[0]:missing.append('casterMetadataOmitted')
    unmatched+=list(spans)
    if caster_inputs:missing.append('missingCasterBinding')
    if unmatched:missing.append('unmatchedCpuSpans')
    return {'schemaValid':True,'captureComplete':False,'rootCauseReady':False,
            'schema':d['schema'],'extensions':extensions,
            'eventCount':len(d['events']),'session':d['session'],'missing':sorted(set(missing)),
            'unmatchedSpanEvents':unmatched,'cpuRecordedSpans':durations,
            'note':'CPU method/command-recording intervals, not GPU execution durations.'}

def link_screenshots(d, paths):
    """Optional canary link. Does not certify continuous pixels or replay inputs."""
    analyze(d)
    result=[]
    for path in paths:
        path=Path(path)
        match=re.fullmatch(r'WC3ScrnShot_\d{8}_\d{6}_\d{3}_(\d+)_(\d+)_p(\d+)\.tga',path.name)
        require(match is not None,'screenshot filename identity')
        pid,serial,ordinal=map(int,match.groups());require(pid==d['processId'],'screenshot PID mismatch')
        selected={}
        for kind in (13,14,15):
            rows=[e for e in d['events'] if e['kind']==kind and
                  u64(e['data'][0])==serial and u64(e['data'][1])==ordinal]
            require(len(rows)==1,'missing/duplicate screenshot event '+str(kind))
            selected[kind]=rows[0]
        prepared,saved,copied=selected[13],selected[14],selected[15]
        require(all(e['owner']==prepared['owner'] and e['data'][:5]==prepared['data'][:5]
                    for e in (saved,copied)),'screenshot resource/value mismatch')
        require(u64(prepared['sequence'])<u64(copied['sequence'])<u64(saved['sequence']),
                'screenshot event ordering')
        ends=[e for e in d['events'] if e['kind']==2 and e['owner']==copied['owner'] and
              u64(e['data'][2])==ordinal]
        require(len(ends)==1,'Present ordinal mapping missing/ambiguous')
        end=ends[0]
        begins=[e for e in d['events'] if e['kind']==1 and e['sequence']==end['parent']]
        require(len(begins)==1,'Present begin evicted/missing')
        require(u64(begins[0]['sequence'])<u64(prepared['sequence'])<u64(end['sequence']),
                'screenshot request outside Present scope')
        require(u64(end['data'][0])==1,'Present did not complete ordinary path')
        pipeline=[e for e in d['events'] if e['kind']==3 and all(e[k]==copied[k]
                  for k in ('owner','frame','mapEpoch','deviceEpoch','thread')) and
                  u64(e['sequence'])<u64(copied['sequence'])]
        require(len(pipeline)==1,'pipeline frame mapping missing/ambiguous')
        pipeline_ends=[e for e in d['events'] if e['kind']==4 and e['parent']==pipeline[0]['sequence']]
        require(len(pipeline_ends)==1 and u64(pipeline_ends[0]['sequence'])<u64(copied['sequence']),
                'pipeline completion record missing')
        with Image.open(path) as im:
            im.load();require(im.size==(u64(prepared['data'][3]),u64(prepared['data'][4])),'image extent mismatch')
        result.append(dict(file=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest().upper(),
                           requestId=str(serial),presentOrdinal=str(ordinal),pipelineFrame=copied['frame'],
                           mapEpoch=copied['mapEpoch'],deviceEpoch=copied['deviceEpoch'],
                           pipelineBegin=pipeline[0]['sequence'],copyEvent=copied['sequence'],savedEvent=saved['sequence'],
                           imageLinkVerified=True,continuousCoverage=False))
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--screenshots',type=Path,nargs='*',default=[])
    args=p.parse_args();data=load(args.input);result=analyze(data)
    if args.screenshots: result['linkedScreenshots']=link_screenshots(data,args.screenshots)
    result['sourceSha256']=hashlib.sha256(args.input.read_bytes()).hexdigest().upper()
    if args.output:
        with args.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k!='cpuRecordedSpans'},indent=2))
if __name__=='__main__':main()
