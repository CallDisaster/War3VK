"""Wiring guard for the bounded upload-only range candidate (not GPU proof)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/render/war3_index_upload_summary.h").read_text(encoding="utf-8")


class UploadSummaryWiring(unittest.TestCase):
    def test_source_copy_contract(self):
        self.assertEqual(DEVICE.count("war3::render::IndexUploadSummary::Copy("), 2)
        self.assertIn("pIndexData, indicesSize, indexSize", DEVICE)
        self.assertIn("data, src, iboUPBufferSize, indexStride", DEVICE)
        self.assertIn("m_war3PerDrawUpload.ibRangeSummary = uploadRange;", DEVICE)
        self.assertIn("std::memcpy(data + vertexBufferSize, pIndexData, indicesSize);", DEVICE)
        self.assertIn("std::memcpy(data, src, iboUPBufferSize);", DEVICE)

    def test_consume_exact_upload_only(self):
        block = DEVICE.split("// A draw-local summary is computed", 1)[1].split(
            "if (persistentPackageObserveEnabled", 1)[0]
        for token in ("!actualIndexDomainKnown", "DynamicSysmemIBO", "m_war3UploadRangeEnabled",
                      "StartVal == 0u", "drawTimeIndexRangeBytes == m_war3PerDrawUpload.ibUploadLength",
                      "m_war3PerDrawUpload.ibUploadBytes", "m_war3PerDrawUpload.ibStorage.ptr()",
                      "m_war3GpuSkinMapEpoch", "m_war3IndexUploadBudget.serial", "if (range.valid)"):
            self.assertIn(token, block)
        for forbidden in ("mapPtr(", "EmitCs(", "FlushBuffer(", "ComputeWar3ExactIndex", "memcpy("):
            self.assertNotIn(forbidden, block)
        self.assertIn("actualVertexStart < 0 || actualVertexEnd < actualVertexStart", DEVICE)
        self.assertIn("uint64_t(actualVertexEnd) >= totalVerts", DEVICE)
        self.assertIn("actualIndexDomainKnown && drawTimeIndexBytes != nullptr", DEVICE)

    def test_value_capability_and_boundaries(self):
        self.assertIn("private:", HEADER)
        self.assertIn("16u * 1024u, PerFrame = 256u * 1024u", HEADER)
        self.assertIn("std::array<uint8_t, 1024> scratch", HEADER)
        self.assertIn("serial == UINT64_MAX", HEADER)
        self.assertIn("bytes % width", HEADER)
        self.assertIn("std::memcpy(scratch.data(), static_cast<const uint8_t*>(src) + offset, n)", HEADER)
        self.assertIn("std::memcpy(static_cast<uint8_t*>(dst) + offset, scratch.data(), n)", HEADER)
        for token in ("destination != m_destination", "bytes != m_bytes", "width != m_width",
                      "allocation != m_allocation", "map != m_map", "serial != m_serial"):
            self.assertIn(token, HEADER)
        for forbidden in ("std::vector", "new ", "mutex", "vkCmd", "mapPtr"):
            self.assertNotIn(forbidden, HEADER)



    def test_b07_producer_slice_offsets_are_anchored(self):
        dipup_start = DEVICE.index(
            "const uint32_t indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;"
        )
        dipup_end = DEVICE.index(
            "War3TryCaptureShadowCasterDrawIndexed(PrimitiveType, 0, MinVertexIndex",
            dipup_start,
        )
        dipup = DEVICE[dipup_start:dipup_end]
        self.assertIn("m_war3PerDrawUpload.ibSlice = upSlice.slice.subSlice(", dipup)
        self.assertIn(
            "vertexBufferSize, upSlice.slice.length() - vertexBufferSize);", dipup
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibUploadBytes = data + vertexBufferSize;", dipup
        )
        self.assertIn("m_war3PerDrawUpload.ibUploadLength = indicesSize;", dipup)
        self.assertLess(
            dipup.index("IndexUploadSummary::Copy("),
            dipup.index("m_war3PerDrawUpload.ibSlice ="),
        )
        self.assertLess(
            dipup.index("m_war3PerDrawUpload.ibSlice ="),
            dipup.index("m_war3PerDrawUpload.ibUploadLength = indicesSize;"),
        )

        upload_start = DEVICE.index("void D3D9DeviceEx::UploadPerDrawData(")
        dynamic_start = DEVICE.index(
            "  if (dynamicSysmemIBO) {\n    if (unlikely(iboUPBufferSize == 0)) {",
            upload_start,
        )
        rebase_end = DEVICE.index("FirstIndex = 0;", dynamic_start) + len(
            "FirstIndex = 0;"
        )
        dynamic = DEVICE[dynamic_start:rebase_end]
        for token in (
            "uint32_t offset = indexStride * FirstIndex;",
            "reinterpret_cast<uint8_t *>(upSlice.mapPtr) + iboUPBufferOffset;",
            "reinterpret_cast<uint8_t *>(ibo->GetMappedSlice()->mapPtr()) + offset;",
            "IndexUploadSummary::Copy(",
            "data, src, iboUPBufferSize, indexStride,",
            "m_war3PerDrawUpload.ibUploadBytes = data;",
            "m_war3PerDrawUpload.ibUploadLength = iboUPBufferSize;",
            "FirstIndex = 0;",
        ):
            self.assertIn(token, dynamic)
        self.assertLess(
            dynamic.index("uint32_t offset = indexStride * FirstIndex;"),
            dynamic.index("reinterpret_cast<uint8_t *>(ibo->GetMappedSlice()->mapPtr()) + offset;"),
        )
        self.assertLess(
            dynamic.index("IndexUploadSummary::Copy("),
            dynamic.index("m_war3PerDrawUpload.ibUploadBytes = data;"),
        )
        self.assertLess(
            dynamic.index("m_war3PerDrawUpload.ibUploadLength = iboUPBufferSize;"),
            dynamic.index("FirstIndex = 0;"),
        )

    def test_b07_consumer_requires_rebased_slice_identity(self):
        block = DEVICE.split("// A draw-local summary is computed", 1)[1].split(
            "if (persistentPackageObserveEnabled", 1
        )[0]
        self.assertIn(
            "!actualIndexDomainKnown && DynamicSysmemIBO && m_war3UploadRangeEnabled &&",
            block,
        )
        self.assertIn(
            "StartVal == 0u && drawTimeIndexRangeBytes == m_war3PerDrawUpload.ibUploadLength",
            block,
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibUploadBytes, uint64_t(drawTimeIndexRangeBytes),",
            block,
        )
        self.assertIn(
            "uint32_t(drawTimeIndexStride), reinterpret_cast<uintptr_t>(m_war3PerDrawUpload.ibStorage.ptr()),",
            block,
        )
        self.assertIn(
            "m_war3GpuSkinMapEpoch, m_war3IndexUploadBudget.serial);", block
        )
        self.assertLess(block.index("!actualIndexDomainKnown"), block.index("StartVal == 0u"))
        self.assertLess(
            block.index("StartVal == 0u"),
            block.index("m_war3PerDrawUpload.ibRangeSummary.query("),
        )

    def test_b07_mandatory_consumer_checks_are_source_anchored(self):
        capture_start = DEVICE.index("War3ShadowDrawTimeCapturePhase::PositionSource")
        exact_start = DEVICE.index("if (actualIndexDomainKnown) {", capture_start)
        exact_end = DEVICE.index(
            "// Device-local IB bytes cannot be scanned on the CPU.", exact_start
        )
        exact = DEVICE[exact_start:exact_end]
        for token in (
            "int64_t(BaseVertexIndex) + int64_t(actualIndexMin)",
            "int64_t(BaseVertexIndex) + int64_t(actualIndexMax)",
            "uint64_t(actualIndexMax) - uint64_t(actualIndexMin) + 1u",
            "actualIndexMax > uint32_t(INT32_MAX)",
            "actualVertexStart < 0 || actualVertexEnd < actualVertexStart",
            "uint64_t(actualVertexEnd) >= totalVerts",
            "actualVertexStart > INT32_MAX",
            "vRangeStart = int32_t(actualVertexStart);",
            "vRangeCount = uint32_t(actualVertexCount);",
            "consumeVertexOffset = -int32_t(actualIndexMin);",
        ):
            self.assertIn(token, exact)
        guard_start = DEVICE.index("if (allStreamsFit && blendBinding == 1u) {")
        guard_end = DEVICE.index("const uint64_t exactIndexBytes =", guard_start)
        guards = DEVICE[guard_start:guard_end]
        for token in (
            "end <= uint64_t(blendInfo.size / blendStride)",
            "if (allStreamsFit && captureAlphaTest &&",
            "resolvedUvBinding == 2u) {",
            "end <= uint64_t(uvInfo.size / uvStride)",
        ):
            self.assertIn(token, guards)

if __name__ == "__main__":
    unittest.main()
