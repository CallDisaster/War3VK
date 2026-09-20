"""Wiring checks supplement the executable tests; they are not runtime proof."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def read(path): return (ROOT/path).read_text(encoding="utf-8")
DEVICE = read("src/d3d9/d3d9_device.cpp")
# M2-2：选择链本体（含合同严格分支）已迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（逐字节）；
# test_strict_live_cannot_fall_through 的函数体断言跟随符号改锚到模块，
# 断言内容一个字未改。
MODULE = read("src/d3d9/war3/semantic/war3_live_palette_selection.cpp")
HOOK = read("src/d3d9/war3/model/war3_model_hook.cpp")
DECODE = read("src/d3d9/war3/render/war3_current_draw_contract.cpp")
CANON = read("src/d3d9/war3/render/war3_canonical_draw.cpp")
def between(s, start, end): return s[s.index(start):s.index(end, s.index(start))]

class Wiring(unittest.TestCase):
    def test_strict_live_cannot_fall_through(self):
        body=between(MODULE,"const uint32_t requiredPaletteCount =", "auto resolvePaletteSlotIndex")
        self.assertIn("QueryOwnedRenderablePartPaletteSnapshot",body)
        self.assertIn("return false;",body)
        self.assertIn("markSource(War3SemanticPaletteSource::OwnedPartSnapshot);\n    return true;",body)
        self.assertNotIn("QueryBlendedPalette",body)
    def test_producer_invalid_slot_stops_before_legacy_reread(self):
        body=between(HOOK,"uint32_t slotIndex = UINT32_MAX;","// Phase 7.52")
        self.assertIn("SafeReadU32Fast",body)
        self.assertIn("old.sealedSelection={};",body)
        self.assertIn("continue;",body)
    def test_owned_query_locks_and_rechecks(self):
        body=between(HOOK,"bool QueryOwnedRenderablePartPaletteSnapshot(", "// Phase 7.51")
        for text in ("TryCell guard", "OwnedSnapshotMatches", "count!=selection.actualGroupCount",
                     "out.assign(entry.palette.begin()", "afterSlot!=currentSlot", "afterFrame!=currentFrame",
                     "epoch!=s_skinPaletteOwnerEpoch.load"):
            self.assertIn(text,body)
        self.assertNotIn("ResolveGlobalBlendedPalette",body)
    def test_producer_rechecks_copy(self):
        body=between(HOOK,"void RecordRenderablePartPaletteBinding(", "void CaptureRuntimeGroupPaletteBindings(")
        for text in ("TryCell guard", "strictCopyStable", "afterSlot == paletteSlotIndex",
                     "afterFrame == frameTag", "ownerEpochWitness==", "NextTicket(s_skinPalettePublicationTicket)"):
            self.assertIn(text,body)
    def test_epoch_poison_does_not_resurrect(self):
        body=between(HOOK,"void ResetMapSession()", "for (auto& entry : s_slotBlendedPaletteCache)")
        self.assertIn("!s_skinPaletteOwnerEpoch.load",body)
        self.assertIn("s_skinPaletteOwnerEpoch.store(0",body)
    def test_epoch_captured_before_native_identity(self):
        body=between(HOOK,"void CaptureRuntimeGroupPaletteBindings(", "uint64_t partSeen")
        self.assertLess(body.index("ownerEpochWitness="),body.index("void* partArrayPtr"))
    def test_superseded_publication_rejected_at_submit(self):
        body=between(HOOK,"bool IsSkinPaletteSelectionCurrent(", "bool QueryOwnedRenderablePartPaletteSnapshot(")
        self.assertIn("TryCell guard",body)
        self.assertIn("entry.sealedSelection.publicationTicket == selected.publicationTicket",body)
        self.assertIn("entry.sealedSelection.runtimeModel == selected.runtimeModel",body)
    def test_decode_all_selected_snapshots_publish_provenance(self):
        body=between(DECODE,"auto publishSelected =", "outPalette.clear();\n  g_capturedPaletteMissNoSnapshot")
        for text in ("publishSelected(*snapshot)", "publishSelected(globalSnapshot)",
                     "publishSelected(attrSnapshot)", "selected.paletteProvenance"):
            self.assertIn(text,body)
        self.assertEqual(DECODE.count("&out.paletteSelection"),2)
    def test_replacement_updates_metadata_with_payload(self):
        body=between(DEVICE,"if (rebuildOk && !submitLiveRebuildScratchTls.empty()", "War3FallbackAppendPhase::InputGroupContract")
        for text in ("CanReplace(selectedPalette, rebuildSelection)", "selectedPalette = rebuildSelection",
                     "selectedSubmitSource = rebuildSource", "drawTimeCapturedPalette = &submitLiveRebuildScratchTls",
                     "PaletteProvenance::ProducerPartPacket"):
            self.assertIn(text,body)
    def test_lease_not_reclassified_as_writer(self):
        body=between(DEVICE,"dxvk::war3::render::skin::Selection liveLeaseSelection", "auto tryFreshenLeasedPoseFromCModel")
        self.assertIn("CanReplace(leased.packet.paletteSelection, liveLeaseSelection)",body)
        self.assertIn("leased.packet.paletteSelection = liveLeaseSelection",body)
    def test_cmodel_restore_denied(self):
        body=between(DEVICE,"auto tryFreshenLeasedPoseFromCModel", "leaseLivePaletteScratch.clear();")
        self.assertIn("skin::ContractEnabled()",body)
        self.assertIn("return false;",body)
    def test_final_canonical_checks_source_and_space(self):
        self.assertIn("!skin::Usable(inputs.selectedPalette",CANON)
        self.assertIn("inputs.selectedPalette.space == skin::Space::World",CANON)
        self.assertIn("if (inputs.hasSelectedPalette)",CANON)
        self.assertIn("IsSkinPaletteSelectionCurrent(selectedPalette)",DEVICE)
    def test_actual_and_rejected_evidence(self):
        self.assertIn('"skin-selection/v1"',DEVICE)
        self.assertIn("event.kind = war3::tools::evidence::Kind::ShadowState",DEVICE)
        self.assertIn("uint64_t(canonicalItem.readinessReason)",DEVICE)
        self.assertIn("draw.inputSkinSelection = selectedPalette",DEVICE)
        self.assertIn("draw.inputSkinSelection = {}; // Semantic palette was not consumed",DEVICE)
        self.assertIn('{"skinPaletteSelection",selection}',read("src/d3d9/war3/tools/war3_frame_inputs.cpp"))
    def test_counter_complete_chain(self):
        field="semanticSceneSubmittedSkinnedPaletteSourceOwnedPartSnapshotCount"
        for p in ("src/d3d9/d3d9_war3_scene.h","src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp",
                  "src/d3d9/war3/tools/war3_diagnostics_hub.cpp","src/d3d9/war3/tools/war3_control_plane.cpp"):
            self.assertIn(field,read(p))
        # One emitter in each of the two separate existing report objects.
        perf=read("src/d3d9/war3/tools/war3_perf_monitor.cpp")
        self.assertEqual(perf.count('json << "    \\"'+field+'\\": "'),2)

if __name__=="__main__": unittest.main()
