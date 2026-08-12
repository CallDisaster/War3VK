#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/model/war3_model_registry.h").read_text(
    encoding="utf-8"
)
REGISTRY = (ROOT / "src/d3d9/war3/model/war3_model_registry.cpp").read_text(
    encoding="utf-8"
)


def body(source: str, marker: str, end: str) -> str:
    return source.split(marker, 1)[1].split(end, 1)[0]


cache = body(
    DEVICE,
    "class War3CurrentDrawPoseAugmentSnapshotCache final",
    "// ShadowObject identity/pose metadata",
)
assert "static constexpr size_t kEntryCount = 128u;" in cache
assert "std::array<Entry, kEntryCount>" in cache
assert "std::unordered_map" not in cache
assert "thread_local" not in cache
assert "PoseAugmentView value" in cache
assert "PoseRecord" not in cache
assert cache.count("registry.mutationGeneration()") >= 3
assert "(currentGeneration & 1u) == 0u" in cache
assert "observedGeneration == publishedGeneration" in cache
assert "findFirstForDirectPacketAugment(" in cache

pose_class = body(HEADER, "class PoseRegistry", "class AttachmentRigidRegistry")
assert "std::atomic<uint64_t> m_mutationGeneration{0};" in pose_class
assert "uint64_t mutationGeneration() const;" in pose_class
assert "uint64_t* mutationGenerationOut = nullptr" in pose_class

for writer, end in (
    ("void PoseRegistry::resetMapSession()", "void PoseRegistry::storeRuntimeModelRecordLocked"),
    ("void PoseRegistry::recordPose(", "void PoseRegistry::recordSpriteFramePose("),
    ("void PoseRegistry::recordSpriteFramePose(", "void PoseRegistry::recordMatrixPalette("),
    ("void PoseRegistry::recordMatrixPalette(", "bool PoseRegistry::findByRuntimeModel("),
):
    writer_body = body(REGISTRY, writer, end)
    lock = writer_body.index("std::unique_lock<std::shared_mutex> lock(m_mutex)")
    mutation = writer_body.index(
        "RegistryMutationGenerationGuard mutation(m_mutationGeneration)"
    )
    assert lock < mutation

lookup = body(
    REGISTRY,
    "bool PoseRegistry::findFirstForDirectPacketAugment(",
    "std::vector<PoseRecord> PoseRegistry::snapshot",
)
assert lookup.count("std::shared_lock<std::shared_mutex> lock(m_mutex)") == 1
assert "*mutationGenerationOut" in lookup
assert "m_mutationGeneration.load(std::memory_order_acquire)" in lookup

builder = body(
    DEVICE,
    "bool War3TryBuildShadowPacketFromCurrentDrawRecord(",
    "dxvk::war3::render::ObjectKind War3ResolveSemanticPacketObjectKindFast",
)
payload_gate = builder.index("if (needPoseMatrixPayload)")
cache_gate = builder.index("else if (poseAugmentSnapshotCache != nullptr)")
full_lookup = builder.index("findFirstForDirectPacket(", payload_gate)
cache_lookup = builder.index("poseAugmentSnapshotCache->find(", cache_gate)
fallback_lookup = builder.index("findFirstForDirectPacketAugment(", cache_lookup)
assert payload_gate < full_lookup < cache_gate < cache_lookup < fallback_lookup
assert "poseRecord.matrixPalette" in builder
assert "poseAugment.matrixPalette" not in builder

populate = body(
    DEVICE,
    "War3CurrentDrawGeosetSnapshotCache currentDrawGeosetSnapshotCache",
    "enterBuildEligiblePhase(\"PostBuild\")",
)
construct = populate.index("War3CurrentDrawPoseAugmentSnapshotCache")
loop = populate.index("for (size_t buildIndex")
call = populate.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
arg = populate.index("&currentDrawPoseAugmentSnapshotCache", call)
assert construct < loop < call < arg

print("direct pose augment snapshot cache static checks passed")
