#!/usr/bin/env python3
"""Offline contracts for the dormant persistent GPU package foundation.

These tests intentionally do not authorize a new renderer consumer.  They pin
the immutable package layout and exact proof rules so later Main/CSM/Outline
work cannot borrow an index slice using pointer identity alone.
"""

from __future__ import annotations

from dataclasses import dataclass, fields, replace
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h"
RES_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp"
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"


def align_up(value: int, alignment: int) -> int:
    if alignment <= 0:
        raise ValueError("alignment must be positive")
    return value + (-value % alignment)


@dataclass(frozen=True)
class PackageProof:
    map_epoch: int
    device_epoch: int
    package_generation: int
    geoset_data: int
    content_hash: int
    index_content_hash: int
    layout_generation: int
    vertex_count: int
    index_count: int
    index_type: int
    static_byte_offset: int
    static_byte_length: int
    index_byte_offset: int
    index_byte_length: int


def pack_package(
    cursor: int,
    static_bytes: int,
    index_count: int,
    *,
    storage_alignment: int = 256,
    budget: int = 128 << 20,
) -> tuple[PackageProof, int]:
    if static_bytes <= 0 or index_count <= 0:
        raise ValueError("empty packages are invalid")
    atlas_offset = align_up(cursor, storage_alignment)
    index_relative_offset = align_up(static_bytes, 2)
    index_bytes = index_count * 2
    package_bytes = index_relative_offset + index_bytes
    if atlas_offset + package_bytes > budget:
        raise MemoryError("static atlas budget exhausted")
    proof = PackageProof(
        map_epoch=7,
        device_epoch=11,
        package_generation=13,
        geoset_data=0x12340000,
        content_hash=0xABCDEF01,
        index_content_hash=0x10203040,
        layout_generation=1,
        vertex_count=static_bytes,
        index_count=index_count,
        index_type=0,
        static_byte_offset=atlas_offset,
        static_byte_length=static_bytes,
        index_byte_offset=atlas_offset + index_relative_offset,
        index_byte_length=index_bytes,
    )
    return proof, atlas_offset + package_bytes


class PackageLayoutTests(unittest.TestCase):
    def test_vertex_and_uint16_index_are_one_nonoverlapping_package(self) -> None:
        proof, next_cursor = pack_package(17, 101, 9, storage_alignment=64)
        self.assertEqual(proof.static_byte_offset, 64)
        self.assertEqual(proof.index_byte_offset, 166)
        self.assertEqual(proof.index_byte_offset % 2, 0)
        self.assertGreaterEqual(
            proof.index_byte_offset,
            proof.static_byte_offset + proof.static_byte_length,
        )
        self.assertEqual(next_cursor, proof.index_byte_offset + 18)

    def test_next_package_realigns_without_overlap(self) -> None:
        first, cursor = pack_package(0, 103, 21, storage_alignment=256)
        second, end = pack_package(cursor, 257, 33, storage_alignment=256)
        self.assertEqual(first.static_byte_offset, 0)
        self.assertEqual(second.static_byte_offset, 256)
        self.assertGreaterEqual(second.static_byte_offset, cursor)
        self.assertGreater(end, second.index_byte_offset)

    def test_budget_accounts_for_indices_and_padding(self) -> None:
        with self.assertRaises(MemoryError):
            pack_package(0, 100, 15, storage_alignment=16, budget=129)
        proof, end = pack_package(0, 100, 14, storage_alignment=16, budget=128)
        self.assertEqual(proof.index_byte_offset, 100)
        self.assertEqual(end, 128)

    def test_every_proof_field_is_exact(self) -> None:
        proof, _ = pack_package(0, 96, 12, storage_alignment=16)
        for field in fields(PackageProof):
            changed = replace(proof, **{field.name: getattr(proof, field.name) + 1})
            with self.subTest(field=field.name):
                self.assertNotEqual(proof, changed)


class SourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = RES_H.read_text(encoding="utf-8")
        cls.resources_source = RES_CPP.read_text(encoding="utf-8")
        cls.store_header = STORE_H.read_text(encoding="utf-8")
        cls.source = STORE_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")

    def test_proof_carries_epoch_generation_content_and_index_identity(self) -> None:
        self.assertIn("struct GpuSkinStaticPackageProof", self.header)
        proof_body = self.header.split(
            "struct GpuSkinStaticPackageProof", 1
        )[1].split("};", 1)[0]
        for token in (
            "mapEpoch",
            "deviceEpoch",
            "packageGeneration",
            "geosetData",
            "contentHash",
            "indexContentHash",
            "layoutGeneration",
            "vertexCount",
            "indexCount",
            "indexType",
            "staticByteOffset",
            "staticByteLength",
            "indexByteOffset",
            "indexByteLength",
        ):
            self.assertIn(token, proof_body)
        equality_body = self.header.split(
            "SameGpuSkinStaticPackageProof", 1
        )[1].split("}\n", 1)[0]
        for token in (field.name for field in fields(PackageProof)):
            cpp_name = "".join(
                part if i == 0 else part.capitalize()
                for i, part in enumerate(token.split("_"))
            )
            self.assertIn(f"lhs.{cpp_name} == rhs.{cpp_name}", equality_body)
        self.assertIn("ValidateGpuSkinStaticPackage", self.header)

    def test_static_atlas_declares_all_future_read_domains(self) -> None:
        static_info = self.source.split("StaticBufferInfo", 1)[1].split(
            "UploadBufferInfo", 1
        )[0]
        self.assertIn("VK_BUFFER_USAGE_STORAGE_BUFFER_BIT", static_info)
        self.assertIn("VK_BUFFER_USAGE_VERTEX_BUFFER_BIT", static_info)
        self.assertIn("VK_BUFFER_USAGE_INDEX_BUFFER_BIT", static_info)
        self.assertIn("VK_BUFFER_USAGE_TRANSFER_DST_BIT", static_info)
        self.assertIn("VK_PIPELINE_STAGE_VERTEX_INPUT_BIT", static_info)
        self.assertIn("VK_ACCESS_INDEX_READ_BIT", static_info)

    def test_vertex_and_index_payload_share_one_upload_transaction(self) -> None:
        create_body = self.source.split("createStaticResource(", 1)[1].split(
            "takeStaticUploads()", 1
        )[0]
        self.assertIn(
            "std::memcpy(blob + indexRelativeOffset, record.indices.data()",
            create_body,
        )
        self.assertIn("resource->packageSlice = DxvkBufferSlice(", create_body)
        self.assertIn("resource->staticSource = DxvkBufferSlice(", create_body)
        self.assertIn("resource->indexSource = DxvkBufferSlice(", create_body)
        self.assertIn("resource->packageSlice,\n      packageBytes", create_body)
        self.assertEqual(create_body.count("resource->pendingUpload = {"), 1)

    def test_old_static_source_abi_is_preserved(self) -> None:
        create_body = self.source.split("createStaticResource(", 1)[1].split(
            "takeStaticUploads()", 1
        )[0]
        self.assertIn(
            "m_staticAtlas, atlasOffset, staticBytes);", create_body
        )
        self.assertIn("resource->sourceLayout = layout;", create_body)

    def test_package_generation_is_not_reused_on_epoch_clear(self) -> None:
        self.assertIn(
            "m_nextStaticPackageGeneration = 1u", self.store_header
        )
        self.assertIn(
            "m_nextStaticPackageGeneration == "
            "std::numeric_limits<uint64_t>::max()",
            self.source,
        )
        clear_body = self.source.split(
            "void War3PersistentGpuPackageStore::clearEpochResources() {", 1
        )[1].split("}  // namespace dxvk::war3::gpu_skin", 1)[0]
        self.assertNotIn("m_nextStaticPackageGeneration", clear_body)

    def test_resources_preserves_public_api_and_delegates_static_path(self) -> None:
        self.assertIn("class War3PersistentGpuPackageStore;", self.header)
        self.assertIn(
            "std::unique_ptr<War3PersistentGpuPackageStore> "
            "m_persistentPackages;",
            self.header,
        )
        for method in (
            "findOrQueueStatic",
            "probeStatic",
            "prepareQueuedStaticResources",
            "takeStaticUploads",
            "retireStaticUpload",
            "staticAtlasSlice",
        ):
            self.assertIn(
                f"m_persistentPackages->{method}", self.resources_source
            )

    def test_no_renderer_consumer_is_enabled_by_this_foundation(self) -> None:
        self.assertNotIn("ValidateGpuSkinStaticPackage", self.device)
        self.assertNotIn("indexSource", self.device)


if __name__ == "__main__":
    unittest.main(verbosity=2)
