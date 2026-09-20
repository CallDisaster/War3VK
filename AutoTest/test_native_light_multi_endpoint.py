"""Two-light transport algebra plus concrete source boundaries, not runtime proof."""
import itertools
import random
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def read(p): return (ROOT / p).read_text(encoding='utf-8')
DEVICE = read('src/d3d9/d3d9_native_light.cpp')
FF = read('src/d3d9/d3d9_fixed_function.cpp')
SHADER = read('subprojects/war3fx/shaders/war3_shadow_receiver.frag')
MAIN = read('src/d3d9/d3d9_device.cpp')

def resolve(c, a, b):
    mix = lambda x,y,t: x*(1-t)+y*t
    return mix(mix(c[3], c[2], a), mix(c[1], c[0], a), b)

class MultiEndpoint(unittest.TestCase):
    def test_binary_corners(self):
        for a,b in itertools.product((0,1), repeat=2):
            self.assertEqual(resolve([.91,.43,.27,.12],a,b), [.91,.43,.27,.12][(1-a)+2*(1-b)])
    def test_convex_bounded_pcf(self):
        rng=random.Random(198)
        for _ in range(2000):
            c=[rng.random() for _ in range(4)]
            x=resolve(c,rng.random(),rng.random())
            self.assertGreaterEqual(x+1e-12,min(c)); self.assertLessEqual(x-1e-12,max(c))
    def test_saturation_requires_both_removed_endpoint(self):
        c=[1.,1.,1.,.1]
        self.assertEqual(resolve(c,0,0),.1)
        self.assertNotEqual(c[0]-(c[0]-c[1])-(c[0]-c[2]),.1)
    def test_one_light_reduction(self):
        for a in (0.,.2,.7,1.):
            self.assertAlmostEqual(resolve([.8,.1,.8,.1],a,1),.1*(1-a)+.8*a)
    def test_source_marker_subset_truth_table(self):
        for marker in (0,1,2):
            actual=[(marker & mask)==0 for mask in (1,2,3)]
            self.assertEqual(actual,{0:[True,True,True],1:[False,True,False],2:[True,False,False]}[marker])
        self.assertIn('opBitwiseAnd(m_uint32Type, markerBits, m_module.constu32(endpoint+1))',FF)
        self.assertIn('alternateDiffuse[endpoint]',FF)
        self.assertIn('alternateSpecular[endpoint]',FF)
    def test_three_private_mrt_preserve_primary(self):
        self.assertIn('attachments.color[i+1].view = m_nativeColor.alternate[i]',MAIN)
        self.assertIn('for (uint32_t i=1; i<4; ++i)',MAIN)
        self.assertIn('((writeMasks & 0xfu) << (4*i))',MAIN)
        self.assertIn('((alphaMasks & 1u) << i)',MAIN)
        self.assertIn('m_module.opStore(m_ps.out.COLOR, primary)',FF)
        self.assertIn('m_module.opStore(m_ps.alternateOutput[endpoint], alternate)',FF)
    def test_explicit_priority_before_distance(self):
        self.assertIn('(explicitRule && !explicitRules[j])',DEVICE)
        self.assertIn('(explicitRule == explicitRules[j] && score < scores[j])',DEVICE)
        self.assertIn('previous.serial == world - 1',DEVICE)
        self.assertIn('ClaimDrawLight(slot, m_state.lights[slot].value(), sample)',DEVICE)
        self.assertIn('ValidateLease(tx.lights.data(), tx.count, tx.worldSerial)',DEVICE)
    def test_tail_is_zero_mask_not_claim_expansion(self):
        self.assertIn('std::array<uint32_t, 2> mask = {};',DEVICE)
        self.assertIn('if (lighting && world) for',DEVICE)
        self.assertIn('if (!world && !m_nativeColor.active) return;',DEVICE)
        self.assertNotIn('War3AbortNativeColor("lit-draw-outside-world")',DEVICE)
        for p in ('UseProgrammableVS()', 'UseProgrammablePS()', 'application-mrt',
                  'm_textureSlotTracking.hazardRT', 'rgb-dependent-alpha',
                  'original != m_nativeColor.original', 'ValidateLease('):
            self.assertIn(p,DEVICE)
    def test_tail_common_source_blends_each_destination(self):
        c=[.9,.6,.7,.4]; alpha=.6; s=.2
        out=[s*alpha+d*(1-alpha) for d in c]
        for a,b in itertools.product((0.,.3,1.),repeat=2):
            self.assertAlmostEqual(resolve(out,a,b), s*alpha+resolve(c,a,b)*(1-alpha))
    def test_all_binary_endpoints_share_coverage_alpha(self):
        wrapper=FF.split('void D3D9FFShaderCompiler::compilePS()',1)[1].split('uint32_t D3D9FFShaderCompiler::compilePSColor',1)[0]
        self.assertEqual(wrapper.count('alphaTestPS();'),1)
        self.assertIn('alternate, primary, swizzle.size()',wrapper)
        self.assertIn('for (uint32_t endpoint=0; endpoint<3; ++endpoint)',wrapper)
    def test_sampler_slots_and_bounds(self):
        for binding,name in ((14,'s_nativeLightBaseline'),(15,'s_nativeLightWithoutB'),(16,'s_nativeLightWithoutBoth')):
            self.assertIn(f'binding = {binding}) uniform texture2D {name}',SHADER)
        self.assertIn('nativeCount <= pointShadow.u_lightCount-nativeSlot',SHADER)
        self.assertIn('if (nativeValid && i >= nativeSlot && i-nativeSlot < nativeCount) continue',SHADER)

if __name__ == '__main__': unittest.main()
