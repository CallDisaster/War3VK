"""Guard the production connection, not a replacement for the shared C++ test."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
LIFETIME = (ROOT / "src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h").read_text(encoding="utf-8")


class SnapshotLifetimeWiring(unittest.TestCase):
    def test_attempt_spans_all_allocations_and_settlement(self):
        start = DEVICE.index("War3DrawTimeSnapshotCaptureAttempt captureAttempt(")
        end = DEVICE.index("} while (false);", start)
        body = DEVICE[start:end]
        self.assertLess(body.index("captureAttempt("), body.index("entry.mapEpoch ="))
        self.assertEqual(body.count("War3AllocateStage11Snapshot("), 3)
        self.assertLess(body.index("gpuSkinShadowSettlement.commit()"), body.index("captureAttempt.commit()"))
        self.assertEqual(body.count("War3ActivateDrawTimeCacheEntry(vbCacheKey, entry)"), 1)
        self.assertEqual(body.count("War3ReleaseDrawTimeUvBacking(entry)"), 1)
        self.assertEqual(body.count("if (!currentUvBackingUsable)"), 1)
        commit = body.index("if (captureAttempt.commit()) {")
        release = body.index("war3::render::War3ReleaseDrawTimeUvBacking(entry);")
        activate = body.index("War3ActivateDrawTimeCacheEntry(vbCacheKey, entry)")
        self.assertLess(commit, release)
        self.assertLess(release, activate)
        self.assertIn(
            "if (captureAttempt.commit()) {\n"
            "          if (!currentUvBackingUsable)\n"
            "            war3::render::War3ReleaseDrawTimeUvBacking(entry);\n"
            "          War3ActivateDrawTimeCacheEntry(vbCacheKey, entry);\n"
            "        }",
            body,
        )
        self.assertNotIn("entry.frameSerial =", body)
        self.assertNotIn("entry.lastAccessFrameSerial =", body)

    def test_uv_valid_branches_set_flag_after_usable_uv(self):
        start = DEVICE.index("War3DrawTimeSnapshotCaptureAttempt captureAttempt(")
        end = DEVICE.index("} while (false);", start)
        body = DEVICE[start:end]
        self.assertEqual(body.count("bool currentUvBackingUsable = false;"), 1)
        self.assertEqual(body.count("currentUvBackingUsable = true;"), 4)
        self.assertLess(body.index("captureAttempt.commit()"), body.index("War3ReleaseDrawTimeUvBacking(entry)"))

        direct = body[body.index("if (gpuSkinSemanticDirectOnly && gpuSkinOutputHasUv) {") : body.index("} else if (gpuSkinOutputHasUv) {")]
        for text in (
            "entry.uvBuffer = gpuSkinSemanticInput.staticSource.buffer();",
            "entry.uvInfo = gpuSkinSemanticInput.staticSource.getSliceInfo(",
            "entry.uvStride = 8u;",
            "entry.uvOffset = 0u;",
            "entry.uvFormat = VK_FORMAT_R32G32_SFLOAT;",
            "entry.uvCapacity = 0u;",
            "entry.uvSnapshotPage.reset();",
            "entry.uvSnapshotOffset = 0u;",
        ):
            self.assertIn(text, direct)
        self.assertEqual(direct.count("currentUvBackingUsable = true;"), 1)

        alias_start = body.index("} else if (gpuSkinOutputHasUv) {")
        alias = body[alias_start:body.index("} else if (!gpuSkinIrreversibleBypass) {", alias_start)]
        for text in (
            "entry.uvSharesPositionBuffer = true;",
            "entry.uvBuffer = entry.positionBuffer;",
            "entry.uvPinnedAllocation = entry.positionPinnedAllocation;",
            "entry.uvInfo = entry.positionInfo;",
            "entry.uvStride = entry.positionStride;",
            "entry.uvOffset = 24u;",
            "entry.uvFormat = VK_FORMAT_R32G32_SFLOAT;",
            "entry.uvSnapshotPage = entry.positionSnapshotPage;",
            "entry.uvSnapshotOffset = entry.positionSnapshotOffset;",
        ):
            self.assertIn(text, alias)
        self.assertEqual(alias.count("currentUvBackingUsable = true;"), 1)

        same = body.index("if (uvStream == posStream && !gpuSkinSemanticBacking &&")
        same_branch = body[same:body.index("} else {", same)]
        for text in (
            "entry.uvSharesPositionBuffer = true;",
            "entry.uvStride = posStride;",
            "entry.uvOffset = uvElem->Offset;",
            "entry.uvFormat = uvFmt;",
            "entry.uvBuffer = entry.positionBuffer;",
            "entry.uvPinnedAllocation = entry.positionPinnedAllocation;",
            "entry.uvInfo = entry.positionInfo;",
            "entry.uvSnapshotPage = entry.positionSnapshotPage;",
            "entry.uvSnapshotOffset = entry.positionSnapshotOffset;",
        ):
            self.assertIn(text, same_branch)
        self.assertEqual(same_branch.count("currentUvBackingUsable = true;"), 1)

        proof = body.index("entry.uvSourceProof = currentUvSourceProof;")
        independent = body[max(0, proof - 500): body.index("currentUvBackingUsable = true;", proof) + 1]
        for text in (
            "entry.uvStride = uvStride;",
            "entry.uvOffset = uvElem->Offset;",
            "entry.uvFormat = uvFmt;",
            "entry.uvSourceProof = currentUvSourceProof;",
        ):
            self.assertIn(text, independent)

    def test_uv_cleanup_helper_contract(self):
        start = LIFETIME.index("void War3ReleaseDrawTimeUvBacking(Entry& entry) noexcept {")
        end = LIFETIME.index("// Owner-thread", start)
        helper = LIFETIME[start:end]
        for text in (
            "entry.uvBuffer = nullptr;",
            "entry.uvPinnedAllocation = nullptr;",
            "entry.uvSnapshotPage.reset();",
            "entry.uvSnapshotOffset = 0u;",
            "entry.uvInfo = {};",
            "entry.uvStride = 0u;",
            "entry.uvOffset = 0u;",
            "entry.uvFormat = {};",
            "entry.uvSharesPositionBuffer = false;",
            "entry.uvCapacity = 0u;",
            "entry.uvSourceProof = {};",
            "entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(entry);",
        ):
            self.assertIn(text, helper)
        for prefix in ("position", "index"):
            for suffix in (
                "Buffer = nullptr",
                "PinnedAllocation = nullptr",
                "SnapshotPage.reset()",
                "SnapshotOffset = 0u",
                "Info = {}",
                "Capacity = 0u",
            ):
                self.assertNotIn(f"entry.{prefix}{suffix}", helper)
        for forbidden in (
            "->used",
            "used =",
            "waitFor",
            "vkWait",
            "vkAllocateMemory",
            "createBuffer",
            "vkFreeMemory",
            "releaseBuffer",
        ):
            self.assertNotIn(forbidden, helper)

    def test_both_position_revocations_use_the_shared_transition(self):
        self.assertEqual(DEVICE.count("War3ReleaseDrawTimePositionBacking(entry)"), 2)
        for prefix in ("position", "uv"):
            for text in (f"{prefix}Buffer = nullptr", f"{prefix}PinnedAllocation = nullptr",
                         f"{prefix}SnapshotPage.reset()", f"{prefix}SnapshotOffset = 0u",
                         f"{prefix}Capacity = 0u", f"{prefix}Info = {{}}"):
                self.assertIn(text, LIFETIME)
        for forbidden in ("->used =", "waitFor", "vkFreeMemory", "createBuffer"):
            self.assertNotIn(forbidden, LIFETIME)

    def test_guard_accounting_and_failure_publication(self):
        destructor = LIFETIME.split("~War3DrawTimeSnapshotCaptureAttempt()", 1)[1].split("bool commit()", 1)[0]
        self.assertIn("if (!m_committed)", destructor)
        self.assertIn("m_entry.captureComplete = false", destructor)
        self.assertIn("m_entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(m_entry)", destructor)
        self.assertNotIn("m_entry.frameSerial =", destructor)
        self.assertIn("if (!m_entry.HasCompleteBacking())", LIFETIME)


if __name__ == "__main__":
    unittest.main()
