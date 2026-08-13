"""Static contracts for Stage11 exact indexed geometry and blocker closure."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
COMMON_CPP = ROOT / "src/d3d9/d3d9_common_buffer.cpp"
SCENE_H = ROOT / "src/d3d9/d3d9_war3_scene.h"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


def source_block(text: str, start: str, end: str, offset: int = 0) -> str:
    begin = text.index(start, offset)
    finish = text.index(end, begin)
    return text[begin:finish]


class Stage11ExactIndexBlockerStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.header = DEVICE_H.read_text(encoding="utf-8")
        cls.common = COMMON_CPP.read_text(encoding="utf-8")
        cls.scene = SCENE_H.read_text(encoding="utf-8")
        cls.shadow = SHADOW_CPP.read_text(encoding="utf-8")

        cls.metadata = source_block(
            cls.device,
            "bool D3D9DeviceEx::War3CaptureShadowDrawMetadata(",
            "void D3D9DeviceEx::War3TryCaptureShadowCaster(",
        )

        capture_start = cls.device.index(
            "void D3D9DeviceEx::War3TryCaptureShadowCaster("
        )
        cls.capture = cls.device[capture_start:]
        cls.producer = source_block(
            cls.device,
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer(",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()",
        )
        cls.grouped = source_block(
            cls.device,
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(",
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
        )

    def test_exact_indexed_capture_scans_observed_ib_domain(self) -> None:
        position = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionSource",
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
        )
        for token in (
            "actualIndexMin",
            "actualIndexMax",
            "actualIndexDomainKnown",
            "mapPtr(",
            "VK_INDEX_TYPE_UINT16",
            "VK_INDEX_TYPE_UINT32",
        ):
            self.assertIn(token, position)

        self.assertRegex(
            position,
            re.compile(
                r"actualVertexStart\s*=\s*[\r\n\s]*"
                r"int64_t\(BaseVertexIndex\)\s*\+\s*"
                r"int64_t\(actualIndexMin\)"
            ),
        )
        self.assertRegex(
            position,
            re.compile(
                r"actualVertexCount\s*=\s*[\r\n\s]*"
                r"uint64_t\(actualIndexMax\)\s*-\s*"
                r"uint64_t\(actualIndexMin\)\s*\+\s*1u"
            ),
        )
        self.assertIn(
            "vRangeStart = int32_t(actualVertexStart);",
            position,
        )
        self.assertIn(
            "vRangeCount = uint32_t(actualVertexCount);",
            position,
        )
        self.assertRegex(
            position,
            re.compile(
                r"consumeVertexOffset\s*=\s*-int32_t\(actualIndexMin\)"
            ),
        )

        # War3 does not guarantee that DrawIndexedPrimitive Min/Num are strict
        # bounds. The exact lane must never crop the copied VB from those hints.
        self.assertNotIn(
            "vRangeStart = BaseVertexIndex + int32_t(MinVertexIndex);",
            position,
        )
        self.assertNotIn("vRangeCount = NumVertices;", position)

    def test_index_scan_is_cached_only_and_replay_is_ordered_gpu_copy(
        self,
    ) -> None:
        position = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionSource",
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
        )
        for token in (
            "ibUploadBytes",
            "ibUploadLength",
            "getMemoryProperties()",
            "VK_MEMORY_PROPERTY_HOST_CACHED_BIT",
            "currentIndexBytesHostCached",
            "drawTimeIndexBytes = currentIndexBytes;",
            "drawTimeIndexCommon->NeedsUpload()",
            "FlushBuffer(drawTimeIndexCommon)",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
        ):
            self.assertIn(token, position)
        self.assertNotIn("drawTimeIndexSlice.mapPtr(", position)
        self.assertNotIn("War3AllocFreezeBuffer(", position)
        self.assertNotIn("snapshotMap", position)
        self.assertLess(
            position.index("drawTimeIndexCommon->NeedsUpload()"),
            position.index("FlushBuffer(drawTimeIndexCommon)"),
        )
        self.assertLess(
            position.index("FlushBuffer(drawTimeIndexCommon)"),
            position.index("drawTimeIndexCommon->GetMappedSlice()"),
        )

        backing = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::IndexBacking",
            "entry.captureComplete = true;",
        )
        for token in (
            "drawTimeIndexSlice.buffer() != nullptr",
            "idxSrcOffset",
            "War3AllocateStage11Snapshot(",
            "entry.indexCapacity = snapshotCapacity;",
            "entry.indexSnapshotOffset",
            "entry.firstIndex = 0u;",
            "ctx->copyBuffer",
        ):
            self.assertIn(token, backing)
        self.assertIn("cDstOff = entry.indexSnapshotOffset", backing)
        self.assertNotIn("drawTimeIndexCpuSnapshot", backing)
        self.assertNotIn("indexHostSnapshot", backing)

    def test_dynamic_index_upload_is_strict_and_generation_pinned(
        self,
    ) -> None:
        position = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionSource",
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
        )
        dynamic = source_block(
            position,
            "if (DynamicSysmemIBO) {",
            "} else if (ib != nullptr)",
        )
        for token in (
            "!m_war3PerDrawUpload.ibValid",
            "m_war3PerDrawUpload.ibStorage == nullptr",
            "m_war3PerDrawUpload.ibUploadBytes == nullptr",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, dynamic)

        self.assertIn(
            "m_war3PerDrawUpload.ibUploadBytes = data;",
            self.device,
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibUploadBytes = data + vertexBufferSize;",
            self.device,
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibUploadLength = iboUPBufferSize;",
            self.device,
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibUploadLength = indicesSize;",
            self.device,
        )

        ib_create = source_block(
            self.common,
            "else if (m_desc.Type == D3DRTYPE_INDEXBUFFER)",
            "if (m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT)",
        )
        self.assertIn("VK_BUFFER_USAGE_TRANSFER_SRC_BIT", ib_create)
        self.assertIn("VK_ACCESS_TRANSFER_READ_BIT", ib_create)

    def test_exact_vertex_upload_provenance_is_recorded_for_every_up_path(
        self,
    ) -> None:
        upload_info = source_block(
            self.header,
            "struct War3PerDrawUploadInfo {",
            "War3PerDrawUploadInfo m_war3PerDrawUpload;",
        )
        self.assertIn(
            "std::array<const void*, caps::MaxStreams> vbUploadBytes = {};",
            upload_info,
        )
        self.assertIn(
            "std::array<uint32_t, caps::MaxStreams> vbUploadLength = {};",
            upload_info,
        )

        primitive_up = source_block(
            self.device,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitiveUP(",
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitiveUP(",
        )
        for token in (
            "m_war3PerDrawUpload.vbUploadBytes[0] = upSlice.mapPtr;",
            "m_war3PerDrawUpload.vbUploadLength[0] = bufferSize;",
        ):
            self.assertIn(token, primitive_up)

        indexed_up = source_block(
            self.device,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitiveUP(",
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ProcessVertices(",
        )
        for token in (
            "m_war3PerDrawUpload.vbUploadBytes[0] = data;",
            "m_war3PerDrawUpload.vbUploadLength[0] = vertexBufferSize;",
        ):
            self.assertIn(token, indexed_up)

        upload_per_draw = source_block(
            self.device,
            "void D3D9DeviceEx::UploadPerDrawData(",
            "void D3D9DeviceEx::InjectCsChunk(",
        )
        for token in (
            "m_war3PerDrawUpload.vbUploadBytes[i] =",
            "reinterpret_cast<const uint8_t*>(upSlice.mapPtr)",
            "copy.dstOffset;",
            "m_war3PerDrawUpload.vbUploadLength[i] = copy.copyBufferLength;",
        ):
            self.assertIn(token, upload_per_draw)

    def test_up_buffer_supports_ordered_shadow_transfer_reads(self) -> None:
        alloc_up = source_block(
            self.device,
            "D3D9BufferSlice D3D9DeviceEx::AllocUPBuffer(",
            "D3D9BufferSlice D3D9DeviceEx::AllocStagingBuffer(",
        )
        for token in (
            "VK_BUFFER_USAGE_TRANSFER_SRC_BIT",
            "VK_PIPELINE_STAGE_TRANSFER_BIT",
            "VK_ACCESS_TRANSFER_READ_BIT",
        ):
            self.assertIn(token, alloc_up)

    def test_dynamic_position_upload_is_strict_and_never_falls_back(
        self,
    ) -> None:
        position_source = source_block(
            self.capture,
            "const uint32_t posStream = declInfo.posStream;",
            "// Resolve the exact IB before choosing a vertex copy range.",
        )
        dynamic_gate = "} else if (DynamicSysmemVBOs) {"
        self.assertIn(dynamic_gate, position_source)
        dynamic_start = position_source.index(
            dynamic_gate
        )
        regular_start = position_source.index(
            "auto *vb = m_state.vertexBuffers[posStream].vertexBuffer.ptr();",
            dynamic_start,
        )
        dynamic = position_source[dynamic_start:regular_start]
        for token in (
            "!m_war3PerDrawUpload.vbValid[posStream]",
            "m_war3PerDrawUpload.storage == nullptr",
            "m_war3PerDrawUpload.vbUploadBytes[posStream] == nullptr",
            "m_war3PerDrawUpload.vbUploadLength[posStream] == 0u",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, dynamic)
        self.assertRegex(
            dynamic,
            re.compile(
                r"m_war3PerDrawUpload\.vbSlices\[posStream\]"
                r"\.buffer\(\)\s*==\s*nullptr"
            ),
        )
        self.assertNotIn(
            "m_state.vertexBuffers[posStream].vertexBuffer", dynamic
        )
        self.assertNotIn(
            "} else if (DynamicSysmemVBOs &&", position_source
        )

    def test_pending_regular_position_and_uv_flush_before_real_copy(
        self,
    ) -> None:
        position_source = source_block(
            self.capture,
            "const uint32_t posStream = declInfo.posStream;",
            "// Resolve the exact IB before choosing a vertex copy range.",
        )
        position_pending_start = position_source.index(
            "posCommon->NeedsUpload()"
        )
        position_real = position_source.index(
            "D3D9_COMMON_BUFFER_TYPE_REAL", position_pending_start
        )
        position_pending = position_source[
            position_pending_start:position_real
        ]
        for token in (
            "FlushBuffer(posCommon)",
            "FAILED(",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, position_pending)
        self.assertLess(
            position_source.index("posCommon->NeedsUpload()"),
            position_source.index("FlushBuffer(posCommon)"),
        )
        self.assertLess(
            position_source.index("FlushBuffer(posCommon)"),
            position_real,
        )
        self.assertNotIn(
            "D3D9_COMMON_BUFFER_TYPE_MAPPING", position_source
        )

        uv_source = source_block(
            self.capture,
            "DxvkBufferSlice uvSrcSlice;",
            "const VkDeviceSize uvSrcOffset =",
        )
        uv_pending_start = uv_source.index("uvCommon->NeedsUpload()")
        uv_real = uv_source.index(
            "D3D9_COMMON_BUFFER_TYPE_REAL", uv_pending_start
        )
        uv_pending = uv_source[uv_pending_start:uv_real]
        for token in (
            "FlushBuffer(uvCommon)",
            "FAILED(",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, uv_pending)
        self.assertLess(
            uv_source.index("uvCommon->NeedsUpload()"),
            uv_source.index("FlushBuffer(uvCommon)"),
        )
        self.assertLess(
            uv_source.index("FlushBuffer(uvCommon)"),
            uv_real,
        )
        self.assertNotIn("D3D9_COMMON_BUFFER_TYPE_MAPPING", uv_source)

    def test_dynamic_independent_uv_upload_is_strict_and_never_falls_back(
        self,
    ) -> None:
        uv_source = source_block(
            self.capture,
            "DxvkBufferSlice uvSrcSlice;",
            "const VkDeviceSize uvSrcOffset =",
        )
        dynamic_gate = "if (DynamicSysmemVBOs) {"
        self.assertIn(dynamic_gate, uv_source)
        dynamic_start = uv_source.index(dynamic_gate)
        regular_start = uv_source.index(
            "auto* uvVb =",
            dynamic_start,
        )
        dynamic = uv_source[dynamic_start:regular_start]
        for token in (
            "!m_war3PerDrawUpload.vbValid[uvStream]",
            "m_war3PerDrawUpload.storage == nullptr",
            "m_war3PerDrawUpload.vbUploadBytes[uvStream] == nullptr",
            "m_war3PerDrawUpload.vbUploadLength[uvStream] == 0u",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, dynamic)
        self.assertRegex(
            dynamic,
            re.compile(
                r"m_war3PerDrawUpload\.vbSlices\[uvStream\]"
                r"\.buffer\(\)\s*==\s*nullptr"
            ),
        )
        self.assertNotIn(
            "m_state.vertexBuffers[uvStream].vertexBuffer", dynamic
        )
        self.assertNotIn(
            "if (DynamicSysmemVBOs &&", uv_source
        )

    def test_exact_blocker_bounds_use_canonical_generation_bytes(
        self,
    ) -> None:
        marker_phase = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
            "War3ShadowDrawTimeCapturePhase::FingerprintAndDedup",
        )
        marker_candidate = source_block(
            marker_phase,
            "if (War3LegacyDrawIsPathBlockerGeometryMarkerCandidate(",
            "drawTimePathBlockerGeometryMarker =",
        )
        self.assertIn(
            "War3ComputeMappedLocalBoundsFromBytes(",
            marker_candidate,
        )
        self.assertNotIn(
            "War3ComputeMappedLocalBoundsFromSlice(",
            marker_candidate,
        )
        self.assertNotIn("posSlice.mapPtr", marker_candidate)

    def test_metadata_blocker_bounds_never_trust_index_hints_or_stale_vb(
        self,
    ) -> None:
        blocker = source_block(
            self.metadata,
            "if (anonymousBlockerProbe) {",
            "if (!War3ShadowMetadataAlphaRuntime() || !nativeAlphaTest)",
        )
        self.assertIn(
            "const bool readable = !indexed && positionBytes != nullptr",
            blocker,
        )
        self.assertIn(
            "publishBlocker(War3ShadowMetadataBlockerReason::Unreadable);",
            blocker,
        )
        self.assertLess(
            blocker.index("const bool readable = !indexed"),
            blocker.index("War3BelowGroundFlatMarkerBoundsFit(bounds)"),
        )
        self.assertIn(
            "War3ComputeMappedLocalBoundsFromBytes(", blocker
        )
        self.assertNotIn(
            "War3ComputeMappedLocalBoundsFromSlice(", blocker
        )

        dynamic_start = blocker.index("if (dynamicSysmemVbos) {")
        regular_start = blocker.index(
            "auto* vb = m_state.vertexBuffers[positionStream]",
            dynamic_start,
        )
        dynamic = blocker[dynamic_start:regular_start]
        for token in (
            "m_war3PerDrawUpload.vbValid[positionStream]",
            "uploadSlice.buffer() != nullptr",
            "m_war3PerDrawUpload.storage != nullptr",
            "m_war3PerDrawUpload.vbUploadBytes[positionStream] != nullptr",
            "m_war3PerDrawUpload.vbUploadLength[positionStream] != 0u",
            "uploadSlice.length()",
            "BuildWar3CpuReadableBufferSpan({",
            "positionBytes = positionReadableSpan.data;",
            "positionByteLength =",
        ):
            self.assertIn(token, dynamic)
        self.assertNotIn(
            "m_state.vertexBuffers[positionStream].vertexBuffer", dynamic
        )

        regular = blocker[regular_start:blocker.index(
            "War3ShadowCasterDraw markerProbe = {}", regular_start
        )]
        for token in (
            "Rc<DxvkResourceAllocation> positionMappedAllocation",
            "positionMappedAllocation = common->GetMappedSlice();",
            "positionMappedAllocation->getBufferInfo()",
            "positionMappedAllocation->mapPtr()",
            "BuildWar3CpuReadableBufferSpan({",
            "positionBytes = positionReadableSpan.data;",
            "positionByteLength =",
        ):
            self.assertIn(token, blocker if token.startswith("Rc<") else regular)
        self.assertNotIn("mappedBase + bindingOffset", regular)
        self.assertNotIn("common->Desc()->Size - bindingOffset", regular)

    def test_metadata_alpha_uv_is_generation_exact_and_fail_closed(
        self,
    ) -> None:
        alpha_uv = source_block(
            self.metadata,
            "const uint32_t uvStream = texcoord->Stream;",
            "const VkFormat uvFormat =",
        )
        dynamic_gate = "if (dynamicSysmemVbos) {"
        self.assertIn(dynamic_gate, alpha_uv)
        dynamic_start = alpha_uv.index(dynamic_gate)
        regular_start = alpha_uv.index("auto* uvVb =", dynamic_start)
        dynamic = alpha_uv[dynamic_start:regular_start]
        for token in (
            "!m_war3PerDrawUpload.vbValid[uvStream]",
            "m_war3PerDrawUpload.storage == nullptr",
            "m_war3PerDrawUpload.vbUploadBytes[uvStream] == nullptr",
            "m_war3PerDrawUpload.vbUploadLength[uvStream] == 0u",
            "metadataRejectedNoUvCount",
            "stashSkipNoUvCount",
            "return false;",
        ):
            self.assertIn(token, dynamic)
        self.assertRegex(
            dynamic,
            re.compile(
                r"uploadSlice\.buffer\(\)\s*==\s*nullptr"
            ),
        )
        self.assertIn(
            "const auto& uploadSlice = "
            "m_war3PerDrawUpload.vbSlices[uvStream];",
            dynamic,
        )
        self.assertNotIn("m_state.vertexBuffers[uvStream]", dynamic)
        self.assertNotIn("if (dynamicSysmemVbos &&", alpha_uv)

        regular = alpha_uv[regular_start:]
        for token in (
            "common->NeedsUpload()",
            "FAILED(FlushBuffer(common))",
            "metadataRejectedUploadCount",
            "stashSkipNoUploadCount",
            "return false;",
            "D3D9_COMMON_BUFFER_TYPE_REAL",
        ):
            self.assertIn(token, regular)
        self.assertLess(
            regular.index("FlushBuffer(common)"),
            regular.index("D3D9_COMMON_BUFFER_TYPE_REAL"),
        )
        self.assertNotIn("D3D9_COMMON_BUFFER_TYPE_MAPPING", regular)

    def test_indexed_metadata_alpha_rejects_before_hint_based_uv_publish(
        self,
    ) -> None:
        alpha_gate = source_block(
            self.metadata,
            "if (!War3ShadowMetadataAlphaRuntime() || !nativeAlphaTest)",
            "ShadowMaterialSignature material = {};",
        )
        indexed_start = alpha_gate.index("if (indexed) {")
        indexed_return = alpha_gate.index("return false;", indexed_start)
        indexed_reject = alpha_gate[indexed_start:indexed_return]
        for token in (
            "metadataRejectedNoUvCount.fetch_add(",
            "stashSkipNoUvCount.fetch_add(",
        ):
            self.assertIn(token, indexed_reject)
        self.assertNotIn("return true;", indexed_reject)
        for unreliable_hint in (
            "minVertexIndex",
            "numVertices",
            "baseVertexIndex",
            "firstPositionVertex",
        ):
            self.assertNotIn(unreliable_hint, indexed_reject)

        absolute_gate = self.metadata.index(
            "if (!War3ShadowMetadataAlphaRuntime() || !nativeAlphaTest)"
        )
        absolute_reject_return = self.metadata.index(
            "return false;",
            self.metadata.index("if (indexed) {", absolute_gate),
        )
        for publish_step in (
            "ShadowMaterialSignature material = {};",
            "const uint32_t uvStream = texcoord->Stream;",
            "War3AllocFreezeBuffer(uvBytes, uvStorageOffset)",
            "metadata.alphaPayloadComplete = true;",
        ):
            self.assertLess(
                absolute_reject_return,
                self.metadata.index(publish_step, absolute_gate),
            )

    def test_exact_native_position_decl_is_bounded_and_published_verbatim(
        self,
    ) -> None:
        position_source = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionSource",
            "// Resolve the exact IB before choosing a vertex copy range.",
        )
        for token in (
            "D3DDECLTYPE_FLOAT3",
            "D3DDECLTYPE_FLOAT4",
            "GetDecltypeSize(declInfo.posType)",
            "declInfo.posOffset >= posStride",
            "posStride - declInfo.posOffset",
            "!gpuSkinSemanticBacking && !gpuSkinSemanticDirectOnly",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "break;",
        ):
            self.assertIn(token, position_source)

        record = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::CacheRecordSetup",
            "War3ShadowDrawTimeCapturePhase::PositionBacking",
        )
        self.assertIn(
            "entry.positionOffset = capturePositionOffset;", record
        )
        self.assertIn(
            "entry.positionFormat = capturePositionFormat;", record
        )
        native_record = record[:record.index(
            "if (gpuSkinSemanticBacking) {"
        )]
        self.assertNotIn(
            "entry.positionFormat = VK_FORMAT_R32G32B32_SFLOAT;",
            native_record,
        )

    def test_exact_uv_decl_is_bounded_and_native_alpha_never_degrades(
        self,
    ) -> None:
        uv = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::UvBacking",
            "War3ShadowDrawTimeCapturePhase::IndexBacking",
        )
        for token in (
            "GetDecltypeSize(D3DDECLTYPE(uvElem->Type))",
            "uvElem->Offset < posStride",
            "posStride - uvElem->Offset",
            "uvElem->Offset < uvStride",
            "uvStride - uvElem->Offset",
        ):
            self.assertIn(token, uv)

        alpha_gate_start = uv.index(
            "if (entry.alphaTestEnabled && "
            "!entry.HasCompleteAlphaPayload())"
        )
        alpha_gate = uv[alpha_gate_start:]
        for token in (
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "entry.captureComplete = false;",
            "break;",
        ):
            self.assertIn(token, alpha_gate)

    def test_exact_backing_failures_are_terminal_owner_decisions(
        self,
    ) -> None:
        source_gates = source_block(
            self.capture,
            "if (gpuSkinIrreversibleBypass &&",
            "// Resolve the exact IB before choosing a vertex copy range.",
        )
        for token in (
            "m_war3DrawTimeVBCache.erase(vbCacheKey);",
            "drawTimeVBCacheRejectNoDecl++",
            "drawTimeVBCacheRejectNoPosition++",
            "drawTimeVBCacheRejectInvalidStride++",
            "drawTimeVBCacheRejectNoSlice++",
        ):
            offset = source_gates.index(token)
            neighborhood = source_gates[
                max(0, offset - 220):offset + len(token) + 220
            ]
            self.assertIn(
                "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
                neighborhood,
                token,
            )

        backing = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionBacking",
            "War3ShadowDrawTimeCapturePhase::UvBacking",
        )
        budget = backing[
            backing.index("bool positionPageBudgetDeferred = false;") :
        ]
        self.assertIn(
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            budget,
        )
        no_buffer = source_block(
            backing,
            "if (!gpuSkinSemanticBacking && !gpuSkinSemanticDirectOnly &&\n"
            "            !directStaticPositionSource &&",
            "if (!gpuSkinSemanticBacking && !gpuSkinSemanticDirectOnly) {",
        )
        self.assertIn(
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            no_buffer,
        )

    def test_exact_index_path_has_no_host_snapshot_lifecycle(self) -> None:
        for token in (
            "drawTimeIndexCpuSnapshot",
            "indexHostSnapshot",
            "m_war3DrawTimeHostSnapshotReleaseFrameSerial",
        ):
            self.assertNotIn(token, self.capture)
            self.assertNotIn(token, self.header)

    def test_gpu_skin_settlement_failure_cannot_publish_static_input(
        self,
    ) -> None:
        settlement = source_block(
            self.capture,
            "if (!committed) {",
            "} else {\n            if (gpuSkinIrreversibleBypass)",
        )
        for token in (
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "entry.captureComplete = false;",
            "entry.positionBuffer = nullptr;",
            "entry.positionInfo = {};",
            "entry.positionCapacity = 0u;",
            "entry.gpuSkinLeaseBacked = false;",
            "entry.gpuSkinInput = {};",
            "if (entry.uvSharesPositionBuffer)",
        ):
            self.assertIn(token, settlement)

    def test_unmapped_index_domain_uses_only_bounded_full_vb_fallback(
        self,
    ) -> None:
        position = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::PositionSource",
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
        )
        fallback_start = position.index("fullVertexDomainFallback")
        fallback = position[fallback_start:]
        for token in (
            "totalVerts",
            "totalVerts > 65536u",
            "vRangeStart = 0;",
            "vRangeCount = uint32_t(totalVerts);",
            "consumeVertexOffset = BaseVertexIndex;",
            "drawTimeVBCacheRejectInvalidRange++",
            "break;",
        ):
            self.assertIn(token, fallback)
        self.assertLess(
            fallback.index("totalVerts > 65536u"),
            fallback.index("vRangeCount = uint32_t(totalVerts);"),
        )

    def test_unknown_index_domain_keeps_small_anonymous_marker_fail_closed(
        self,
    ) -> None:
        marker = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds",
            "const VkDeviceSize posSrcOffset",
        )
        for token in (
            "unknownDomainSmallIndexedMarker",
            "indexed && !actualIndexDomainKnown",
            "markerIndexCount != 0u",
            "kPathBlockerAnonymousRigidMarkerMaxIndices",
            "kPathBlockerAnonymousRigidMarkerMaxVertices",
            "anonymousMarkerGeometryFits",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "semanticSceneRejectedPathBlockerEarlyBypassCount++",
        ):
            self.assertIn(token, marker)

    def test_verified_marker_rejection_owns_prior_grace_representation(
        self,
    ) -> None:
        key = source_block(
            self.header,
            "struct War3DrawTimeAnonymousMarkerSliceKey {",
            "class D3D9InterfaceEx;",
        )
        for token in (
            "void* renderablePart",
            "void* meshPayloadPtr",
            "uint32_t layerIndex",
            "War3DrawTimeAnonymousMarkerSliceKeyHash",
        ):
            self.assertIn(token, key)
        for forbidden in (
            "instanceIdentity",
            "jHandle",
            "payloadWord108",
            "payloadWord11C",
        ):
            self.assertNotIn(forbidden, key)
        self.assertIn(
            "std::unordered_map<War3DrawTimeAnonymousMarkerSliceKey, uint64_t,",
            self.header,
        )
        self.assertNotIn(
            "m_war3DrawTimeAnonymousMarkerRejectedFrameSerial",
            self.header,
        )

        marker = source_block(
            self.capture,
            "// Stage12 exact-index safety can require copying an entire",
            "War3ShadowDrawTimeCapturePhase::FingerprintAndDedup",
        )
        self.assertIn(
            "War3RememberDrawTimeAnonymousMarkerRejection(vbCacheKey)",
            marker,
        )
        self.assertEqual(
            self.device.count(
                "War3RememberDrawTimeAnonymousMarkerRejection("
            ),
            2,
        )
        for token in (
            "constexpr uint64_t kWar3AnonymousMarkerRejectionHoldFrames = 8u",
            "m_war3DrawTimeAnonymousMarkerRejectedSlices[",
            "m_war3ShadowPersistentFrameSerial - it->second <=",
            "kWar3AnonymousMarkerRejectionHoldFrames",
        ):
            self.assertIn(token, self.device)

        owner = source_block(
            self.grouped,
            "const auto currentFrameDrawTimeProducerOwnsRecord =",
            "struct DirectObjectCompletenessBucket",
        )
        for token in (
            "War3DrawTimeAnonymousMarkerRejectionActive(",
            "record.renderablePart, record.meshPayloadPtr",
            "record.layerIndex",
        ):
            self.assertIn(token, owner)
        append = source_block(
            self.device,
            "// Defensive final ownership gate.",
            "War3FallbackAppendRawTiming fallbackAppendTiming;",
        )
        for token in (
            "War3DrawTimeAnonymousMarkerRejectionActive(",
            "packet.renderable.renderablePart, contract.meshPayloadPtr",
            "packet.renderable.layerIndex",
        ):
            self.assertIn(token, append)
        self.assertNotIn(
            "directCurrentDrawSample->contract.known",
            append,
        )
        lease = source_block(
            self.grouped,
            "// An exact current-frame Stage11 decision outranks every historical",
            "if (directPartPacketLeaseFrame <= leaseIt->second.lastSubmittedFrame)",
        )
        for token in (
            "War3DrawTimeAnonymousMarkerRejectionActive(",
            "leasedPart, currentContract.meshPayloadPtr, leasedLayer",
        ):
            self.assertIn(token, lease)
        key_factory = source_block(
            self.device,
            "War3DrawTimeVBCacheKey War3MakeDrawTimeVBCacheKey(",
            "bool War3CurrentDrawContractNamesExactSlice(",
        )
        self.assertIn(
            "CurrentDrawContractHasCanonicalIdentity(*contract)", key_factory
        )
        tombstones = source_block(
            self.device,
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()",
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(",
        )
        for token in (
            "tombstone.identity.producerStage == 11",
            "m_war3DrawTimeAnonymousMarkerRejectedSlices.clear()",
            "it->first.renderablePart ==",
            "tombstone.identity.renderablePart",
            "m_war3DrawTimeAnonymousMarkerRejectedSlices.erase(it)",
        ):
            self.assertIn(token, tombstones)

    def test_invalid_observed_index_domain_fails_before_capture_complete(
        self,
    ) -> None:
        position_start = self.capture.index(
            "War3ShadowDrawTimeCapturePhase::PositionSource"
        )
        complete = self.capture.index(
            "entry.captureComplete = true;", position_start
        )
        block = self.capture[position_start:complete]
        invalid = block.index("if (actualIndexDomainKnown) {")
        fallback = block.index(
            "// Device-local IB bytes cannot be scanned on the CPU.", invalid
        )
        invalid_block = block[invalid:fallback]
        for token in (
            "actualIndexMax > uint32_t(INT32_MAX)",
            "actualVertexStart < 0",
            "actualVertexEnd < actualVertexStart",
            "uint64_t(actualVertexEnd) >= totalVerts",
            "actualVertexCount > 65536u",
            "drawTimeVBCacheRejectInvalidRange++",
            "break;",
        ):
            self.assertIn(token, invalid_block)
        self.assertLess(
            invalid,
            block.index("entry.positionInfo =", invalid),
        )

    def test_unknown_exact_identity_is_never_promoted_to_unit(self) -> None:
        object_kind = source_block(
            self.producer,
            "auto objectKind =",
            "m_war3Scene.shadowStats"
            ".drawTimeSemanticProducerVisibleCandidateCount++",
        )
        self.assertNotIn(
            ": dxvk::war3::render::ObjectKind::Unit;",
            object_kind,
        )
        self.assertIn(
            "dxvk::war3::render::ObjectKind::Unknown",
            object_kind,
        )
        self.assertIn(
            "objectKind == dxvk::war3::render::ObjectKind::Unknown",
            object_kind,
        )

        manifest = source_block(
            self.producer,
            "const auto appendExactSubmittedManifestRecord =",
            "uint32_t submitted = 0u;",
        )
        self.assertIn(
            "exactRecord.objectKind = entry.objectKind;",
            manifest,
        )
        self.assertNotRegex(
            manifest,
            re.compile(
                r"entry\.objectKind\s*!=\s*"
                r"dxvk::war3::render::ObjectKind::Unknown[\s\S]*?"
                r":\s*dxvk::war3::render::ObjectKind::Unit"
            ),
        )

    def test_exact_producer_consumes_blocker_metadata_before_publish(
        self,
    ) -> None:
        owner = self.producer.index(
            "entry.exactOwnerFrameSerial = m_war3ShadowPersistentFrameSerial;"
        )
        publish = self.producer.index("War3ShadowCasterDraw draw = {};", owner)
        block = self.producer[owner:publish]
        for token in (
            "War3ShadowDrawMetadataQuery blockerQuery = {};",
            "blockerQuery.instanceIdentity = cacheKey.instanceIdentity;",
            "blockerQuery.renderablePart = cacheKey.renderablePart;",
            "blockerQuery.meshPayloadPtr = cacheKey.meshPayloadPtr;",
            "blockerQuery.layerIndex = cacheKey.layerIndex;",
            "blockerQuery.producerStage = exactProducerStage;",
            "blockerQuery.payloadWord108 = cacheKey.payloadWord108;",
            "blockerQuery.payloadWord11C = cacheKey.payloadWord11C;",
            "War3ShadowDrawMetadataStore().lookupBlocker(",
            "metadataBlockerReason",
            "metadataBlockerKeyHash",
            "War3MarkDrawTimeExactRejectedCurrentFrame(cacheKey)",
            "continue;",
        ):
            self.assertIn(token, block)
        self.assertLess(block.index("lookupBlocker("), block.index("continue;"))

        draw_setup = self.producer[
            publish:self.producer.index(
                "// 2026-05-31：身份写入 caster", publish
            )
        ]
        self.assertIn(
            "draw.shadowMetadataKeyHash = 0u;",
            draw_setup,
        )
        self.assertIn("draw.shadowExactGeometryKeyHash =", draw_setup)
        self.assertIn("draw.shadowUnitIdentityProven =", draw_setup)
        self.assertIn(
            "objectKind == dxvk::war3::render::ObjectKind::Unit",
            draw_setup,
        )
        self.assertIn("entry.unitIdentityProven", draw_setup)
        exact_hash = source_block(
            draw_setup,
            "draw.shadowExactGeometryKeyHash =",
            "draw.shadowMetadataKeyHash =",
        )
        self.assertIn("War3DrawTimeVBCacheKeyHash", exact_hash)

    def test_early_blocker_decisions_publish_exact_rejection(self) -> None:
        helper = source_block(
            self.capture,
            "const auto markCurrentStage11ExactRejected =",
            "if (isStage13WorldObjectDraw)",
        )
        for token in (
            "stage != 11",
            "GetCurrentDrawDispatchContext()",
            "QueryCurrentDrawGeometryContract(",
            "contract.renderFrameIndex != currentRenderFrameIndex",
            "contract.producerStage != 11",
            "!contract.producerFreshThisFrame",
            "contract.fromGrace",
            "War3CurrentDrawContractNamesExactSlice(",
            "War3MarkDrawTimeExactRejectedCurrentFrame(",
        ):
            self.assertIn(token, helper)

        for start, end in (
            (
                "if (fastRawcode != 0u && IsLosBlockerFourCc(fastRawcode))",
                "// 慢路径：rawcode 还是 0",
            ),
            (
                "if (blockerRawcode != 0u && "
                "IsLosBlockerFourCc(blockerRawcode))",
                "shadowCaptureGateTiming.enter(\n"
                "      War3ShadowCaptureGatePhase::GpuSkinContract)",
            ),
            (
                "if (!earlyNeedsSemanticContext && earlyTerrainDoodadCaster",
                "if (earlyNeedsSemanticContext)",
            ),
            (
                "if (pathBlocker) {",
                "// Safe metadata-only capture must run",
            ),
            (
                "if (metadataRejectedBlocker) {",
                "const bool earlySemanticSceneUnitLikeCandidate",
            ),
            (
                "if (dxvk::war3::internal::kPathBlockerHideEnabled &&\n"
                "            (vbCacheContract.pathBlocker",
                "War3ShadowDrawTimeCapturePhase::GpuSkinInput",
            ),
        ):
            gate = source_block(self.capture, start, end)
            self.assertTrue(
                "War3MarkDrawTimeExactRejectedCurrentFrame" in gate
                or "markCurrentStage11ExactRejected" in gate,
                start,
            )

    def test_rejection_ledger_outvotes_packet_grouped_and_lease(self) -> None:
        for token in (
            "m_war3DrawTimeExactRejectedKeys",
            "m_war3DrawTimeExactRejectedFrameSerial",
            "War3MarkDrawTimeExactRejectedCurrentFrame(",
            "War3DrawTimeExactRejectedCurrentFrame(",
        ):
            self.assertIn(token, self.header)
        self.assertIn(
            "std::unordered_set<War3DrawTimeVBCacheKey, "
            "War3DrawTimeVBCacheKeyHash>",
            self.header,
        )

        append = source_block(
            self.device,
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n"
            "    const dxvk::war3::shadow::ShadowDrawPacket& packet,\n"
            "    const dxvk::war3::render::CurrentDrawAuthoritativeSample*",
            "War3FallbackAppendRawTiming fallbackAppendTiming;",
        )
        self.assertIn(
            "War3DrawTimeExactRejectedCurrentFrame(cacheKey)",
            append,
        )

        current_owner = source_block(
            self.grouped,
            "const auto currentFrameDrawTimeProducerOwnsRecord =",
            "struct DirectObjectCompletenessBucket",
        )
        self.assertIn(
            "War3DrawTimeExactRejectedCurrentFrame(cacheKey)",
            current_owner,
        )

        lease = source_block(
            self.grouped,
            "// An exact current-frame Stage11 decision outranks every "
            "historical",
            "if (directPartPacketLeaseFrame <= "
            "leaseIt->second.lastSubmittedFrame)",
        )
        self.assertIn(
            "War3DrawTimeExactRejectedCurrentFrame(currentKey)",
            lease,
        )

        overflow = source_block(
            self.device,
            "if (historyOverflowed) {",
            "const auto appendIdentityKey =",
        )
        self.assertIn("War3ResetShadowSessionState", overflow)
        central_reset = source_block(
            self.device,
            "void D3D9DeviceEx::War3ResetShadowSessionState",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones",
        )
        self.assertIn(
            "m_war3DrawTimeExactRejectedKeys.clear();", central_reset
        )
        self.assertIn(
            "m_war3DrawTimeExactRejectedFrameSerial =",
            central_reset,
        )
        self.assertLess(
            central_reset.index("m_war3DrawTimeExactRejectedKeys.clear();"),
            central_reset.index(
                "m_war3DrawTimeExactRejectedFrameSerial ="
            ),
        )

    def test_anonymous_unit_exemption_requires_explicit_identity_proof(
        self,
    ) -> None:
        self.assertIn("bool shadowUnitIdentityProven = false;", self.scene)
        proof_assignment = source_block(
            self.capture,
            "entry.unitIdentityProven =",
            "entry.vertexCount = vRangeCount;",
        )
        for token in (
            "exactUnitIdentityProven",
            "!entry.pathBlocker",
        ):
            self.assertIn(token, proof_assignment)

        proof = source_block(
            self.capture,
            "const bool exactUnitIdentityProven =",
            "War3ShadowDrawTimeCapturePhase::FingerprintAndDedup",
        )
        for token in (
            "exactUnitObject != nullptr",
            "exactUnitObject->kind ==",
            "exactUnitObject->unitPtr != nullptr",
            "vbCacheContract.unitPtr == exactUnitObject->unitPtr",
            "drawDispatchContext.valid",
            "drawDispatchContext.sceneNode != nullptr",
            "vbCacheContract.sceneNode == drawDispatchContext.sceneNode",
            "semantic.sceneNode == drawDispatchContext.sceneNode",
            "exactUnitObject->sceneNode == drawDispatchContext.sceneNode",
            "vbCacheContract.worldObjectEntry ==",
            "exactUnitObject->worldObjectEntry",
            "!vbCacheContract.pathBlocker",
            "!semantic.pathBlocker",
        ):
            self.assertIn(token, proof)
        self.assertNotIn(
            "War3SemanticContextHasDynamicUnitObjectEvidence", proof
        )

        device_gate = source_block(
            self.device,
            "inline bool War3CasterIsAnonymousSmallPathBlockerMarker(",
            "inline void NoteAnonymousSmallPathBlockerMarkerRejectLog(",
        )
        stage11_gate = source_block(
            self.shadow,
            "inline bool War3ReplayDrawIsAnonymousStage11Marker(",
            "inline bool War3ReplayDrawIsAnonymousSmallMarker(",
        )
        small_gate = source_block(
            self.shadow,
            "inline bool War3ReplayDrawIsAnonymousSmallMarker(",
            "inline void War3ReplayNoteAnonymousStage11Reject(",
        )
        for gate in (device_gate, stage11_gate, small_gate):
            self.assertIn("draw.shadowUnitIdentityProven", gate)
            exact_backed = source_block(
                gate,
                "const bool exactCurrentDrawContractBacked =",
                "if (exactCurrentDrawContractBacked)",
            )
            self.assertNotIn("draw.shadowMetadataKeyHash", exact_backed)

    def test_full_domain_small_marker_uses_referenced_vertex_upper_bound(
        self,
    ) -> None:
        helper = source_block(
            self.scene,
            "inline uint32_t War3ShadowReferencedVertexUpperBound(",
            "enum class War3ShadowReplayMode",
        )
        for token in (
            "draw.indexed",
            "draw.shadowFullVertexDomainFallback",
            "!draw.shadowActualIndexDomainKnown",
            "draw.indexCount",
        ):
            self.assertIn(token, helper)

        device_gate = source_block(
            self.device,
            "inline bool War3CasterIsAnonymousSmallPathBlockerMarker(",
            "inline void NoteAnonymousSmallPathBlockerMarkerRejectLog(",
        )
        replay_gate = source_block(
            self.shadow,
            "inline bool War3ReplayDrawIsAnonymousSmallMarker(",
            "inline void War3ReplayNoteAnonymousStage11Reject(",
        )
        for gate in (device_gate, replay_gate):
            self.assertIn("War3ShadowReferencedVertexUpperBound(draw)", gate)

        capture_reject = source_block(
            self.capture,
            "Stage12 exact-index safety can require copying an entire",
            "War3ShadowDrawTimeCapturePhase::FingerprintAndDedup",
        )
        for token in (
            "fullVertexDomainFallback",
            "actualIndexDomainKnown",
            "exactUnitIdentityProven",
            "War3CasterIsAnonymousSmallPathBlockerMarker(exactMarkerProbe)",
            "War3MarkDrawTimeExactRejectedCurrentFrame(vbCacheKey)",
            "DrawTimeExact/FullDomainAnonymousSmallMarker",
        ):
            self.assertIn(token, capture_reject)


if __name__ == "__main__":
    unittest.main()
