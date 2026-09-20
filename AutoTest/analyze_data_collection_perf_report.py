"""Diagnostic-only comparison: retain every duplicate value, never last-key-wins.

Duplicate JSON is not accepted as a valid report contract. Unique fields may be
inspected for diagnosis; no product or performance acceptance is emitted.
"""
import argparse,json,hashlib,math,re
from pathlib import Path
class Pairs(list):pass
def norm(v,path='',dups=None):
 if dups is None: dups=[]
 if isinstance(v,Pairs):
  d={}
  for k,x in v:d.setdefault(k,[]).append(norm(x,path+'/'+k,dups))
  for k,vs in d.items():
   if len(vs)>1: dups.append({'path':path+'/'+k,'values':vs})
   d[k]=vs[0] if len(vs)==1 else {'duplicateValues':vs}
  return d
 if isinstance(v,list): return [norm(x,path+'/'+str(i),dups) for i,x in enumerate(v)]
 return v
def load(p):
 b=p.read_bytes();s=b.decode('utf-8-sig');m=list(re.finditer(r'\bconst\s+data\s*=\s*(?=\{)',s))
 if len(m)!=1: raise ValueError('expected unique root')
 raw,end=json.JSONDecoder(object_pairs_hook=Pairs,parse_constant=reject_constant).raw_decode(s,m[0].end())
 if not s[end:].lstrip().startswith(';'): raise ValueError('invalid root boundary')
 dups=[];return norm(raw,dups=dups),dups,hashlib.sha256(b).hexdigest().upper()
def reject_constant(value):
 raise ValueError('non-finite JSON: '+value)
def ticks(value):
 if not isinstance(value,str) or not re.fullmatch(r'[0-9]+',value): raise ValueError('invalid tick string')
 n=int(value)
 if n>2**64-1: raise ValueError('tick overflow')
 return n
def audit_tree(tree):
 if tree is None: return {'present':False}
 if tree.get('enabled') is not True: return {'present':True,'enabled':False}
 result={'present':True,'enabled':True,'coverageComplete':tree.get('coverageComplete'),
         'frames':tree.get('frames'),'samplePeriod':tree.get('samplePeriod'),'threads':[]}
 for t in tree.get('threads',[]):
  nodes=t['nodes']; by_id={n['id']:n for n in nodes}; paths={}; errors=[]
  if len(by_id)!=len(nodes) or not nodes or nodes[0]['id']!=0: raise ValueError('duplicate/missing node')
  for n in nodes:
   if n['id']==0: paths[0]='DataCollection'
   else:
    if n['parent'] not in paths or n['parent']>=n['id']: raise ValueError('invalid parent order')
    paths[n['id']]=paths[n['parent']]+'/'+n['name']
   direct=sum(ticks(c['ticks']) for c in nodes if c['id']!=0 and c['parent']==n['id'])
   if direct!=ticks(n['childTicks']) or ticks(n['ticks'])<direct: errors.append(n['id'])
  if ticks(nodes[0]['ticks'])!=ticks(nodes[0]['childTicks']): errors.append(0)
  root=nodes[0]['inclusiveMs']
  result['threads'].append({'threadId':t['threadId'],'mainThread':t['mainThread'],
    'nodeCount':len(nodes),'closureErrors':errors,'mathematicalClosureOnly':True,
    'reportedClosed':t['closed'],'faults':t['faults'],'inFlight':t['inFlight'],
    'sampledRootMs':root,'sampledRoots':nodes[0]['sampledCalls'],
    'topLevel':[{'path':paths[n['id']],'sampledMs':n['inclusiveMs'],
       'shareOfSampledDataRootPct':n['inclusiveMs']/root*100 if root else None,
       'unclassifiedMs':n['unclassifiedMs'],'samples':n['sampledCalls']}
       for n in sorted(nodes[1:],key=lambda n:n['inclusiveMs'],reverse=True) if n['parent']==0],
    'topSelf':[{'path':paths[n['id']],'unclassifiedMs':n['unclassifiedMs'],
       'inclusiveMs':n['inclusiveMs'],'samples':n['sampledCalls']}
       for n in sorted(nodes[1:],key=lambda n:n['unclassifiedMs'],reverse=True)[:8]]})
 return result
def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('reports',nargs='+');a=p.parse_args();out=[]
 for x in a.reports:
  d,dups,h=load(Path(x));b=d.get('shadowBudgetSummary',{});t=d.get('shadowRuntimeV2Summary',{});cs=d.get('workloadSeriesColumns',[]);rows=d.get('workloadSeries',[])
  if len(set(cs))!=len(cs) or len(rows)!=d['frameCount'] or any(len(r)!=len(cs) for r in rows): raise ValueError('invalid workload series')
  def mean(k):
   return sum(r[cs.index(k)] for r in rows)/len(rows) if k in cs and rows else None
  out.append({'path':x,'sha256':h,'strictJsonValid':not dups,'duplicates':dups,'meta':d.get('meta',{}),'performance':{k:d.get(k) for k in ['frameCount','windowSec','avgFps','avgFrameTimeMs','p50CpuMs','p95CpuMs','p99CpuMs','avgGpuTimeMs','avgMainThreadCpuMs','avgWorkerThreadsCpuMs','jank16','jank33','jank50','avgUncoveredFrameWallMs']},'workloadMean':{k:mean(k) for k in ['capturedDrawCount','shadowMetadataCaptureCalls','drawTimeVBCacheCaptureCount','drawTimeVBCacheSameFrameDedupMiss','drawTimeGenerationBackedPositionReuseCount','drawTimeGenerationBackedIndexReuseCount','dynamicSkinnedOutputCount','semanticSceneSubmittedSkinned','replayCasterCount','replayGeometryWork']},'integrity':{k:b.get(k) for k in ['framesObserved','framesIncomplete','framesProducerIncomplete','producerRequiredCasterOmissionCount','framesBudgetExceeded','shadowReceiverFrames','avgArenaMb','maxArenaMb']},'treeAudit':audit_tree(d.get('dataCollectionTree')),'tree':d.get('dataCollectionTree'),'gpuSkin':{k:d.get('gpuSkinSnapshot',{}).get(k) for k in ['mode','hooksEnabled']},'keyGauges':{k:t.get(k) for k in ['modelRegistryHit','modelRegistryMiss','modelReuseCount','poseCacheHit','poseCacheMiss','semanticSceneVisibleLookupPartLayerHitCount','semanticSceneVisibleLookupMissCount']}})
 if len(out)>1:
  a0=out[0]; a1=out[1]; f0=a0['performance'].get('avgFps');f1=a1['performance'].get('avgFps');
  out.append({'comparison':'first versus second input','fpsDelta':f0-f1 if f0 is not None and f1 is not None else None,'fpsPct':(f0/f1-1)*100 if f0 and f1 else None,'frameMsDelta':a0['performance'].get('avgFrameTimeMs')-a1['performance'].get('avgFrameTimeMs'),'warning':'Different DLL/config/path coverage or duplicate JSON prevents product acceptance.'})
 with a.output.open('x',encoding='utf-8') as f:json.dump({'schemaVersion':2,'productAccepted':False,'reports':out},f,ensure_ascii=False,indent=2,allow_nan=False)
 print(json.dumps({'reports':len(a.reports),'output':str(a.output),'productAccepted':False},ensure_ascii=False))
if __name__=='__main__':main()
