import copy
import json
import tempfile
import unittest
from pathlib import Path
from analyze_frame_evidence import analyze, load, CAPABILITIES, u64, link_screenshots
from PIL import Image

def event(seq,kind,parent=0,label='test'):
    return dict(sequence=str(seq),session='1',parent=str(parent),qpc=str(seq*100),
                thread=7,kind=kind,owner='55',frame='9007199254740993',mapEpoch='3',deviceEpoch='2',
                label=label,data=['0']*12,words32=[0]*48)

def fixture():
    return dict(schema=2,state=3,reason=2,session='1',processId=123,processNonce='999',
                qpcFrequency='1000000',accepted='4',evicted='0',triggerSequence='2',
                producerLosses='0',capacity=256,postRemaining=0,captureComplete=False,rootCauseReady=False,
                capabilities={k:k=='cpuBoundaryEvents' for k in CAPABILITIES},
                events=[event(1,3),event(2,5,1),event(3,6,2),event(4,4,1)])

class ReaderTests(unittest.TestCase):
    def configured(self):
        d=fixture();d.update(schema=7,reserved='4',effectiveConfiguration=dict(frameEvidence=True,rawInputs=True,
            skinPaletteContract=True,localRecorderOwner=True,paletteObjectEvidence=False));return d
    def test_schema7_actual_config(self):self.assertTrue(analyze(self.configured())['schemaValid'])
    def test_schema7_does_not_retrofit_old_schema(self):
        d=self.configured();d['schema']=6
        with self.assertRaises(ValueError):analyze(d)
    def test_schema7_requires_config(self):
        d=self.configured();del d['effectiveConfiguration']
        with self.assertRaises(ValueError):analyze(d)
    def test_schema7_exact_config_fields(self):
        d=self.configured();d['effectiveConfiguration']['invented']=True
        with self.assertRaises(ValueError):analyze(d)
    def test_schema7_bool_not_integer(self):
        for key in self.configured()['effectiveConfiguration']:
            d=self.configured();d['effectiveConfiguration'][key]=1
            with self.assertRaises(ValueError):analyze(d)
    def test_schema7_frame_gate_required(self):
        d=self.configured();d['effectiveConfiguration']['frameEvidence']=False
        with self.assertRaises(ValueError):analyze(d)
    def test_schema7_external_mode_valid(self):
        d=self.configured();d['effectiveConfiguration'].update(rawInputs=False,skinPaletteContract=False,localRecorderOwner=False)
        self.assertTrue(analyze(d)['schemaValid'])
    def test_valid_remains_incomplete(self):
        r=analyze(fixture());self.assertTrue(r['schemaValid']);self.assertFalse(r['rootCauseReady'])
        self.assertFalse(r['captureComplete']);self.assertEqual(len(r['cpuRecordedSpans']),2)
        self.assertEqual(r['unmatchedSpanEvents'],[])
    def test_uint64_exact(self):self.assertEqual(u64('9007199254740993'),9007199254740993)
    def test_uint64_rejects(self):
        for value in (1,True,'01','-1','1.0','18446744073709551616'):
            with self.subTest(value=value),self.assertRaises(ValueError):u64(value)
    def test_root_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'bad.json';p.write_text('{"schema":1,"schema":1}')
            with self.assertRaises(ValueError):load(p)
    def test_nested_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'bad.json';p.write_text('{"x":{"a":1,"a":2}}')
            with self.assertRaises(ValueError):load(p)
    def test_nonfinite(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'bad.json';p.write_text('{"x":NaN}')
            with self.assertRaises(ValueError):load(p)
    def test_root_fields(self):
        d=fixture();d['invented']=1
        with self.assertRaises(ValueError):analyze(d)
    def test_schema_bool(self):
        d=fixture();d['schema']=True
        with self.assertRaises(ValueError):analyze(d)
    def test_not_frozen(self):
        d=fixture();d['state']=1
        with self.assertRaises(ValueError):analyze(d)
    def test_false_acceptance(self):
        for key in ('captureComplete','rootCauseReady'):
            d=fixture();d[key]=True
            with self.assertRaises(ValueError):analyze(d)
    def test_forged_pixels(self):
        d=fixture();d['capabilities']['pixelEvidence']=True
        with self.assertRaises(ValueError):analyze(d)
    def test_sequence_gap(self):
        d=fixture();d['events'][1]['sequence']='99'
        with self.assertRaises(ValueError):analyze(d)
    def test_mixed_session(self):
        d=fixture();d['events'][2]['session']='2'
        with self.assertRaises(ValueError):analyze(d)
    def test_retention_mismatch(self):
        d=fixture();d['evicted']='1'
        with self.assertRaises(ValueError):analyze(d)
    def test_span_wrong_frame(self):
        d=fixture();d['events'][2]['frame']='3'
        with self.assertRaises(ValueError):analyze(d)
    def test_span_wrong_kind(self):
        d=fixture();d['events'][2]['kind']=4
        with self.assertRaises(ValueError):analyze(d)
    def test_qpc_regression(self):
        d=fixture();d['events'][2]['qpc']='0'
        with self.assertRaises(ValueError):analyze(d)
    def test_matrix_extent(self):
        d=fixture();d['events'][0]['words32'].pop()
        with self.assertRaises(ValueError):analyze(d)
    def test_legacy_schema_1_cpu_only(self):
        d=fixture();d['schema']=1
        for e in d['events']:e['float32Bits']=e.pop('words32')
        self.assertTrue(analyze(d)['schemaValid'])
    def test_schema_1_rejects_new_kind(self):
        d=fixture();d['schema']=1
        for e in d['events']:e['float32Bits']=e.pop('words32')
        d['events'][0]['kind']=9
        with self.assertRaises(ValueError):analyze(d)
    def test_schema_5_reservations(self):
        d=fixture();d.update(schema=5,reserved='4')
        self.assertTrue(analyze(d)['schemaValid'])
    def test_schema_5_cross_thread_qpc_not_gpu_order(self):
        d=fixture();d.update(schema=5,reserved='4')
        d['events'][1]['thread']=8;d['events'][2]['thread']=8
        d['events'][1]['qpc']='50';d['events'][2]['qpc']='60'
        self.assertTrue(analyze(d)['schemaValid'])
    def test_schema_5_thread_clock_still_monotonic(self):
        d=fixture();d.update(schema=5,reserved='4');d['events'][2]['qpc']='0'
        with self.assertRaises(ValueError):analyze(d)
    def test_schema_5_requires_loss_accounting(self):
        d=fixture();d.update(schema=5,reserved='5')
        with self.assertRaises(ValueError):analyze(d)
    def test_schema_5_rejects_duplicate_reservation(self):
        d=fixture();d.update(schema=5,reserved='4');d['events'][2]['sequence']='2'
        with self.assertRaises(ValueError):analyze(d)
    def test_evicted_span_explicit(self):
        d=fixture();d['evicted']='1';d['events'].pop(0)
        r=analyze(d);self.assertIn('unmatchedCpuSpans',r['missing'])
    def test_capacity_freeze_explicit(self):
        d=fixture();d['reason']=3;d['postRemaining']=5
        r=analyze(d);self.assertIn('postWindowCapacityExceeded',r['missing'])
        self.assertIn('postWindowNotComplete',r['missing'])
    def test_loss_explicit(self):
        d=fixture();d['producerLosses']='8'
        self.assertIn('producerContentionOrException',analyze(d)['missing'])
    def test_label_overrun(self):
        d=fixture();d['events'][0]['label']='a'*32
        with self.assertRaises(ValueError):analyze(d)
    def test_label_truncated(self):
        d=fixture();d['events'][0]['data'][11]='1'
        self.assertIn('truncatedLabel',analyze(d)['missing'])

class ExtensionVersionTests(unittest.TestCase):
    """2026-09-17 上级裁定 ⑦：根对象扩展必须**显式**版本化。

    通用读方对根字段做精确相等检查；paletteObject 块是登记在案的扩展，必须由本读方自己
    判定版本（history/watcher 入口只引入本读方）。旧产物（无 version 的冻结形状）仍可读，
    未知版本 / 未知形状必须显式拒绝。
    """
    _MISSING=object()
    def configured(self):
        d=fixture();d.update(schema=7,reserved='4',
            effectiveConfiguration=dict(frameEvidence=True,rawInputs=True,
                skinPaletteContract=True,localRecorderOwner=True,
                paletteObjectEvidence=False));return d
    def palette_block(self, version=_MISSING, **extra):
        block=dict(watchCount=0,counters={"emitted":"0"})
        if version is not self._MISSING:block["version"]=version
        block.update(extra)
        return block
    def with_palette(self, **kwargs):
        d=self.configured();d["paletteObject"]=self.palette_block(**kwargs);return d
    def test_registered_extension_is_accepted_and_versioned(self):
        r=analyze(self.with_palette())
        self.assertTrue(r["schemaValid"])
        self.assertEqual(r["extensions"],{"paletteObject":1})
        self.assertEqual(r["schema"],7)
    def test_explicit_version_two_is_accepted(self):
        self.assertEqual(analyze(self.with_palette(version=2))["extensions"],
                         {"paletteObject":2})
    def test_unknown_extension_version_is_rejected(self):
        # 2026-09-18 阶段 C 更正：这里原先把 **3** 列为"未知版本"，但 3 早已登记
        # （v3 = 分段 + 正常观察链）⇒ 本用例一直在**假失败**（它不在 259 个
        # `test_*_static.py` 门禁内，所以没被发现）。现在 3 与 4 都是已登记版本，
        # 本用例改用**真正未登记**的 5。**要求未变**：未登记版本必须整份拒绝。
        for version in (0,5,7,99,-1,"1",2.0,True,None):
            with self.subTest(version=version):
                with self.assertRaises(ValueError):
                    analyze(self.with_palette(version=version))
    def test_unknown_extension_shape_is_rejected(self):
        with self.assertRaises(ValueError):analyze(self.with_palette(invented=1))
        with self.assertRaises(ValueError):
            analyze(self.with_palette(version=2,invented=1))
        with self.assertRaises(ValueError):
            analyze(self.with_palette(version=2,**{"counters2":{}}))
    def test_extension_must_be_an_object(self):
        for value in (0,"x",[],None):
            with self.subTest(value=value):
                d=self.configured();d["paletteObject"]=value
                with self.assertRaises(ValueError):analyze(d)
    def test_old_artifacts_without_the_extension_stay_readable(self):
        r=analyze(self.configured())
        self.assertTrue(r["schemaValid"])
        self.assertEqual(r["extensions"],{})
    def test_unregistered_root_fields_are_still_rejected(self):
        d=self.with_palette();d["paletteObjectV2"]={"watchCount":0}
        with self.assertRaises(ValueError):analyze(d)
        d=self.configured();d["invented"]=1
        with self.assertRaises(ValueError):analyze(d)
    def test_extensions_are_only_allowed_on_the_frozen_schema(self):
        d=self.with_palette();d["schema"]=6
        d["reserved"]="4"
        del d["effectiveConfiguration"]
        with self.assertRaises(ValueError):analyze(d)


class ScreenshotCanaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.path=Path(self.tmp.name)/'WC3ScrnShot_20260915_000000_123_123_12_p99.tga'
        Image.new('RGBA',(2,2),(128,80,40,255)).save(self.path)
        self.d=fixture()
        self.d['accepted']='9';self.d['triggerSequence']='8'
        events=[event(1,3),event(2,5,1),event(3,6,2),event(4,4,1),event(5,1),
                event(6,13),event(7,15),event(8,2,5),event(9,14)]
        for e in events[:4]+[events[6]]:e['thread']=8;e['frame']='100'
        for e in (events[4],events[7]):e['frame']='99'
        for e in (events[5],events[8]):e['frame']='0';e['mapEpoch']='0';e['deviceEpoch']='0'
        for i in (5,6,8):events[i]['data'][:5]=['12','99','4','2','2']
        events[8]['thread']=9
        events[7]['data'][0]='1';events[7]['data'][2]='99'
        self.d['events']=events
    def tearDown(self):self.tmp.cleanup()
    def test_exact_link_not_inferred_offset(self):
        result=link_screenshots(self.d,[self.path])[0]
        self.assertTrue(result['imageLinkVerified']);self.assertEqual(result['pipelineFrame'],'100')
        self.assertEqual(result['presentOrdinal'],'99');self.assertFalse(result['continuousCoverage'])
    def test_wrong_pid(self):
        self.d['processId']=555
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_fence_value(self):
        self.d['events'][8]['data'][2]='5'
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_dimensions(self):
        Image.new('RGB',(3,3)).save(self.path)
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_pipeline_epoch(self):
        self.d['events'][6]['mapEpoch']='4'
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_copy_order(self):
        self.d['events'][6]['kind']=14;self.d['events'][8]['kind']=15
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_copy_missing(self):
        self.d['events'][6]['kind']=8
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_present_ordinal(self):
        self.d['events'][7]['data'][2]='100'
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_wrong_present_path(self):
        self.d['events'][7]['data'][0]='0'
        with self.assertRaises(ValueError):link_screenshots(self.d,[self.path])
    def test_numeric_payload_not_truncation(self):
        e=event(10,12);e['data'][11]='99';self.d['events'].append(e);self.d['accepted']='10'
        self.assertNotIn('truncatedLabel',analyze(self.d)['missing'])

if __name__=='__main__':unittest.main()
