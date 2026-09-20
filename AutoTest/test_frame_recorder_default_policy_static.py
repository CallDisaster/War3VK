from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
def read(path):return (ROOT/path).read_text(encoding='utf-8')
class BuildDefaultWiring(unittest.TestCase):
    def test_release_option_is_not_global_debug_mode(self):
        options=read('meson_options.txt')
        self.assertIn("option('warvk_internal_frame_recorder', type : 'boolean', value : false",options)
        policy=read('src/d3d9/war3/tools/war3_frame_recorder_config.h')
        self.assertNotIn('NDEBUG',policy);self.assertNotIn('SetEnvironmentVariable',policy)
        self.assertNotIn('setenv',policy)
    def test_generated_default_has_one_authority(self):
        cpp=read('src/d3d9/war3/tools/war3_frame_evidence.cpp')
        self.assertIn('#include "war3_frame_recorder_build.h"',cpp)
        for name in ('Enabled','DrawsEnabled','LocalRecorderEnabled','InternalRecorderBuild'):
            self.assertIn('bool '+name+'() noexcept {return recorderConfiguration().',cpp)
        self.assertIn('return recorderConfiguration().inputs;',cpp)
        self.assertIn('war3::tools::evidence::DrawsEnabled()',read('src/d3d9/d3d9_war3_shadow.cpp'))
    def test_native_profiles_test_production_control(self):
        build=read('src/d3d9/meson.build')
        self.assertIn('foreach profile : [0, 1]',build)
        self.assertIn("['../../AutoTest/test_frame_recorder_defaults.cpp', 'war3/tools/war3_frame_evidence.cpp',",build)
        self.assertIn("'war3/tools/war3_frame_recorder_memory.cpp'",build)
        test=read('AutoTest/test_frame_recorder_defaults.cpp')
        for token in ('Control(', 'InternalRecorderBuild()', 'InputsEnabled()', 'RecorderDiskBudget('):self.assertIn(token,test)
    def test_resource_guard_before_cpu_and_gpu_arm(self):
        cpp=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        worker=cpp.split('void FrameHistory::runRecorder()',1)[1].split('FrameHistory::Capture',1)[0]
        self.assertLess(worker.index('RecorderDiskBudget('),worker.index('evidence::Control('))
        self.assertIn('four-GiB-image-budget',cpp);self.assertIn('insufficient-video-memory-headroom',cpp)
    def test_profile_does_not_enable_rendering_candidates(self):
        config=read('src/d3d9/war3/tools/war3_frame_evidence.cpp').split('const RecorderConfiguration&',1)[1].split('void RegisterInputExporter',1)[0]
        for token in ('SKIN_PALETTE_CONTRACT','NATIVE_MODEL_LIGHT','PERF_RECORD_ON','SHADOW_OBSERVERS'):
            self.assertNotIn(token,config)
    def test_internal_high_pressure_profile_is_used_by_all_three_rings(self):
        profile=read('src/d3d9/war3/tools/war3_frame_recorder_profile.h')
        for token in ('65536, 96, 4, 1000, 224, true','262144, 256, 4, 1000, 576, false'):
            self.assertIn(token,profile)
        history=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        self.assertIn('profile.cpuEvents',history);self.assertIn('profile.imagePreFrames',history)
        inputs=read('src/d3d9/war3/tools/war3_frame_inputs.cpp')
        self.assertIn('slotCount(DefaultRecorderProfile(InternalRecorderBuild()).inputSlots)',inputs)
        self.assertIn('m->serial%m->slotCount',inputs)
    def test_skin_palette_candidate_default_is_build_scoped_and_overridable(self):
        options=read('meson_options.txt')
        self.assertIn("option('warvk_skin_palette_contract_candidate', type : 'boolean', value : false",options)
        build=read('src/d3d9/meson.build')
        self.assertIn("'-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1'",build)
        header=read('src/d3d9/war3/render/war3_skin_palette_selection.h')
        self.assertIn('#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0',header)
        self.assertIn('return v?std::strcmp(v,"1")==0:WARVK_SKIN_PALETTE_CONTRACT_DEFAULT!=0;',header)
if __name__=='__main__':unittest.main()
