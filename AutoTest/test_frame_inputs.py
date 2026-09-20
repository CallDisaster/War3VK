import copy
from pathlib import Path
import struct
import unittest
import numpy as np
from analyze_frame_inputs import analyze_inputs, reconstruct, attribute
from package_frame_input_research import parse_frames,select_evidence

def fixture(skinned=False):
    points=struct.pack('<9f',0,0,0,1,0,0,0,1,0)
    indices=struct.pack('<3H',0,1,2)
    matrix=np.eye(4,dtype='<f4');matrix[3,:3]=[10,20,30]
    mats=matrix.tobytes()*(256 if skinned else 1)
    bone=bytes([0,0,0,0]*3)
    parts=[points,indices,bone if skinned else b'',b'',mats,b'',b'']
    binary=b'';spans=[]
    for i,data in enumerate(parts):
        offset=len(binary);binary+=data
        spans.append(dict(buffer=str(i+11),sourceOffset='0',bytes=str(len(data)),owner='1',allocation='0',generation='1',
                          offset=offset,status=1 if data else 0,usage=1))
    words=[0]*40
    for i,v in {0:11,1:1234,4:12,6:106,7:0,8:3,14:3,15:int(skinned),16:int(skinned),
                19:int(skinned),20:4,24:39,33:1,34:3,35:1,37:1}.items():words[i]=v
    draw=dict(index=1,words=words,provenance=['1']*16,worldMatrixBits=[0]*16,part='22',metadata='0',geometry='0',alphaFrame='1',boundsGeneration='0',spans=spans)
    batch=dict(id='1',owner='7',frame='10',mapEpoch='2',deviceEpoch='3',volume=False,gpuComplete=True,fenceValue='1',observedValue='1',
               fileOffset='0',bytes=len(binary),eligible=1,omitted=0,draws=[draw])
    root=dict(schema=1,rootCauseReady=False,session='1',processNonce='2',slots=576,bytesPerSlot=262144,drawsPerSlot=128,binaryBytes=str(len(binary)),
              captureDrops='0',batches=[batch])
    pc=np.eye(4,dtype='<f4').view('<u4').reshape(-1).tolist()+[0]*32
    pc[16]=3 if skinned else 0;pc[28]=12;pc[40]=1
    data=['0']*12;data[0]='1';data[3:6]=['11','0',str(len(points))];data[6:9]=['12','0',str(len(indices))]
    event=dict(kind=18,owner='7',frame='10',mapEpoch='2',deviceEpoch='3',words32=pc,data=data)
    cpu=dict(session='1',processNonce='2',events=[event])
    return root,binary,cpu

class Reconstruction(unittest.TestCase):
    def test_selection_is_explicit_and_preserves_records(self):
        d,b,c=fixture();original=copy.deepcopy((d,b,c))
        sc,si,sb=select_evidence(c,d,b,{'10'})
        self.assertEqual(sc['selectionSchema'],1);self.assertNotIn('schema',sc)
        self.assertEqual(si['batches'][0]['originalFileOffset'],'0')
        self.assertEqual(si['batches'][0]['draws'],d['batches'][0]['draws'])
        self.assertEqual(analyze_inputs(si,sb,sc)['reconstructedDraws'],1)
        self.assertEqual((d,b,c),original)
    def test_gather_roundtrip_and_rejected_address(self):
        d,b,c=fixture();draw=d['batches'][0]['draws'][0]
        gathered=b''.join(struct.pack('<4I',i,i,1,0)+b[i*12:i*12+12] for i in range(3))
        rebuilt=b''
        for role,s in enumerate(draw['spans']):
            old=b[s['offset']:s['offset']+int(s['bytes'])]
            s['sourceBytes']=s['bytes'];s['encoding']=1 if role==0 else 0
            value=gathered if role==0 else old
            s['offset']=len(rebuilt);s['bytes']=str(len(value));rebuilt+=value
        d['schema']=2;d['binaryBytes']=str(len(rebuilt));d['batches'][0]['bytes']=len(rebuilt)
        self.assertEqual(analyze_inputs(d,rebuilt,c)['draws'][0]['worldMin'],[10,20,30])
        bad=rebuilt[:8]+struct.pack('<I',0)+rebuilt[12:]
        result=analyze_inputs(d,bad,c);self.assertEqual(result['reconstructedDraws'],0)
        self.assertIn('GPU gather rejected',result['draws'][0]['unavailableReason'])
    def test_marked_ranges_zero_based(self):
        self.assertEqual(parse_frames('0,69-71,103-106',175),{0,69,70,71,103,104,105,106})
        for invalid in ('175','-1','5-2','1-3-4','NaN'):
            with self.assertRaises(ValueError):parse_frames(invalid,175)
    def test_static_world(self):
        a=analyze_inputs(*fixture());self.assertEqual(a['reconstructedDraws'],1)
        self.assertEqual(a['draws'][0]['worldMin'],[10,20,30]);self.assertFalse(a['rootCauseReady'])
    def test_only_frozen_full_or_high_pressure_slot_profiles(self):
        d,b,c=fixture();d.update(slots=224,maxSlots=576)
        self.assertEqual(analyze_inputs(d,b,c)['reconstructedDraws'],1)
        for value in (223,225,575):
            bad,binary,cpu=fixture();bad['slots']=value
            with self.assertRaises(ValueError):analyze_inputs(bad,binary,cpu)
        bad,binary,cpu=fixture();bad.update(slots=224,maxSlots=575)
        with self.assertRaises(ValueError):analyze_inputs(bad,binary,cpu)
    def test_zero_weight_indexed_palette_equivalent(self):
        a=analyze_inputs(*fixture());b=analyze_inputs(*fixture(True))
        self.assertEqual(a['draws'][0]['worldSha256'],b['draws'][0]['worldSha256'])
        self.assertEqual(b['draws'][0]['maxBoneIndex'],0)
    def test_binding_mismatch(self):
        d,b,c=fixture();c['events'][0]['data'][3]='123'
        r=analyze_inputs(d,b,c);self.assertEqual(r['reconstructedDraws'],0)
        self.assertIn('binding mismatch',r['draws'][0]['unavailableReason'])
    def test_session(self):
        d,b,c=fixture();c['session']='2'
        with self.assertRaises(ValueError):analyze_inputs(d,b,c)
    def test_trailing_binary(self):
        d,b,c=fixture()
        with self.assertRaises(ValueError):analyze_inputs(d,b+b'0',c)
    def test_inflight_not_readable(self):
        d,b,c=fixture();d['batches'][0]['observedValue']='0'
        with self.assertRaises(ValueError):analyze_inputs(d,b,c)
    def test_span_overrun(self):
        d,b,c=fixture();d['batches'][0]['draws'][0]['spans'][0]['offset']=len(b)
        with self.assertRaises(ValueError):analyze_inputs(d,b,c)
    def test_duplicate_draw(self):
        d,b,c=fixture();q=d['batches'][0];q['draws']*=2;q['eligible']=2
        with self.assertRaises(ValueError):analyze_inputs(d,b,c)
    def test_capacity_is_missing_not_zero_geometry(self):
        d,b,c=fixture();d['batches'][0]['draws'][0]['spans'][0]['status']=5
        a=analyze_inputs(d,b,c);self.assertEqual(a['reconstructedDraws'],0)
        self.assertIn('position:capacity',a['draws'][0]['missing'])
    def test_negative_base_vertex_is_validated(self):
        d,b,c=fixture();d['batches'][0]['draws'][0]['words'][10]=2**32-1
        self.assertEqual(analyze_inputs(d,b,c)['reconstructedDraws'],0)
    def test_nan_is_not_repaired(self):
        d,b,c=fixture();b=struct.pack('<f',float('nan'))+b[4:]
        self.assertEqual(analyze_inputs(d,b,c)['reconstructedDraws'],0)
    def test_scaled_is_not_normalized(self):
        self.assertEqual(attribute(bytes([5,0,0,0]),39,4,0,np.array([0]))[0,0],5)
        self.assertAlmostEqual(attribute(bytes([5,0,0,0]),37,4,0,np.array([0]))[0,0],5/255)
    def test_cascade_and_epoch(self):
        for key,value in [('mapEpoch','4'),('deviceEpoch','4'),('frame','11')]:
            d,b,c=fixture();c['events'][0][key]=value
            self.assertEqual(analyze_inputs(d,b,c)['reconstructedDraws'],0)

class SourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[1]
        cls.source=(cls.root/'src/d3d9/war3/tools/war3_frame_inputs.cpp').read_text(encoding='utf-8')
    def test_no_wait_or_hot_disk(self):
        hot=self.source.split('uint64_t InputCapture::capture',1)[1].split('json InputCapture::exportNew',1)[0]
        for token in ('WaitFor','waitIdle','waitFor','CreateFile','dump(','Logger::'):self.assertNotIn(token,hot)
        self.assertIn('std::try_to_lock',hot)
    def test_completion_ownership(self):
        for token in ('vkGetSemaphoreCounterValue','isInUse(DxvkAccess::Write)','ctx->track(pinned)',
                      'VK_BUFFER_USAGE_TRANSFER_SRC_BIT','VK_QUEUE_FAMILY_IGNORED','s.finalized','CREATE_NEW'):
            self.assertIn(token,self.source)
    def test_outside_actual_rendering_and_linked(self):
        shadow=(self.root/'src/d3d9/d3d9_war3_shadow.cpp').read_text(encoding='utf-8')
        body=shadow.split('uint64_t inputEvidenceBatch = 0;',1)[1]
        self.assertLess(body.index('m_inputEvidence->capture'),body.index('ctx->cmdBeginRendering'))
        self.assertIn('event.bits[40]=uint32_t(inputEvidenceBatch)',body)
    def test_release_default_off_resolved_profile_and_explicit_limits(self):
        gate=(self.root/'src/d3d9/war3/tools/war3_frame_evidence.cpp').read_text(encoding='utf-8')
        self.assertIn('return recorderConfiguration().inputs;',gate)
        self.assertIn('std::getenv("DXVK_WAR3_FRAME_EVIDENCE_INPUTS")',gate)
        options=(self.root/'meson_options.txt').read_text(encoding='utf-8')
        self.assertIn("option('warvk_internal_frame_recorder', type : 'boolean', value : false",options)
        core=(self.root/'src/d3d9/war3/tools/war3_frame_inputs_core.h').read_text()
        for token in ('Slots = 576','BytesPerSlot = 256 * 1024','DrawsPerSlot = 128'):self.assertIn(token,core)
    def test_streamed_export_no_second_full_tree(self):
        export=self.source.split('json InputCapture::exportNew',1)[1]
        self.assertIn('auto encoded=batch.dump()',export)
        self.assertNotIn('result["batches"].push_back',export)
        self.assertIn('s.finalized&&m->complete(s,observed)',export)
    def test_cpu_provider_is_backend_independent(self):
        cpu=(self.root/'src/d3d9/war3/tools/war3_frame_evidence.cpp').read_text(encoding='utf-8')
        self.assertNotIn('#include "war3_frame_inputs.h"',cpu)
        self.assertIn('inputExporter.load',cpu)
    def test_gather_no_transform_and_bounded_reads(self):
        shader=(self.root/'src/d3d9/shaders/war3_frame_input_gather.comp').read_text(encoding='utf-8')
        for token in ('ordinal>=p.count','vertex<p.sourceBytes/p.stride','p.outputData.words[outWord+2]=valid?1u:0u',
                      'p.source.words[vertex*(p.stride/4)+j]','0x7fffffffu-uint(p.vertexOffset)'):
            self.assertIn(token,shader)
        self.assertNotIn('mat4',shader);self.assertNotIn('float ',shader)
        self.assertIn('ctx->track(m->gatherPipeline)',self.source)
        self.assertIn('requiredIndexBytes>draw.indexInfo.size',self.source)
    def test_no_geometry_policy_read_of_provenance(self):
        s=(self.root/'src/d3d9/d3d9_war3_scene.h').read_text(encoding='utf-8')
        self.assertIn('Never consulted by production admission or replay',s)
        self.assertNotIn('inputEvidenceProvenance',s.split('inline uint64_t War3ShadowReplaySourceGeneration',1)[1])

if __name__=='__main__':unittest.main()
