"""Phase 7.105 semantic perf probe: 在 IceCrown 开局后 30s，查 semanticContractCaptureUs / 
semanticConsumerBuildUs 等 atomic counter，定位 semantic.data 内具体哪个 sub-scope 卡。"""
import json
import sys
import time
sys.path.insert(0, '.')
from war3_autotest_mcp import (
    launch_war3_test,
    wait_for_game_ready,
    _control_plane_request,
    stop_war3,
    DEFAULT_WAR3_DIR,
)

map_path = r"E:\Work\War3\Maps\(12)IceCrown.w3m"
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
pid = launch.get("pid", 0)
print(f"launch ok={launch.get('ok')} pid={pid}")
ready = wait_for_game_ready(timeout_sec=60, pid=pid)
print(f"ready ok={ready.get('ok')} elapsed={ready.get('elapsedSec')}s")
time.sleep(30)

s = _control_plane_request(pid=pid, command="get_shadow_runtime_summary",
                           timeout_sec=10.0).get("result", {})

# 全部 semantic perf tag 统计
print("\n=== semantic.data perf breakdown (累计 calls/us 自启动) ===")
keys_pairs = [
    ("ModelHook", None, "semanticModelHookUs"),
    ("PoseHook", "semanticPoseHookCalls", "semanticPoseHookUs"),
    ("AttachmentHook", "semanticAttachmentHookCalls", "semanticAttachmentHookUs"),
    ("FrameRegistryPublish", "semanticFrameRegistryPublishCalls", "semanticFrameRegistryPublishUs"),
    ("ContractCapture", "semanticContractCaptureCalls", "semanticContractCaptureUs"),
    ("ConsumerBuild", "semanticConsumerBuildCalls", "semanticConsumerBuildUs"),
    ("ModelBuildChildPreScan", "semanticModelBuildChildPreScanCalls", "semanticModelBuildChildPreScanUs"),
    ("ModelRuntimeChildCollect", "semanticModelRuntimeChildCollectCalls", "semanticModelRuntimeChildCollectUs"),
    ("ModelRuntimeChildBootstrap", "semanticModelRuntimeChildBootstrapCalls", "semanticModelRuntimeChildBootstrapUs"),
    ("ModelRuntimeChildParentMap", "semanticModelRuntimeChildParentMapCalls", "semanticModelRuntimeChildParentMapUs"),
    ("ModelRuntimeChildOwnerPropagate", "semanticModelRuntimeChildOwnerPropagateCalls", "semanticModelRuntimeChildOwnerPropagateUs"),
    ("ModelPromoteRuntime", "semanticModelPromoteRuntimeCalls", "semanticModelPromoteRuntimeUs"),
    ("ModelSpriteHostBind", "semanticModelSpriteHostBindCalls", "semanticModelSpriteHostBindUs"),
    ("ModelRuntimeModelBinding", "semanticModelRuntimeModelBindingCalls", "semanticModelRuntimeModelBindingUs"),
    ("ModelGeosetResource", "semanticModelGeosetResourceCalls", "semanticModelGeosetResourceUs"),
    ("ModelRuntimeCtor", "semanticModelRuntimeCtorCalls", "semanticModelRuntimeCtorUs"),
    ("ModelRuntimeResolve", "semanticModelRuntimeResolveCalls", "semanticModelRuntimeResolveUs"),
    ("ModelRuntimeInitCopy", "semanticModelRuntimeInitCopyCalls", "semanticModelRuntimeInitCopyUs"),
    ("PoseRuntimePose", "semanticPoseRuntimePoseCalls", "semanticPoseRuntimePoseUs"),
    ("PoseRuntimePaletteTree", "semanticPoseRuntimePaletteTreeCalls", "semanticPoseRuntimePaletteTreeUs"),
    ("PoseRuntimeMatrixPalette", "semanticPoseRuntimeMatrixPaletteCalls", "semanticPoseRuntimeMatrixPaletteUs"),
    ("PoseSpriteFrameSourceIdentity", "semanticPoseSpriteFrameSourceIdentityCalls", "semanticPoseSpriteFrameSourceIdentityUs"),
    ("PoseSpriteFramePose", None, None),
]

print(f"  {'tag':40s} {'calls':>10s} {'totalMs':>10s} {'avgUs':>10s}")
for tag, ck, uk in keys_pairs:
    calls = s.get(ck, 0) if ck else 0
    us = s.get(uk, 0) if uk else 0
    if us > 0 or calls > 0:
        avg = us / calls if calls > 0 else 0
        print(f"  {tag:40s} {calls:>10} {us/1000.0:>9.2f}ms {avg:>9.2f}us")

# 模块运行状态
print("\n=== module enable status ===")
for k in ["semanticDataModuleEnabled", "semanticModelProducerEnabled"]:
    print(f"  {k}: {s.get(k)}")

# Frame info
fi = s.get("frameIndex", 0)
print(f"\n  frameIndex={fi}")

stop_war3(pid=pid, graceful_wait_sec=3, avoid_foreground_switch=True)
