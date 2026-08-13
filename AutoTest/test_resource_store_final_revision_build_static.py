from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8", errors="replace")

start = SOURCE.index("void ShadowRuntimeContractCache::captureLiveState()")
end = SOURCE.index("void ShadowRuntimeContractCache::capturePoseOnlyLiveState()", start)
body = SOURCE[start:end]

snapshot = body.index("SnapshotPoseAttachment")
direct_pose = body.index("DirectCModelPose", snapshot)
post_revision = body.index("postAttachmentResourceRevision", direct_pose)
reset = body.index("resourcesPtr.reset();", post_revision)
final_build = body.index("if (resourcesPtr == nullptr)", reset)
root_supplement = body.index("RootUnitSupplement", final_build)

# The resource store is not consumed by pose/attachment capture. Build only
# after those producers settle the final revision, but before root supplement
# first needs resource lookup.
assert snapshot < direct_pose < post_revision < reset < final_build < root_supplement
assert body.count("resourcesPtr = buildResourceStore();") == 1
assert "resourceRevision = postAttachmentResourceRevision;" in body[post_revision:final_build]

# Existing revision/coverage reuse remains authoritative; only a missing or
# invalidated store reaches the one final build.
coverage = body.index("ResourceStoreHasReadyManifestCoverage")
assert coverage < snapshot
assert "resourcesPtr = m_resources;" in body[:coverage]

print("resource store final-revision build static checks passed")
