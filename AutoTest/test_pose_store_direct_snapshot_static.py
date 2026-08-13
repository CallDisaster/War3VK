from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/model/war3_model_registry.h"
CONTRACT = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"

header = HEADER.read_text(encoding="utf-8")
contract = CONTRACT.read_text(encoding="utf-8")

visitor = header.split("void forEachSnapshotPose(Fn&& fn) const", 1)[1].split(
    "std::vector<PoseRecord> snapshot() const", 1
)[0]
assert "std::shared_lock<std::shared_mutex> lock(m_mutex)" in visitor
assert "for (const auto& it : m_byRuntimeModel)" in visitor
assert "fn(it.second)" in visitor
assert "for (const auto& it : m_bySceneNode)" in visitor
assert "it.second.runtimeModelPtr == nullptr" in visitor
assert "std::vector<PoseRecord>" not in visitor
assert "out.push_back" not in visitor

capture = contract.split("auto snapshotScope = ContractCpuScope(", 1)[1].split(
    "auto directPoseScope = ContractCpuScope(", 1
)[0]
assert "poseRegistry.forEachSnapshotPose(" in capture
assert "poses.add(ConvertPose(pose, manifest.frameSerial))" in capture
assert "poseRegistry.snapshot()" not in capture

pose_only = contract.split("void ShadowRuntimeContractCache::capturePoseOnlyLiveState()", 1)[1].split(
    "ShadowFrameManifest ShadowRuntimeContractCache::snapshotManifest()", 1
)[0]
assert "poseRegistry.forEachSnapshotPose(" in pose_only
assert "poses.add(ConvertPose(pose, poseFrameSerial))" in pose_only
assert "poseRegistry.snapshot()" not in pose_only

# Root-unit supplementation and explicit full-trace diagnostics can query the
# registry from inside their loops, so they intentionally retain owned copies.
root_supplement = contract.split("void AppendRootUnitSupplementRecords(", 1)[1].split(
    "void PrioritizeRootUnitSemanticRecords(", 1
)[0]
assert "for (const auto& pose : poseRegistry.snapshot())" in root_supplement

print("pose store direct snapshot static contract passed")
