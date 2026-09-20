"""Read-only panel wiring contract; CPU/runtime/visual gates remain separate."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


class RenderStatsPanelContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ui = (ROOT / "src/d3d9/war3/ui/war3_imgui.cpp").read_text(encoding="utf-8")
        cls.panel = body(cls.ui, "void War3Imgui::drawRenderStatsPanel()")
        cls.arena = body((ROOT / "src/d3d9/war3/memory/war3_shadow_arena.cpp").read_text(encoding="utf-8"),
                         "ShadowArenaMemoryStats ShadowArena_QueryMemoryStats()")

    def test_panel_is_expanded_only(self):
        block = body(self.ui, 'if (ImGui::CollapsingHeader("渲染统计 (Render Stats)"))')
        self.assertEqual(block.strip(), "drawRenderStatsPanel();")
        self.assertNotIn("渲染队列: 激活", self.ui)

    def test_no_recording_or_scene_work(self):
        for forbidden in ("setRecording", "QueryRuntimeStatusSnapshot", "QueryShadowRuntimeBridgeSummary",
                          "requestLatestFrameBuild", "ShadowArena_QueryDiagnostics", "EmitCs", "m_war3Scene",
                          "createBuffer", "ControlFrameEvidence", "ofstream", "Logger::"):
            self.assertNotIn(forbidden, self.panel)

    def test_all_reads_inside_throttle(self):
        reads = body(self.panel, "if (m_renderStatsRefresh.due(GetTickCount64()))")
        for query in ("QueryShadowProducerRuntimeDiagnostics()", "ShadowArena_QueryMemoryStats()",
                      "QueryShadowReplayDiagnostics()", "QueryCsmResolutionDiagnostics()",
                      "QueryShadowDisplayStats()",
                      "War3Stage11SnapshotResidentCapBytes()", "GlobalMemoryStatusEx("):
            self.assertEqual(self.panel.count(query), 1)
            self.assertIn(query, reads)
        self.assertIn("m_renderStatsRefresh.reset();", body(self.ui, "void War3Imgui::shutdown()"))

    def test_atomic_memory_query_only(self):
        assignments = re.findall(r"result\.(\w+)\s*=\s*(g_\w+)\.load\(std::memory_order_relaxed\);", self.arena)
        self.assertEqual(len(assignments), 18)
        self.assertEqual(len(set(name for name, _ in assignments)), 18)
        source = (ROOT / "src/d3d9/war3/memory/war3_shadow_arena.cpp").read_text(encoding="utf-8")
        for _, variable in assignments:
            self.assertRegex(source, r"std::atomic<\w+>\s+" + variable + r"\b")
        for forbidden in ("g_frameStates", "g_ownerDevice", "QueryDiagnostics(", "getMemoryHeapInfo(", "for (", "while ("):
            self.assertNotIn(forbidden, self.arena)

    def test_truthful_unavailable_and_units(self):
        for marker in ("尚无已封口场景数据", "暂无可信采样", "不可相加", "不是总显存上限",
                       "不是物理 RAM 或显存", "各来源独立采样", "drawn 是提交统计"):
            self.assertIn(marker, self.panel)
        self.assertIn("if (a.budgetSupported && a.budgetTrusted && a.budgetFrameSerial)", self.panel)
        self.assertIn("if (samples.processMemoryValid)", self.panel)
        self.assertNotIn("if (a.residentLimitBytes)", self.panel)  # zero is valid pressure

    def test_receiver_published_lock_not_live_scene(self):
        source = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(encoding="utf-8")
        query = body(source, "ShadowDisplayStats QueryShadowDisplayStats()")
        lock = query.index("std::shared_lock<std::shared_mutex> lock(g_shadowSceneStatsMutex)")
        self.assertLess(lock, query.index("= g_shadowSceneStats."))
        self.assertEqual(len(re.findall(r"= g_shadowSceneStats\.\w+;", query)), 12)
        for forbidden in ("requestLatestFrameBuild", "snapshotBundle", "refresh", "for (", "while ("):
            self.assertNotIn(forbidden, query)

    def test_memory_and_integrity_fields_used(self):
        for name in ("drawTimeSnapshotPageResidentBytes", "drawTimeSnapshotPageUsedBytes",
                     "drawTimeSnapshotPageCapacityRejectCount", "drawTimeSnapshotPageReclaimedCount",
                     "producerRequiredCasterOmissionCount", "producerCompletenessReasonMask",
                     "drawTimeVBCacheStaticLiveBytes", "drawTimeVBCacheStaticProtectedBytes"):
            self.assertIn("p." + name, self.panel)
        for name in ("usedBytes", "residentBytes", "residentLimitBytes", "activeGenerationCount",
                     "submittedSerial", "completedSerial", "overflowCount", "quarantineCount"):
            self.assertIn("a." + name, self.panel)


if __name__ == "__main__":
    unittest.main()
