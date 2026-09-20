import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT/'src/d3d9/war3/model/war3_native_light_bridge.cpp').read_text(encoding='utf-8')
CORE = (ROOT/'src/d3d9/war3/model/war3_native_light_core.h').read_text(encoding='utf-8')

class NativeLightBridge(unittest.TestCase):
    def test_load_is_exact_nested_transaction(self):
        for s in ('loadStack[loadDepth-1]', 'scope.document == uintptr_t(document)',
                  'buffer == scope.buffer', 'scope.bytes == size',
                  'SafeCopy(owned.data()', 'Sha256(owned.data()',
                  'result && proof.parsedOk', 'state.documents.erase(uintptr_t(document))'):
            self.assertIn(s, CPP)
    def test_independent_node_mapping(self):
        for s in ('count != proof.sources.lights[0].count', 'id!=source.objectId',
                  'array+4*source.ordinal', 'array+4*proof.lights[i].ordinal',
                  'light==from->second.lights[i]', 'state.generation.next()',
                  'parsedArray > UINT32_MAX-376u*count'):
            self.assertIn(s, CPP)
    def test_cache_is_generation_bound(self):
        self.assertIn('owner->second.generation != slots[i].generation', CPP)
        self.assertIn('state.lights.erase(uintptr_t(light))', CPP)
        self.assertIn('state.documents.clear(); state.models.clear(); state.lights.clear()', CPP)
        self.assertIn('slots[index].enabled = enabled != 0', CPP)
    def test_closed_recording_stage(self):
        self.assertIn('originalEgress(index, light);', CPP)
        self.assertNotIn('AddPointLight(', CPP)
        self.assertIn('lightingAuthorized=0 shadowAuthorized=0', CPP)
    def test_bounds_and_wrap(self):
        for s in ('state.models.size() >= 4096', 'state.lights.size()+proof.count > 4096',
                  'state.documents.size() < 128', 'count <= 16',
                  'frame.count == frame.lights.size()'):
            self.assertIn(s, CPP)
        self.assertIn('if (value == UINT64_MAX) return 0;', CORE)
        self.assertIn('selectionScore = 0; // NOT range', CORE)
    def test_no_copy_of_existing_observer_consumer(self):
        self.assertNotIn('war3_native_model_light_bridge.h', CPP)
        self.assertIn('RelocatedEntry(bytes.data()', CPP)
        self.assertIn('actual != expected', CPP)

if __name__ == '__main__':
    unittest.main()
