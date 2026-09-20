from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'src/d3d9/war3/tools'


class MemoryWiring(unittest.TestCase):
    def test_cpu_admits_before_allocation_and_generation(self):
        code = (TOOLS / 'war3_frame_evidence.cpp').read_text(encoding='utf-8')
        arm = code.split('if(action=="arm") {', 1)[1].split('} else if(action=="trigger")', 1)[0]
        self.assertLess(arm.index('RecorderMemoryAdmission('), arm.index('s->ring.arm('))
        self.assertLess(arm.index('s->ring.arm('), arm.index('++s->generation'))
        self.assertIn('inputs::HostPayloadBudgetForSlots(profile.inputSlots)', arm)
        self.assertIn('catch(const std::bad_alloc&)', arm)

    def test_image_partial_batch_cleanup_is_before_submission(self):
        code = (TOOLS / 'war3_frame_history.cpp').read_text(encoding='utf-8')
        arm = code.split('if(m->state==Impl::PendingArm){', 1)[1].split('m->epoch=key.mapEpoch', 1)[0]
        self.assertLess(arm.index('RecorderMemoryAdmission('), arm.index('m->device->createImage('))
        self.assertIn('for(auto& frame:m->frames)frame={}', arm)
        self.assertNotIn('vkDeviceWaitIdle', arm)
        self.assertIn('insufficient-video-memory-headroom', arm)
        self.assertIn('RecorderMemoryReason(evidence::RecorderMemoryReject(rejected))', code)

    def test_input_initialization_is_private_until_complete(self):
        code = (TOOLS / 'war3_frame_inputs.cpp').read_text(encoding='utf-8')
        init = code.split('if(!s.buffer){', 1)[1].split('if(!s.buffer||!s.fence', 1)[0]
        self.assertLess(init.index('RecorderMemoryAdmission('), init.index('m->device->createBuffer('))
        self.assertLess(init.index('storage.reserve(DrawsPerSlot)'), init.index('s.buffer=std::move(buffer)'))
        self.assertLess(init.index('auto fence='), init.index('s.buffer=std::move(buffer)'))
        self.assertIn('static_assert(sizeof(Draw)<=MaxDrawMetadataBytes', code)
        self.assertIn('if(m->allocationFailure){++m->drops;return 0;}', code)
        self.assertIn('{"ok",m->allocationFailure==nullptr}', code)
        self.assertIn('recorder-memory/v1', code)

    def test_sampler_is_current_process_bounded_and_read_only(self):
        code = (TOOLS / 'war3_frame_recorder_memory.cpp').read_text(encoding='utf-8')
        for token in ('GlobalMemoryStatusEx', 'VirtualQuery(', 'regions < 65536', 'SetLastError(value)', 'GetSystemInfo('):
            self.assertIn(token, code)
        for token in ('VirtualAlloc(', 'OpenProcess(', 'WriteProcessMemory', 'new ', 'std::vector', 'vkAllocateMemory'):
            self.assertNotIn(token, code)

    def test_tests_link_production_boundary_and_sampler_separately(self):
        build = (ROOT / 'src/d3d9/meson.build').read_text(encoding='utf-8')
        for name in ('war3_frame_recorder_memory', 'war3_frame_recorder_memory_control'):
            self.assertIn("test('" + name + "'", build)
        # 2026-09-17：控制台可执行文件把"生产边界 + 对象级 palette 证据发射器"
        # （war3_palette_object_evidence_sink.cpp，war3_frame_evidence.cpp 现在引用它）
        # 链在一起。源列表被 meson 折行，因此按**空白归一化**后的文本比对：
        # 断言口径（同一源列表里必须同时有测试 TU 与生产边界）不变，只放宽排版。
        flat = ' '.join(build.split())
        self.assertIn("['../../AutoTest/test_frame_recorder_memory.cpp', 'war3/tools/war3_frame_evidence.cpp', 'war3/tools/war3_palette_object_evidence_sink.cpp']", flat)
        self.assertIn("['../../AutoTest/test_frame_recorder_memory.cpp', 'war3/tools/war3_frame_recorder_memory.cpp']", flat)


if __name__ == '__main__':
    unittest.main()
