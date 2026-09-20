import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def read(path): return (ROOT/path).read_text(encoding='utf-8')
BRIDGE=read('src/d3d9/war3/model/war3_native_light_bridge.cpp')
CORE=read('src/d3d9/war3/model/war3_native_light_core.h')
JAPI=read('src/d3d9/war3/japi/war3_japi_v1.cpp')

class ModelLightApi(unittest.TestCase):
    def test_map_scoped_bounded_rules(self):
        for s in ('state.rules.size()>=64','state.policyGeneration==UINT64_MAX',
                  'state.catalogs.clear(); state.rules.clear();',
                  'sample.policyGeneration != state.policyGeneration',
                  'state.catalogs[catalog].path==path && state.catalogs[catalog].sha==sha'):
            self.assertIn(s,BRIDGE)
    def test_no_jass_gpu_or_model_io(self):
        block=BRIDGE.split('bool SetModelPath(',1)[1].split('uint64_t CurrentWorldSerial',1)[0]
        for s in ('LoadLibrary','SFile','CreateFile','AddPointLight','EmitCs','copyBuffer'):
            self.assertNotIn(s,block)
        self.assertIn('NormalizeModelLightPath(path,normalized)',block)
    def test_shadow_off_preserves_native(self):
        self.assertIn('rule->second.enabled && (!shadow || rule->second.shadows)',BRIDGE)
        self.assertIn('if (!Allowed(state,source,true)) continue;',BRIDGE)
        self.assertIn('if (!Allowed(state,source,true)) return false;',BRIDGE)
        self.assertIn('originalEgress(index, light)',BRIDGE)
        self.assertIn('originalEnable(index, enabled)',BRIDGE)
    def test_current_and_future_instances_share_exact_source(self):
        self.assertIn('instanceModel.sources=proof',BRIDGE)
        self.assertIn('model->second.generations[owner.index]!=owner.generation',BRIDGE)
        self.assertIn('light->second.generation==model->second.generations[i]',BRIDGE)
        self.assertIn('light==instanceModel.lights[j]',BRIDGE)
    def test_wire_and_editor_are_same_contract(self):
        jass=read('WarVK/jass/warvk_api.j'); action=read('WarVK/action.txt'); calls=read('WarVK/call.txt')
        self.assertIn('"modelPointLights.setEnabled", Carrier::Preloader, "sbb", kFeaturePointLight, false',JAPI)
        self.assertIn('"modelPointLights.count", Carrier::Hotkey, "s", kFeaturePointLight, false',JAPI)
        for n in ('WarVKSetModelPointLightsEnabled','WarVKGetModelPointLightCount','WarVKIsModelPointLightRegistered'):
            self.assertIn('function '+n+' takes',jass)
            self.assertIn('['+n+']',action+calls)
    def test_parser_admission_not_hash_bypass(self):
        for s in ('size > 4u*1024u*1024u','total>=16','ids[i]==id','interpolation>3',
                  'components=1; eligible=false;', 'out.lights[i].count=total'):
            self.assertIn(s,CORE)
        self.assertIn('const bool frozen=DecodeReviewedSource(',BRIDGE)
        self.assertIn('return source.policyIndex != UINT32_MAX;',BRIDGE)
    def test_candidate_defaults_have_explicit_off(self):
        self.assertIn('getEnvVar("DXVK_WAR3_NATIVE_MODEL_LIGHTS") != "1"',BRIDGE)
        self.assertIn('getEnvVar("DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER") == "1"',read('src/d3d9/d3d9_native_light.cpp'))

if __name__=='__main__': unittest.main()
