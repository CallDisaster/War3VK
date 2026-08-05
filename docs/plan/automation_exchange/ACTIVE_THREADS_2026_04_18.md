# Active Threads

Date: 2026-04-18

## Active

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/hooks/war3_hook_render.cpp`, `src/d3d9/war3/platform/war3_runtime_bootstrap.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `src/d3d9/war3/tools/war3_control_plane.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 14:11:53.540 +08:00`
   LastHeartbeat: `2026-04-24 14:11:53.540 +08:00`
   Status: `active`
   Goal: `把 semantic shadow 从 tail/BeforeUi 验证推进到 active CWorld stage：Stage2 后准备 native semantic frame，Stage11/12 阴影阶段执行已准备 draw，并补可观测 counters；继续 isolated desktop + windowed，禁止全屏与 snapshot/freeze/VB/IB 回退`
   Notes: `上游 owner/resource/pose 已闭环；本轮不重开 WorldObjectList+0x14、a3/sourceObject、sourceObject+0x100、childSprite->Model alias。`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `AutoTest/war3_autotest_mcp.py`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 12:03:00.646 +08:00`
   LastHeartbeat: `2026-04-24 12:07:55.371 +08:00`
   ClosedAt: `2026-04-24 12:07:55.371 +08:00`
   Status: `closed`
   Goal: `把 native successful execute 接入 AutoTest gate，让报告区分 native semantic 已执行与 DXVK validation scene pending`
   Result: `[正常关闭:(已将 AutoTest hot-shadow gate 从 current nativeD3D9BackendExecutedDrawCount 改成 stable/effective native execute draw count，优先读取 nativeD3D9BackendLastSuccessfulExecutedDrawCount，兼容回退到 ExecuteSuccessCount + LastExecuteSubmitted/Failed；新增 semanticShadowPhase=native-semantic-executed-scene-pending，用于表达 native backend 已成功执行 semantic draws 但 DXVK validation scene pass 未消费 fresh revision。python -m py_compile AutoTest/war3_autotest_mcp.py 通过。)]`
   Notes: `本轮只改 AutoTest 口径，未重新启动 War3；上一轮 runtime 证据来自 11:55 MCP direct probe。下一轮若继续验证，仍必须 isolated desktop + windowed + enforceVideoBaseline=false。`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 11:07:52.343 +08:00`
   LastHeartbeat: `2026-04-24 12:03:00.646 +08:00`
   ClosedAt: `2026-04-24 12:03:00.646 +08:00`
   Status: `closed`
   Goal: `Phase 4 render-thread scene consumption：让 DXVK scene populate 在首次/少数 render pass 内消费 fresh supplemented semantic frame，而不是停在旧 publish revision；继续 isolated desktop + windowed，禁止全屏和 snapshot/freeze/VB/IB 回退`
   Result: `[正常关闭:(已完成 render-thread latest semantic build 请求与小型 build observe 窄补丁，并为 NativeD3D9BackendRuntime 补 stable execute counters。fresh 工件 codex_model_runtime_probe_native_execute_counters_20260424_1157.summary.json 证明 semantic core 仍健康：semanticCoreFrameFresh=true、semanticCoreSubmittedDrawCount=22、semanticCoreAttachmentRigidResolved=20、objectFallbackDrawCount=0；同时 native D3D9 backend 已实际执行 draw：nativeD3D9BackendExecuteAttemptCount=58、ExecuteSuccessCount=58、SkippedNoDevice=0、SkippedNoDraws=0、LastExecuteFailedDrawCount=0。当前剩余 blocker 不是上游数据链，也不是 native backend 能不能画，而是 DXVK validation scene pass 仍只消费旧 revision：semanticScenePopulateAttemptCount=1、LastSourcePublishRevision=37、semanticScenePublishRevisionLag=4294967301。)]`
   Notes: `所有测试继续 isolated desktop + windowed + enforceVideoBaseline=false + avoid_focus_on_stop=true；12:00 复跑出现一次 ready pipe timeout（named pipe 不可用: 2），无新 crash dump、无残留 War3、videoRestorePending=false。下一轮应把 native successful execute 纳入 native path gate，并把 DXVK scene pending 作为 validation-host tail-frame 限制单独展示；不要重开 owner/resource/pose 逆向。`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`, `AutoTest/*`
   StartedAt: `2026-04-23 18:10:00.000 +08:00`
   LastHeartbeat: `2026-04-24 12:03:00.646 +08:00`
   ClosedAt: `2026-04-24 12:03:00.646 +08:00`
   Status: `closed`
   Goal: `沿 0x184E50 / 0x184ED3 AttachModelToPoint family 继续往上抬，定位在 owner-runtime 进入 full sprite-frame update 之前最后一次仍然同时持有非空 context 与 logical/source owner 的具体 producer/caller`
   Result: `[superseded/正常关闭:(后续轮次已经完成 owner/resource/pose 方向收口：AttachModelToPoint / CAttachedEffect_Init owner identity、child runtime modelResource、attachment rigid contract 与 semantic core 消费均已抬升；当前 blocker 已转到 DXVK validation scene pass / native backend gate，不再回到该 owner-producer 逆向目标。)]`
   Notes: `关闭陈旧 active marker，避免后续线程误判还处在 owner producer phase。继续参考 2026-04-24 12:03 的 native validation 结论。`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 17:42:00.000 +08:00`
   LastHeartbeat: `2026-04-23 18:06:10.000 +08:00`
   ClosedAt: `2026-04-23 18:06:10.000 +08:00`
   Status: `closed`
   Goal: `继续缩小 sprite-frame full update 的 reverse gate：区分 anonymous attachment owner-runtime hit 是 full 还是 lite，并直接捕获稳定 caller RVA，确认下一轮该沿哪条具体 family 往上抬`
   Result: `[正常关闭:(已先在 sprite-frame attachment probe 上补 full/lite/context 区分并复跑，fresh 工件 codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json 证明当前 anonymous attachment owner-runtime hit 全都发生在 full update（spriteFrameAttachmentFullUpdateHitCount=2，LiteUpdateHitCount=0，lastSpriteFrameAttachmentUpdateKind=1），而不是 lite 路线；同时进入该层时 context 仍为空（spriteFrameAttachmentContextHintCount=0，lastSpriteFrameAttachmentContextPtr=0）。随后继续补 caller RVA 观测并复跑，fresh 工件 codex_model_runtime_probe_sprite_frame_caller_20260423_180513.json 进一步坐实：owner-runtime hit 的 caller 已经稳定收敛到 0x184ED3，且 CallerChangedCount=0，说明两次命中都来自同一上游调用点。由于 0x184ED3 落在 0x184E50 AttachModelToPoint family 内部，当前 blocker 已继续收紧为：anonymous attachment owner-runtime 已进入 0x6F182300 / 0x6F1820C0 full update，但真正需要继续上抬的具体 family 已不再是泛化的 sprite-frame caller 链，而是 0x184E50 AttachModelToPoint family 的更上游 producer/caller。)]`
   Notes: `本轮所有运行继续保持 useIsolatedDesktop=true、windowed=true；收尾 current_state() 显示 war3Alive=false。关键事实：spriteFrameAttachmentOwnerRuntimeHitCount=2、ContextHintCount=0、FullUpdateHitCount=2、LiteUpdateHitCount=0、CallerKnownCount=2、CallerChangedCount=0、lastSpriteFrameAttachmentCallerRva=0x184ED3。`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 16:31:00.000 +08:00`
   LastHeartbeat: `2026-04-23 17:40:37.000 +08:00`
   ClosedAt: `2026-04-23 17:40:37.000 +08:00`
   Status: `closed`
   Goal: `验证当前 anonymous attachment root/owner/child runtime 是否会进入 0x6F182300 / 0x6F1820C0 的 sprite-frame update 层，并继续保持隔离桌面窗口模式避免影响前台`
   Result: `[正常关闭:(已先用 codex_model_runtime_probe_sprite_frame_attachment_20260423_171521.json 证明当前 anonymous attachment family 不是“完全不进 sprite-frame 层”，而是 owner runtime 会进入 sprite-frame，且 roleMask=2；随后补了 full/lite/context 区分并重新编译，在隔离桌面窗口模式下用 codex_model_runtime_probe_sprite_frame_kind_20260423_174025.json 复跑，结果进一步坐实：owner-runtime hit 全都发生在 full update（lastSpriteFrameAttachmentUpdateKind=1，spriteFrameAttachmentFullUpdateHitCount=2，LiteUpdateHitCount=0），但进入该层时 context 仍为空（spriteFrameAttachmentContextHintCount=0，lastSpriteFrameAttachmentContextPtr=0），同时 identity 侧没有任何抬升（attachmentRigidCountWithAnyIdentity / contractAttachmentRigidCountWithAnyIdentity / semanticCoreAttachmentRigidResolved 仍为 0）。当前 blocker 已收紧为：anonymous attachment family 的 owner runtime 已进入 0x6F182300 / 0x6F1820C0，但要继续沿其 caller 链向上找“最后一个仍然带着非空 a3/context 且同时持有 logical/source owner 的函数”；主线不再回到 lite update，也不再回到 0x6F131F60。)]`
   Notes: `本轮所有运行均保持 useIsolatedDesktop=true、windowed=true；收尾 current_state() 显示 war3Alive=false，没有前台残留 War3。关键事实：spriteFrameAttachmentRootRuntimeHitCount=0、OwnerRuntimeHitCount=2、ChildRuntimeHitCount=0，说明命中的不是 root/child，而是 owner runtime；同时 spriteFrameAttachmentContextHintCount=0 证明当前 full update 层看到的 owner-runtime hit 已经不再携带非空 context。`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 15:42:26.930 +08:00`
   LastHeartbeat: `2026-04-23 16:14:56.252 +08:00`
   ClosedAt: `2026-04-23 16:14:56.252 +08:00`
   Status: `closed`
   Goal: `验证当前已进入 parent-link 图的 anonymous attachment runtime 是否只是漏进了 ShadowModelResourceCache / runtime registries；优先把 child-link 观察时的 runtime->modelResource bootstrap 编译并在隔离桌面窗口模式下做 fresh model_runtime_probe`
   Result: `[正常关闭:(已先修复 child-link runtime->modelResource bootstrap 的编译缺口，并在隔离桌面窗口模式下用 codex_model_runtime_probe_child_link_bootstrap_20260423_155157.json 复跑，结果证明仅靠 RecordObservedRuntimeChildLink 里的 direct runtime bootstrap 仍然无法把当前 anonymous attachment sample 拉进 child runtime resource/pose/identity contract；随后继续把 CModelDataOffsets::ChildRuntimeGroupRecords/Count 接进 BuildChildRuntimeModelLinks，显式按资源侧 child group -> runtime child bucket 一一配对并回灌 runtime creation provenance + modelResource binding，再用 codex_model_runtime_probe_child_group_bridge_20260423_160930.json 验证，结果依然是 attachmentRigidChildRuntimeRecordKnownCount / ModelResourceKnownCount / PoseKnownCount 全为 0，semanticCoreAttachmentRigidResolved 仍为 0。与此同时，sample rootRuntimeModelPtr 与 lastRuntimeChildLinkBuildParentRuntimeModelPtr 完全不是同一棵 runtime，sample child 仍没有 createModelDataPtr，而 owner runtime 依旧只有 0x12A3E8 这条旧 provenance。当前 blocker 已进一步收紧为：anonymous attachment sample 的 parent-link 虽然可观测，但并不是当前 hot BuildChildRuntimeModelLinks family 直接产出的那批 child，下一轮必须从 0x6F12EC90 / 0x6F12E900 / 0x6F182300 之上的 local-point/controller producer family 继续上抬，而不是继续在 0x6F131F60 上 blind-fix。)]`
   Notes: `本轮所有运行均保持 useIsolatedDesktop=true、windowed=true，且收尾检查 current_state 显示 war3Alive=false，不存在前台残留 War3。关键事实：codex_model_runtime_probe_child_link_bootstrap_20260423_155157.json 与 codex_model_runtime_probe_child_group_bridge_20260423_160930.json 都显示 attachmentRigidCount=2、attachmentRigidCountWithAnyIdentity=0、contractAttachmentRigidCountWithAnyIdentity=0、semanticCoreAttachmentRigidResolved=0、attachmentRigidChildRuntimeParentLinkKnownCount=2；后者进一步显示 attachment sample 的 rootRuntimeModelPtr=466385192/466387124，但 lastRuntimeChildLinkBuildParentRuntimeModelPtr=757641296，说明当前 anonymous sample 的 parent-link 不是这轮 hot build hook 样本直接产出的 family。`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 14:55:45.000 +08:00`
   LastHeartbeat: `2026-04-23 15:19:01.110 +08:00`
   ClosedAt: `2026-04-23 15:19:01.110 +08:00`
   Status: `closed`
   Goal: `验证当前 anonymous attachment sample child runtime 是否虽然已经进了 parent-link 图，但其实仍然不在 model/resource/pose/source-object registry 里，并尝试用最窄 fallback 把 owner/source 或 direct semantic key 回灌到 attachment contract`
   Result: `[正常关闭:(已确认当前 fresh anonymous attachment sample 与上一轮不是同一批样本；在最新两轮隔离桌面窗口模式 probe 下，attachmentRigidChildRuntimeParentLinkKnownCount 与 contractAttachmentRigidChildRuntimeParentLinkKnownCount 都稳定为 2，说明当前匿名 child runtime 已经进了 parent-link 图；但 attachmentRigidChildRuntimeRecordKnownCount / ModelResourceKnownCount / PoseKnownCount 仍全部为 0，且 attachmentRigidCountWithAnyIdentity / contractAttachmentRigidCountWithAnyIdentity / semanticCoreAttachmentRigidResolved 仍然都是 0。随后补了两条窄 fallback：一是 PublishRuntimeSourceObject 时立刻尝试 source-object -> owner identity publish，二是 TryResolveRuntimeModelSemanticKey 增加 OwnedModelDataHandle -> direct modelResourcePtr fallback；两轮 fresh 工件都证明这两条 fallback 对当前匿名 attachment 样本没有带来 identity 抬升，当前 blocker 已收紧为“anonymous attachment 现在属于 parent-link family，但仍不 join 到 model/resource/pose/source-object registry，下一轮应直接围绕 child/root runtime 的 direct handle/modelData 发布层或 sourceMeta=3/10 的 builder 家族继续上抬”。)]`
   Notes: `本轮 fresh 工件：codex_model_runtime_probe_child_registry_20260423_145545.json、codex_model_runtime_probe_owner_identity_from_source_20260423_151209.json、codex_model_runtime_probe_direct_semantic_key_20260423_151823.json。关键事实：所有运行均 useIsolatedDesktop=true、windowed=true，不会切前台全屏；最新匿名样本 childRuntimeModelPtr 已稳定命中 parent-link（sample0/1 的 ParentRuntimeModelPtr 非 0，sourceMeta=3/10），但 child runtime 仍无 createModelData/sourceObject/modelResource/pose，owner/root/sourceObject 也仍为 0，说明当前问题已经不是“ancestor merge 不工作”，而是 parent-link family 之上的 runtime publish 契约仍没进现有 registries。`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 13:18:32.000 +08:00`
   LastHeartbeat: `2026-04-23 13:44:14.000 +08:00`
   ClosedAt: `2026-04-23 13:44:14.000 +08:00`
   Status: `closed`
   Goal: `在保持隔离桌面窗口模式测试的前提下，验证当前 anonymous attachment sample child runtime 是否真的已经进了 runtime parent-link 图，并排除“ancestor merge 自身失效”这条解释`
   Result: `[正常关闭:(已确认当前 model_runtime_probe 仍是 useIsolatedDesktop=true + windowed=true，不会切前台全屏；同时两轮 fresh probe 已坐实 runtimeChildLinkBuildCount=505 / runtimeChildLinkBuiltChildCount=2489 说明 child-link family 是热的，但 attachmentRigidChildRuntimeParentLinkKnownCount 与 contractAttachmentRigidChildRuntimeParentLinkKnownCount 仍然都是 0，sample child runtime 完全没有 parent-link entry，blocker 已继续收紧到“anonymous attachment sample child runtime 属于另一族更早 producer/construction family”)]`
   Notes: `本轮新增只读 parent-link query：war3_model_hook::QueryRuntimeParentLink(...)，并把 raw/contract attachment sample 的 child->parent runtime/sourceMeta/lastSeenFrame 接进 get_shadow_runtime_summary。fresh 工件 codex_model_runtime_probe_parent_link_20260423_134238.json 与 codex_model_runtime_probe_parent_link_retry_20260423_134414.json 都显示 attachmentRigidCount=2、attachmentRigidChildRuntimeParentLinkKnownCount=0、contractAttachmentRigidChildRuntimeParentLinkKnownCount=0、attachmentRigidSample0/1ChildRuntimeParentRuntimeModelPtr=0、attachmentAncestorIdentityHintWriteCount=0，而 launch 元数据同时证明 useIsolatedDesktop=true、windowed=true。下一轮不再继续在 ancestor merge 层 blind-fix，而是直接向更早 producer/construction family 上抬。`

3. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 03:20:27.716 +08:00`
   LastHeartbeat: `2026-04-23 03:36:43.793 +08:00`
   ClosedAt: `2026-04-23 03:36:43.793 +08:00`
   Status: `closed`
   Goal: `沿 CreateSpriteAndBindSourceObject / CreateSpriteRuntimeFromSourceObject / CSpriteUber_PreRenderAndUpdatePosePalette 的 caller/producer 链继续上抬，找到第一次同时持有 logical/source object、sprite/root runtime 与 prerender context 的 authoritative owner producer`
   Result: `[正常关闭:(已把 sourceObjectPtr/source-sprite 血缘正式并入 ModelInstanceRegistry、AttachmentRigidRegistry 与 attachment contract，并确认该 producer family 在 fresh model_runtime_probe 下是真热；但匿名 attachment 的 3 条 record 依然 0 条命中 sourceObject，说明当前 anonymous attachment runtime tree 并不 join 到这条 CreateSpriteAndBindSourceObject / CSpriteUber_PreRenderAndUpdatePosePalette producer runtime 家族，blocker 已继续收敛到“另一族 runtime producer 仍未定位”)]`
   Notes: `本轮新增 runtime source-object publication contract：CreateSpriteAndBindSourceObject、AttachedEffectInit/Direct、AttachModelToPoint、CSpriteUber sprite-frame source 都会把 sourceObjectPtr/source-sprite 记到 runtime；同时 attachment rigid publish 会尝试从 child/owner/root runtime 回收这条 bloodline，并把 sourceObject 穿进 contract 与 control-plane summary。fresh 工件 codex_model_runtime_probe_source_object_producer_20260423_033623.json 显示 runtimeSourceObjectCount=43、runtimeSourceObjectPublishCount=49，说明 producer side 是热的；但 attachmentRigidCountWithSourceObject=0、contractAttachmentRigidCountWithSourceObject=0、attachmentRigidSourceObjectFromChild/Owner/RootRuntimeCount 全为 0、semanticCoreAttachmentRigidResolved 仍为 0，说明当前那 3 条匿名 attachment 不是这条 producer family 的孩子。IDA 本轮已补 SourceObject_GetSpriteSourceObject(0x6F6A0AD0) 命名与注释；下一轮不再继续在当前 source-object producer family 里猜 owner，而是要直接追生成这 3 条匿名 attachment root/owner/child runtime 的另一族 producer/caller。`

4. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 02:56:59.741 +08:00`
   LastHeartbeat: `2026-04-23 03:15:16.465 +08:00`
   ClosedAt: `2026-04-23 03:15:16.465 +08:00`
   Status: `closed`
   Goal: `把 render TLS worldObjectEntry/sceneNode 直接接进 model/attachment owner 发布链，验证当前匿名 attachment runtime 是否其实已经运行在带 world-object 身份的 pre-render 调用栈内`
   Result: `[正常关闭:(已用两轮 fresh model_runtime_probe 坐实当前 anonymous attachment runtime 在 model hooks 时机里既拿不到 WorldObjectEntry_Render 的 current world-object TLS，也拿不到 ExecBatch semantic/current-batch TLS；currentRenderIdentityHintCount 分别为 68/69，但 Resolved 始终为 0，因此 render-side backfill 路线被正式排除，blocker 继续收敛到 0x6F182300/0x6F1820C0 之上的 owner producer/caller)]`
   Notes: `本轮先把 War3Renderer current world-object TLS 接进 SpriteHost/AttachedEffect/AttachModelToPoint/SpriteFrameSource/AttachmentRigid 发布链，再把 War3RenderState::GetTlsShadowSemanticState() 与 GetCurrentBatchObject() 也并进去；工件 codex_model_runtime_probe_tls_owner_bridge_20260423_0310.json 与 codex_model_runtime_probe_tls_semantic_owner_bridge_20260423_0318.json 都显示 attachmentRigidCount=3、contractAttachmentRigidCountWithAnyIdentity=0、semanticCoreAttachmentRigidResolved=0，且 currentRenderIdentityResolvedCount=0；同时已在 IDA 内补了 CSpriteUber_PreRenderAndUpdatePosePalette / CSpriteMini_PreRenderAndUpdatePosePalette / CreateSpriteAndBindSourceObject / CreateSpriteRuntimeFromSourceObject / AttachModelToPoint_CreateChildSpriteRuntimeIfNeeded / WorldObjectList_AppendEntryWithOwnerHint 等命名与注释，下一轮不再继续做 render-side TLS backfill，而是直接上抬 producer/caller 链`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/render/*`, `src/d3d9/war3/hooks/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 02:06:45.787 +08:00`
   LastHeartbeat: `2026-04-23 02:39:40.999 +08:00`
   ClosedAt: `2026-04-23 02:39:40.999 +08:00`
   Status: `closed`
   Goal: `沿 render-layer identity recover 链继续上抬，确认 WorldObjectListEntry 的真实写入语义，并用 live probe 验证 +0x14 owner hint 是否只在 root entry 上热`
   Result: `[正常关闭:(已用 fresh probe 坐实当前 model_runtime_probe 下真正进入 WorldObjectEntry_Render 的 59 条 entry 全部都对回 zero-owner list write；同时把 render-layer recover 方法直接套到 0x6F182300 的 a3/sourceObject 上仍然完全不命中，因此 blocker 已继续收敛到 0x6F182300/0x6F1820C0 caller 之上的 context producer)]`
   Notes: `本轮先收尾并补齐 WorldObjectEntry_Render <-> WorldObjectListEntry writer 的 ownerHint 对账，工件 codex_model_runtime_probe_list_write_refine_20260423_023030.json 显示 worldObjectEntryRenderKnownListOwnerHintZeroCount=59、Nonzero=0、Unknown=0；随后在 TryResolveSourceObjectIdentity(...) 接入 render-layer fallback，把 sourceObjectPtr 当 worldObjectEntry/sceneNode 回查 ShadowObjectRegistry，fresh 工件 codex_model_runtime_probe_source_render_bridge_20260423_023835.json 显示 sourceObjectRenderBridgeResolvedByEntryCount=0、BySceneNodeCount=0、spriteFrameSourceHintCount=29 但 ResolvedIdentity 仍为 0；下一轮不再继续把 a3/sourceObject 当 render object，而是直接追 0x6F182300 / 0x6F1820C0 的 caller`

## Closed

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-23 00:08:14.209 +08:00`
   LastHeartbeat: `2026-04-23 00:17:40.529 +08:00`
   ClosedAt: `2026-04-23 00:17:40.529 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已验证 render-side 已知 identity 补进 runtime-owner map 后 runtimeOwnerIdentityCount 从 19 抬到 22，但 anonymous attachment 仍完全匿名；同时 fresh probe 已证明 worldObjectEntry + 0x20(sceneNode) 在 WorldObjectEntry_Render 进来之前就已全部写好，且该函数前后不会改写 sceneNode)]`
   Notes: `本轮先对齐 render identity bridge 并在 noteInstanceIdentityLocked(...) 补上 propagateRuntimeOwnerIdentityLocked(...)，fresh 工件 codex_model_runtime_probe_20260423_render_owner_bridge.json 显示 runtimeOwnerIdentityCount=22 但 contractAttachmentRigidCountWithAnyIdentity 仍为 0；随后新增 WorldObjectEntry_Render lifecycle probe，并用 codex_model_runtime_probe_20260423_render_identity_lifecycle.json 证明 sceneNodeReadyBefore/After 均为 59、filledByCall=0、changedByCall=0；下一轮不再继续怀疑 WorldObjectEntry_Render 是 sceneNode 写入点，而是直接上抬去查 WorldObjectListEntry 构建层或 source object -> sprite/runtime 绑定层`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-23 00:03:41.905 +08:00`
   LastHeartbeat: `2026-04-23 00:04:38.801 +08:00`
   ClosedAt: `2026-04-23 00:04:38.801 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已验证 CSpriteUber__PreRenderAndUpdatePosePalette 的 a3 外部上下文在 model_runtime_probe 下是真热，但 direct resolve 仍为 0；anonymous attachment rigid 依然完全匿名，因此 blocker 已继续收敛到 0x6F182300 caller 之上的 source/object 发布层)]`
   Notes: `本轮先补了 sprite<->runtime 绑定回灌，fresh probe 仍未抬起 contractAttachmentRigidCountWithAnyIdentity；随后新增 sprite-frame source probe，并用 codex_model_runtime_probe_20260423_000432.json 证明 spriteFrameSourceHintCount>0 而 spriteFrameSourceResolvedIdentityCount=0；下一轮不再继续把 0x6F182300 的 a3 当 owner 本体，而是直接追是谁在 caller 层构造并传入这块外部上下文`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 23:14:17 +08:00`
   LastHeartbeat: `2026-04-22 23:28:53 +08:00`
   ClosedAt: `2026-04-22 23:28:53 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已通过两轮 fresh model_runtime_probe 证明 anonymous attachment rigid 既没有 arg-block identity，也没有 spritePtr 绑定；parentSprite repair 同样完全不命中，blocker 已进一步上抬到 sprite 之上的 source/object 发布层)]`
   Notes: `本轮先补了 argBlock/arg4Block identity-hint 与 parentSprite fallback，再用 codex_model_runtime_probe_20260422_232247.json 验证两条恢复链都为 0；随后新增 sprite-bound candidate 观测并再次跑 codex_model_runtime_probe_20260422_232806.json，确认 localPointSpriteBoundCandidateWriteCount 仍为 0；下一轮不再继续做 sprite repair，而是直接追 0x6F12E900/0x6F12EB70 caller 的 source-object 发布面`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 17:20:17.419 +08:00`
   LastHeartbeat: `2026-04-22 17:21:47.808 +08:00`
   ClosedAt: `2026-04-22 17:21:47.808 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已确认 0x6F6BB2C0/AttachedEffectInit 在当前 model_runtime_probe 里是真热且能解析出 attachment identity，0x6F6B9FF0 仍然完全不热；但 current attachment rigid 那 2 条匿名 record 仍未吃到这批 identity，blocker 已继续收敛到 attachment rigid raw/contract runtime join 本身)]`
   Notes: `本轮先落了 0x6F6B9FF0 hook 和 attachedEffectInit/attachedEffectDirect 分层计数，再用 codex_model_runtime_probe_20260422_171401.json / 171928.json 做 fresh 验证；随后补了 live attachment prefer 修补，但 contractAttachmentRigidCountWithAnyIdentity 仍为 0；下轮不再继续深挖 6B9FF0，而是直接把 anonymous attachment rigid raw/contract record 的 root/owner/child runtime sample 打出来，确认它们是否就是当前已知有 identity 的那批 child runtime`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 14:54:25 +08:00`
   LastHeartbeat: `2026-04-22 14:54:25 +08:00`
   ClosedAt: `2026-04-22 14:54:25 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已验证 CSprite_AttachModelToPoint 是 live parent->child 挂接点，但这层 parent sprite 仍然匿名；attachModelToPointBindCount=18 而 resolvedIdentityCount=0，remaining attachment rigid blocker 继续收缩到更上游的 object/widget->sprite owner 发布层)]`
   Notes: `本轮先补了 attachment sprite-chain repair，但 fresh model_runtime_probe 仍保持 contractAttachmentRigidCountWithAnyIdentity=0；随后新增 0x6F184E50 hook 并再次跑 run_named_scenario(model_runtime_probe)，确认热命中存在但 owner identity 仍为 0；下轮不再重复试 sprite/runtime repair，而是直接追 object/widget->sprite 绑定层`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 11:13:52.026 +08:00`
   LastHeartbeat: `2026-04-22 12:44:16.833 +08:00`
   ClosedAt: `2026-04-22 12:44:16.833 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已验证 attachment rigid live contract fallback 生效，contractAttachmentRigidCount 已稳定抬到 3；但 contractAttachmentRigidCountWithAnyIdentity 仍为 0，semanticCoreAttachmentRigidResolved 仍为 0，当前 blocker 已切到必须从 local-point producer/controller 链直接拿 identity)]`
   Notes: `本轮 fresh 验证使用 run_named_scenario(model_runtime_probe)；control-plane/hot-frame safety gate 仍稳定停在 render.isInGame=false -> wait_until stalled，没有再复发 CPU storm；下轮不再继续堆 downstream join，而是直接追 producer-side identity owner`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 04:23:49.925 +08:00`
   LastHeartbeat: `2026-04-22 11:13:52 +08:00`
   ClosedAt: `2026-04-22 11:13:52 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已把 current blocker 收敛到 a3 arg block / root runtime / child-link 映射这一条线上，下一轮直接转入 attachment rigid contract 发布与 pipe-first hot-frame safety gate 实装)]`
   Notes: `本轮未回退到旧 VB/IB 语义恢复路线；当前下一步固定为 root-runtime 命中计数、attachment rigid live store、runtime contract capture、renderer core rigid 消费与 AutoTest hot-frame 节流验证`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 04:01:32.597 +08:00`
   LastHeartbeat: `2026-04-22 04:21:07.715 +08:00`
   ClosedAt: `2026-04-22 04:21:07.715 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已通过 quick19/20/21 证明 local-point write context 既不直接带 child links，也在近邻 context 窗口里找不到 owner/root runtime，当前 blocker 已收敛到 override eval context -> root runtime handoff)]`
   Notes: `本轮证据为 war3_upstream_quick19 / war3_upstream_quick20 / war3_upstream_quick21；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；下一轮不再继续猜 slot/tag/sourceRecord remap，而是直接追 controller/eval context 的 owner root`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 03:48:23.157 +08:00`
   LastHeartbeat: `2026-04-22 03:59:56.341 +08:00`
   ClosedAt: `2026-04-22 03:59:56.341 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已把 controller-output probe 接到 0x6F77DA20/0x6F77DAA0/0x6F77DF10，并通过 quick18/live18 确认 local-point output 热帧大量活跃，而 primary/shared preset 在 model_runtime_probe 里仍为 0)]`
   Notes: `本轮证据为 war3_upstream_live18 / war3_upstream_quick18；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；当前下一步已收敛到 node_type=7 local-point output slot 与 child link tag 的映射，而不是继续扩 CModel + 0x60`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 03:35:17.783 +08:00`
   LastHeartbeat: `2026-04-22 03:39:28.096 +08:00`
   ClosedAt: `2026-04-22 03:39:28.096 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已把 probe 再推进到 post 0x6F12EC90，并用 IDA 复核 0x6F77C280/0x6F77CDD0；fresh live17 仍确认 descendant pose 通过 CModel + 0x5C/+0x60 为 0，当前更像 controller/local-point propagation 而非最终 palette publish)]`
   Notes: `本轮证据为 war3_upstream_live17；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；当前下一步已收敛到查 0x6F77BFE0 / 0x6F77CF40 / 0x6F789C60 这一组 controller/output block`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 03:09:31.184 +08:00`
   LastHeartbeat: `2026-04-22 03:32:10.241 +08:00`
   ClosedAt: `2026-04-22 03:32:10.241 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已补齐四条 wrapper-level sprite pose 路径、补上 descendant runtime tree-level binding pass，并试了 runtime-owner root 直用；shadowRuntimeModelCount 从 218 抬到 309~310，但 fresh live 仍证明 primary blocker 还是 descendant pose publication 缺失)]`
   Notes: `本轮证据为 war3_upstream_quick15/16/17 与 war3_upstream_live14/15/16；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；当前下一步已收敛到查 0x6F12EC90 -> sub_6F77C280(...) 及 child/attachment runtime 是否根本不把最终姿态留在 CModel + 0x5C/+0x60`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 02:42:47.913 +08:00`
   LastHeartbeat: `2026-04-22 03:04:35.581 +08:00`
   ClosedAt: `2026-04-22 03:04:35.581 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已把 no-runtime-group-palette 的真实 blocker 缩到 descendant runtime pose 缺失；fresh live 证据确认 descRuntimeCount=1/2 但 descPoseHitCount=0，当前 child runtime 即使 live-read 也没有 palette)]`
   Notes: `本轮证据为 war3_upstream_live10/live13/quick14；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；当前下一步已收敛到补新的 descendant pose capture/publish 来源，而不是继续扩 consumer 猜测`

2. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 01:20:33.146 +08:00`
   LastHeartbeat: `2026-04-22 02:38:19.914 +08:00`
   ClosedAt: `2026-04-22 02:38:19.914 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已完整回退 over-strict runtime-model validator，保留 geoset-owner recovery，并把上游噪声从 no-pose 统计里压下一截；quick9 验证到 semanticCoreResolved=378, noPose=71, noRuntimeGroupPalette=441)]`
   Notes: `本轮证据为 war3_upstream_quick6/quick9/live3/live7；CURRENT_STATUS_2026_04_18.md 与 semantic_shadow_control_plane_status_2026_04_19.md 已回写；当前 primary blocker 已收敛到 child-runtime palette capture/linkage`

3. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/d3d9_war3_shadow.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 00:42:24.894 +08:00`
   LastHeartbeat: `2026-04-22 01:00:48.655 +08:00`
   ClosedAt: `2026-04-22 01:00:48.655 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic 动态阴影 bounds/cascade 修正已落地；safe host 路线下 ShadowFactor 从近乎发白恢复到明显可见，主画面保持非白模)]`
   Notes: `CURRENT_STATUS / semantic_shadow_control_plane_status / native_render_takeover_plan 已回写；关键证据为 war3_20260422_005514.png / war3_20260422_005631.png / war3_20260422_005801.png 与 00_55_12 / 00_56_30 / 00_58_00 perf report`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/native/*`, `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-22 00:27:21.531 +08:00`
   LastHeartbeat: `2026-04-22 00:32:00 +08:00`
   ClosedAt: `2026-04-22 00:32:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(撤下默认 full-scene native takeover，恢复 safe DXVK-host native shadow execute；三套命名场景重新通过 hot-frame 验收，默认路径白模已消失且最新阴影链仍可执行)]`
   Notes: `CURRENT_STATUS_2026_04_18.md / semantic_shadow_control_plane_status_2026_04_19.md / native_render_takeover_plan.md 已同步回写；本轮关键工件为 war3_20260422_002925.png / war3_20260422_003016.png / war3_20260422_003043.png 与 00_29_23 / 00_30_15 / 00_30_43 三份 perf report`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/d3d9_war3_hook.*`, `src/d3d9/war3/native/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-21 22:40:14.178 +08:00`
   LastHeartbeat: `2026-04-21 23:06:12 +08:00`
   ClosedAt: `2026-04-21 23:06:12 +08:00`
   Status: `closed`
   Result: `[正常关闭:(takeover-only native execute 已在 current DXVK host 内跑通；三套命名场景回归均已看到 nativeD3D9BackendExecutedDrawCount>0；blocker 已切到 late-inject/native-only 独立宿主与 perf 收口)]`
   Notes: `CURRENT_STATUS_2026_04_18.md / semantic_shadow_control_plane_status_2026_04_19.md / native_render_takeover_plan.md 已同步回写；本轮还升级了 AutoTest hot-frame 验收为“semantic hot frame + native execute”两段式`

1. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/native/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/platform/*`, `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-21 21:18:57 +08:00`
   LastHeartbeat: `2026-04-21 22:19:49 +08:00`
   ClosedAt: `2026-04-21 22:19:49 +08:00`
   Status: `closed`
   Result: `[正常关闭:(runtime-bridge warm gate 已实装并首次真实安装 native takeover；新 blocker 已收敛到 render-thread semantic scene 仍消费空 frame)]`
   Notes: `已多次编译通过；已用 isolated desktop control-plane 验证到 takeover 安装日志与 RenderScene hook 安装日志；CURRENT_STATUS_2026_04_18.md / semantic_shadow_control_plane_status_2026_04_19.md 已回写当前 blocker`

1. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/model/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 11:03:36.685 +08:00`
   LastHeartbeat: `2026-04-18 11:16:52.317 +08:00`
   ClosedAt: `2026-04-18 11:18:26.845 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了 one-more-hop 二级 candidate + stream1/aux0 并排样本，恢复了延迟最终帧抓图并确认 blocker 已收敛到 l1cHop remap/span contract)]`
   Notes: `CURRENT_STATUS_2026_04_18.md 已回写；本轮关键工件为 rb_aux_pair_probe_1776482012.json / rb_aux_manual_capture_1776482174.json；capture_final_frame 延迟后成功但尺寸仍为 1902x963，下一轮应继续解 0x6A414BA0 这一跳`

1. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/hooks/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 10:02:48.145 +08:00`
   LastHeartbeat: `2026-04-18 10:14:55.753 +08:00`
   ClosedAt: `2026-04-18 10:14:55.753 +08:00`
   Status: `closed`
   Result: `[正常关闭:(修正 aux stream 语义 + primitiveBaseIndex 止血，后台验证确认 dynIdx 恢复但 aux 头更像 float 权重流)]`
   Notes: `CURRENT_STATUS_2026_04_18.md 已回写；本轮 live 证据显示 geoIdx=3 从 baseIdx 垃圾值恢复到 baseIdx=0/dynIdx=36，但 dynGrpSrc 仍为 -1，下一轮应转向权重流/compact index 配对`

1. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 09:03:55.837 +08:00`
   LastHeartbeat: `2026-04-18 09:36:13.614 +08:00`
   ClosedAt: `2026-04-18 09:36:13.614 +08:00`
   Status: `closed`
   Result: `[正常关闭:(把 dispatch contract probe 挂到 live dispatch hook，后台验证确认 QueueTakeover 关闭且 blocker 缩到 layer/aux 字段语义)]`
   Notes: `CURRENT_STATUS_2026_04_18.md 已回写；最新后台证据为 rb_dispatch_gate_1776475765.json / rb_dispatch_live_1776475910.json；当前 authoritative probe 已不再依赖不会执行的 reimpl takeover path`

2. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 07:02:26.704 +08:00`
   LastHeartbeat: `2026-04-18 07:07:32.670 +08:00`
   ClosedAt: `2026-04-18 08:02:47.600 +08:00`
   Status: `closed`
   Result: `[异常关闭:(超过心跳窗口，接力线程接管)]`
   Notes: `08:02 接力线程发现该记录已超过心跳窗口，按协作规则关闭并接管当前轮`

3. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 06:01:35.280 +08:00`
   LastHeartbeat: `2026-04-18 06:19:05.425 +08:00`
   ClosedAt: `2026-04-18 06:19:05.425 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了 layerState contract + primary-stream aux 候选，完成两轮隔离桌面验证并确认 blocker 仍在 layer/aux authoritative contract)]`
   Notes: `CURRENT_STATUS 已回写；capture_final_frame 与 perf report 后台链本轮均恢复；但 dynGrpPrimary/dynamic-group override 仍未命中，单位阴影形状未转正`

4. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 04:02:20.406 +08:00`
   LastHeartbeat: `2026-04-18 04:02:20.406 +08:00`
   ClosedAt: `2026-04-18 05:03:16.213 +08:00`
   Status: `closed`
   Result: `[异常关闭:(超过心跳窗口，接力线程接管)]`
   Notes: `05:03 接力线程发现该记录长时间无心跳，按协作规则关闭并移交当前轮`

5. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 05:03:16.213 +08:00`
   LastHeartbeat: `2026-04-18 05:15:25.017 +08:00`
   ClosedAt: `2026-04-18 05:15:25.017 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了stream1子偏移扫描+vbIndexed fallback prune，并完成隔离桌面control-plane验证)]`
   Notes: `已编译通过；capture_final_frame 在隔离桌面下恢复成功；CURRENT_STATUS_2026_04_18.md 已回写“视觉 blocker 未解除、perf report 仍未翻新”`

1. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 08:02:47.600 +08:00`
   LastHeartbeat: `2026-04-18 08:26:58.737 +08:00`
   ClosedAt: `2026-04-18 08:29:10.872 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了 canonical layerState contract + strong-key fallback prune，完成隔离桌面验证并回写 CURRENT_STATUS)]`
   Notes: `本轮新增 state-view probe 与 fallback ownership/prune 两类落地；latest screenshot/report/artifact 已回写 CURRENT_STATUS_2026_04_18.md`

2. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 03:01:57.370 +08:00`
   LastHeartbeat: `2026-04-18 03:13:47.982 +08:00`
   ClosedAt: `2026-04-18 03:13:47.982 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了 subPrimitive slice 匹配 + dynamic-group override，验证 blocker 仍在 stream1/aux binding)]`
   Notes: `已两次编译通过，并完成 dynamic_shadow_pressure / low_pressure_static_reuse 隔离桌面验证，CURRENT_STATUS 已回写`

2. Thread: `rb-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-18 02:35:27.636 +08:00`
   LastHeartbeat: `2026-04-18 02:51:25.324 +08:00`
   ClosedAt: `2026-04-18 02:51:25.324 +08:00`
   Status: `closed`
   Result: `[正常关闭:(验证到 meshPoseCtx/primitiveBaseIndex blocker)]`
   Notes: `已编译+隔离桌面 AutoTest+control plane 验证，并回写 CURRENT_STATUS_2026_04_18.md`

6. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-22 22:40:05.516 +08:00`
   LastHeartbeat: `2026-04-22 22:40:05.516 +08:00`
   ClosedAt: `2026-04-22 22:40:05.516 +08:00`
   Status: `closed`
   Result: `[正常关闭:(补了 attachment raw/contract runtime sample + owner-identity/match 计数，并确认匿名 attachment rigid 不是 AttachedEffectInit/AttachModelToPoint 那一族 runtime)]`
   Notes: `fresh 工件为 codex_model_runtime_probe_20260422_223445.json / codex_model_runtime_probe_20260422_223931.json；当前 blocker 已收敛为 local-point/controller 主路径上的另一族匿名 runtime tree 仍未找到 owner/source 发布层`

7. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 03:42:26.422 +08:00`
   LastHeartbeat: `2026-04-23 03:50:49.001 +08:00`
   ClosedAt: `2026-04-23 03:50:49.001 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已先后 live 证伪“sourceObject 只写到 root runtime、没沿 runtime tree 传播”以及“漏了 direct 0x6F185250 source publication”两条假设；当前 3 条 anonymous attachment 仍全部 0 sourceObject，blocker 已更硬地收敛到另一族 runtime producer/caller)]`
   Goal: `[验证 sourceObject/sourceSpriteObject 是否只是没有沿 runtime tree 传播，目标是在不扩大 RE 面的前提下先证伪/证实 attachment 匿名仍属同族 producer]`
   Notes: `本轮先在 ModelInstanceRegistry 新增 propagateRuntimeSourceObjectLocked(...) 并跑 fresh 工件 codex_model_runtime_probe_source_object_propagation_20260423_034542.json；随后又在 Hook_CreateSpriteRuntime(0x6F185250) 直接补 source publication，再跑 fresh 工件 codex_model_runtime_probe_create_runtime_source_20260423_034828.json。最终 attachmentRigidCount=3、attachmentRigidCountWithSourceObject=0、contractAttachmentRigidCountWithSourceObject=0、attachmentRigidSourceObjectFromChild/Owner/RootRuntimeCount 全为 0，semanticCoreAttachmentRigidResolved 仍为 0；说明 anonymous attachment 不只是“没继承到 source”，而是根本不在当前 source-object producer family 里。IDA 本轮补了 0x6F185A10/0x6F185A40/0x6F1859D0/0x6F185A60 注释；下一轮必须直接围绕 fresh sample runtime trio (457945124 / 457944896 / 427038428) 上抬到真正的 producer/caller。`

8. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 03:52:23.481 +08:00`
   LastHeartbeat: `2026-04-23 03:58:58.069 +08:00`
   ClosedAt: `2026-04-23 03:58:58.069 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已在 chosen child-link 上 live 证伪“linkNode+0x0C 能直接解 owner/source”这条假设；overrideLocalPointChildSourceMetaIdentityHintWriteCount 仍为 0，anonymous attachment owner 继续收敛到 local-point/controller 更上游的 producer/caller)]`
   Goal: `[验证 child linkNode +0x0C 是否真的是 source meta，而不只是 tag；若能从 chosen child link 直接解出 owner/source，则立即并入 attachment rigid contract]`
   Notes: `本轮把 RuntimeChildLinkProbeRecord 扩成 linkNode+0x0C 双视角（兼容 tag，同时单独记 sourceMeta），并只在 TryResolveSourceObjectIdentity(...) 高置信成功时才统计命中；fresh 工件 codex_model_runtime_probe_child_source_meta_20260423_035849.json 显示 overrideLocalPointChildSourceMetaIdentityHintWriteCount=0、overrideLastChildSourceMetaPtr=0、attachmentRigidCount=3、contractAttachmentRigidCountWithSourceObject=0、semanticCoreAttachmentRigidResolved=0。说明 chosen child-link 自己并没有直接暴露可消费的 owner/source；下一轮必须继续围绕 fresh sample runtime trio (463319076 / 463318848 / 432412380) 上抬到真正的 producer/caller。`

9. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 04:01:36.720 +08:00`
   LastHeartbeat: `2026-04-23 04:48:09.808 +08:00`
   ClosedAt: `2026-04-23 04:48:09.808 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已新增 runtime create provenance + direct init fallback probe，并确认当前 anonymous attachment trio 不在 hot 0x6F12A3C0->0x6F12A5C0 create family，也不在 hot 0x6F130D90 direct-init family)]`
   Goal: `[直接上抬到 0x6F182300/0x6F1820C0 caller 与 0x6F0CB000->0x6F185B60->0x6F139360->0x6F138510 中层链，找到第一次同时持有 logical/source object + sprite/root runtime + context 的 producer]`
   Notes: `本轮先在 ModelInstanceRegistry 新增 runtime create provenance，并接入 Hook_PromoteRuntimeModel(0x6F12A5C0)；fresh 工件 codex_model_runtime_probe_runtime_create_provenance_20260423_044138.json 显示 runtimeModelCreateCount=18、runtimeModelCreateCallerOtherCount=18、lastRuntimeModelCreateCallerRva=0x12A3E8。IDA 已确认 0x6F12A3C0=ModelHandle_TryResolveRuntimeModel、0x6F12A5C0=CModelData_PromoteToRuntimeModel、0x6F130D90=CModelComplex_CopyFromModelData、0x6F131F60=CModelComplex_BuildChildRuntimeModelLinks。随后又补上 Hook_RuntimeInitFromModelData(0x6F130D90) fallback，fresh 工件 codex_model_runtime_probe_runtime_init_fallback_20260423_044711.json 显示 runtimeModelInitCopyCount=0、runtimeModelInitCopyPublishedFallbackCount=0，而 attachmentRigidSample0/1 与 contract sample0/1 的 root/owner/child runtime create caller 仍全为 0。当前 blocker 已更硬地收敛为：anonymous attachment trio 来自另一族 producer/construction family，或来自当前 hot-shadow 观察窗之前的更早创建阶段；下一轮必须围绕 fresh sample trio (456306724 / 456306496 / 425400028) 继续上抬到更早 producer/publisher。`

10. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 04:53:05.363 +08:00`
   LastHeartbeat: `2026-04-23 05:04:59.956 +08:00`
   ClosedAt: `2026-04-23 05:04:59.956 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已新增 CModel/CModelComplex ctor provenance probe，并确认当前 anonymous attachment trio 也不在 hot constructor family；blocker 继续收紧到更早生命周期/更早安装点)]`
   Goal: `[继续只做 owner producer 逆向，围绕 anonymous attachment trio (456306724 / 456306496 / 425400028) 上抬到更早的 CModel/CModelComplex 构造/发布家族，验证它们是否在当前 hot-shadow 观察窗内经过另一条 constructor/provenance family]`
   Notes: `本轮新增 Hook_RuntimeModelPlainCtor(0x6F121880) / Hook_RuntimeModelComplexCtor(0x6F1219C0)，并把 ctor counters 接入 control-plane；fresh 工件 codex_model_runtime_probe_runtime_ctor_family_20260423_0500.json 与 codex_model_runtime_probe_runtime_ctor_family_retry_20260423_0506.json 一致显示 runtimeModelCtorCount=513、runtimeModelPlainCtorCount=513、runtimeModelComplexCtorCount=0、runtimeModelCtorCallerPromoteCount=18、runtimeModelCtorCallerOtherCount=495，但 attachmentRigidSample0/1 与 contract sample0/1 的 root/owner/child runtime create caller 仍全为 0。两轮 hot-shadow 都以 wait_until stalled 收尾，但 shadowRuntimeSummary 已稳定给出 ctor 结论；下一轮必须转去验证 anonymous trio 是否在 hook 安装前就已完成创建/发布，而不是继续细挖当前 hot ctor family。`

11. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/platform/*`, `src/d3d9/war3/model/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 05:06:55.500 +08:00`
   LastHeartbeat: `2026-04-23 05:10:13.322 +08:00`
   ClosedAt: `2026-04-23 05:10:13.322 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已确认 war3_model_hook 之前安装过晚；把 model hook 前移到 bootstrap 后，anonymous trio 首次恢复了 owner-runtime provenance，但 root/child 仍未恢复，identity 仍未闭环)]`
   Goal: `[验证 war3_model_hook 的真实安装时机，判断 anonymous trio 是否可能早于 hook 安装完成；若“安装过晚”成立，则下一轮直接把 provenance probe 前移到更早 bootstrap/lifecycle 阶段]`
   Notes: `本轮先静态对齐了调用顺序：war3_model_hook::Init(...) 原本只在 ActivateWar3Runtime -> InstallGameHooks 里调用，确实晚于 bootstrap。随后在 d3d9_war3_hook.cpp 的 War3Hook::InstallHooks(...) 中提前执行 war3::model::Init(gameInfo.base)，再跑 fresh 工件 codex_model_runtime_probe_bootstrap_model_init_20260423_0512.json。结果显示 attachmentRigidOwnerRuntimeCreateCallerKnownCount=3、contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount=3，sample0 的 owner runtime create caller 首次恢复为 0x12A3E8 (ModelHandle_TryResolveRuntimeModel family)；但 root/child runtime create caller 仍为 0，contractAttachmentRigidCountWithAnyIdentity 仍为 0，semanticCoreAttachmentRigidResolved 仍为 0。说明 late-install 的确吃掉过 provenance，但当前真正缺的 identity 闭环还要继续沿 owner runtime -> source/identity 发布层往上追。`

12. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-23 05:16:03.377 +08:00`
   LastHeartbeat: `2026-04-23 18:47:00.000 +08:00`
   ClosedAt: `2026-04-23 18:47:00.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(owner identity/source 已经通过 CAttachedEffect_Init -> AttachModelToPoint parent runtime 回灌，reverse/contract identity gate 通过；blocker 已转为 child runtime resource/pose 不完整)]`
   Goal: `[沿 ownerRuntimeCreateCallerRva=0x12A3E8(ModelHandle_TryResolveRuntimeModel family) 继续上抬到 handle/modelData/source object 的 identity 发布层，目标是在同一时刻拿到 logical/source object + runtime handle/modelData + frame-hot context，并为 anonymous attachment trio 找到 authoritative owner producer]`
   Notes: `后续 fresh 工件 codex_model_runtime_probe_attach_owner_publish_registry_20260423_1846.json / codex_model_runtime_probe_attach_owner_publish_registry_state_20260423_1847.json 已确认 attachmentRigidCountWithAnyIdentity=2、contractAttachmentRigidCountWithAnyIdentity=2；当前下一步固定为补 child runtime modelResource/pose 并把 semanticCoreAttachmentRigidResolved 抬起。`

13. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 02:30:15.385 +08:00`
   LastHeartbeat: `2026-04-24 02:50:38.162 +08:00`
   ClosedAt: `2026-04-24 02:50:38.162 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic core 已通过 resource-owner world-pose rigid fallback 稳定产出 2 个 semantic-only draw packet；objectFallbackDrawCount=0；但 attachmentRigidResolved/skinned/upperLayerEmitted 仍未抬起)]`
   Goal: `[进入 Phase 3：补 anonymous attachment child runtime 的 modelResource/pose 消费链，优先验证 childSprite->Model 是否能作为 attachment internal child runtime 的 semantic fallback，把 semanticCoreAttachmentRigidResolved 从 0 抬起]`
   Notes: `本轮测试全部 useIsolatedDesktop=true + windowed=true，且收尾无 War3 残留。fresh 工件：codex_model_runtime_probe_world_pose_rigid_fallback_20260424_0302.json、codex_dynamic_shadow_pressure_world_pose_rigid_fallback_20260424_0307.json、codex_low_pressure_static_reuse_world_pose_rigid_fallback_20260424_0312.json。核心结果：三套场景均 semanticCoreResolved=2 / semanticCoreRigidResolved=2 / semanticCoreSubmittedDrawCount=2 / semanticCoreSkippedNoPose=0 / objectFallbackDrawCount=0；下一轮继续补 resourceRuntimeOwner ±0xA0 diagnostics，并把 world-pose fallback 升级为 skinned/attachment rigid contract。`

14. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 03:02:36.592 +08:00`
   LastHeartbeat: `2026-04-24 03:49:11.719 +08:00`
   ClosedAt: `2026-04-24 03:49:11.719 +08:00`
   Status: `closed`
   Result: `[正常关闭:(resourceRuntimeOwner +0xA0 已确认只是 world-pose/source-context 观察面，不是逻辑 owner；sourceObject+0x100 registry/runtime 命中已证伪为 context/矩阵块误命中；sprite-frame source 弱身份发布已收紧为 unit/jHandle/rawcode 才 authoritative)]`
   Goal: `[把 resourceRuntimeOwner <-> spriteFrameRuntime(+0xA0) 的 source/world-pose 关系正式回灌到 runtime contract，并继续追 matrix palette 发布点，目标把当前 rigid fallback 升级为 skinned/attachment rigid]`
   Notes: `继续要求所有 AutoTest 使用 useIsolatedDesktop=true + windowed=true；本轮禁止回退 VB/IB snapshot/freeze。fresh 工件：codex_model_runtime_probe_source_object_deep_probe_20260424_0330.json / codex_model_runtime_probe_deep_identity_merge_20260424_0338.json / codex_model_runtime_probe_source_field_merge_20260424_0348.json / codex_model_runtime_probe_source_logical_gate_20260424_0400.json。最终指标：semanticCoreRigidResolved=2、semanticCoreSubmittedDrawCount=2、objectFallbackDrawCount=0、semanticCoreAttachmentRigidResolved=0、spriteFrameSourceResolvedIdentityCount=0、lastSpriteFrameSourceObjectRuntimeFieldOffset=0x100、lastSpriteFrameSourceObjectRegistryFieldOffset=0x100、lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform=1、lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount=0。下一轮不要再把 sprite-frame a3/sourceObject 当 owner；直接追调用 CSpriteUber_PreRenderAndUpdatePosePalette_* 的更高层 producer，并继续追 0x6F12F7E0 后的 matrix palette 发布点。`

15. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 04:14:17.783 +08:00`
   LastHeartbeat: `2026-04-24 04:51:56.953 +08:00`
   ClosedAt: `2026-04-24 04:51:56.953 +08:00`
   Status: `closed`
   Result: `[正常关闭:(resource-owner explicit rigid path 已正式计数并通过 model_runtime_probe 阶段 gate；attachment child lineage 已收敛为 noUniqueChild，下一轮应在 0x6F131F60 build-time 直接发布 childRuntime->childModelData/modelResource)]`
   Goal: `[执行 8 小时无人值守推进计划；本轮主线固定为 attachment child runtime modelResource/pose/matrixPalette contract，不重开已证伪 owner/source 路线；所有 War3 runtime 测试必须 isolated desktop + windowed，禁止全屏，禁止 snapshot/freeze/VB/IB 回退]`
   Notes: `fresh 工件 codex_model_runtime_probe_child_lineage_observed_links_20260424_0550.json；ninja -C build32 -j1 与 python -m py_compile AutoTest/war3_autotest_mcp.py 通过。最新指标：semanticCoreExplicitResourceOwnerRigidResolved=2、semanticCoreSubmittedDrawCount=2、objectFallbackDrawCount=0、attachmentChildLineageBootstrapAttemptCount=4、attachmentChildLineageBootstrapSuccessCount=0、attachmentChildLineageBootstrapMissNoModelDataLinksCount=0、attachmentChildLineageBootstrapMissNoUniqueChildCount=4。已证伪：childSprite->Model fallback、全父模型唯一 child、runtime bucket ordinal、root link + owner modelData cross bootstrap、observed root links bootstrap。下一轮只追 0x6F131F60 的 build-time v9[2]=sub_6F12A5C0(v7[2])，直接记录 childRuntimeModelPtr->childModelDataPtr/modelResourcePtr；若仍不命中，再 hook 0x6F12FDC0/0x6F12FF50。`

16. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 04:55:56.290 +08:00`
   LastHeartbeat: `2026-04-24 06:43:08.114 +08:00`
   ClosedAt: `2026-04-24 06:43:08.114 +08:00`
   Status: `closed`
   Result: `[正常关闭:(AttachModelToPoint child runtime promotion + contract resource-store rebuild + ShadowRendererCore attachment supplemental path 已打通；model_runtime_probe 现在 semanticCoreAttachmentRigidResolved=96、semanticCoreResolved=98、semanticCoreSubmittedDrawCount=98、objectFallbackDrawCount=0；dynamic_shadow_pressure smoke 与 low_pressure_static_reuse smoke 均 fallback=0)]`
   Goal: `[继续 Phase 3：只追 0x6F131F60 build-time child runtime producer，直接发布 childRuntimeModelPtr -> childModelDataPtr/modelResourcePtr，目标抬起 attachmentRigidChildRuntimeModelResourceKnownCount / contractAttachmentRigidChildRuntimeModelResourceKnownCount]`
   Notes: `本轮没有重开 WorldObjectList+0x14、a3/sourceObject、sourceObject+0x100、childSprite->Model alias；所有 War3 runtime 测试均 isolated desktop + windowed，禁止全屏，收尾无残留 War3 进程。0x6F131F60 当前 hook 层的 readable child modelData list 仍不可用；实际突破来自 AttachModelToPoint 真实 child runtime promotion。fresh 工件：codex_model_runtime_probe_attachment_stats_fixed_20260424_0905.json、codex_dynamic_shadow_pressure_attachment_rigid_smoke_20260424_0915.json、codex_low_pressure_static_reuse_attachment_rigid_smoke_20260424_0925.json。下一轮进入 Phase 4：正式 gate 区分 attachment rigid 已通与 skinned 未完成，继续抬 semanticCoreSkinnedResolved / upperLayerEmitted，并观察 96 个 attachment supplemental draw 的性能与 cache/dedup。`

17. Thread: `native-main-codex`
   Role: 主线程
   Scope: `AutoTest/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 06:49:00.257 +08:00`
   LastHeartbeat: `2026-04-24 07:13:56.709 +08:00`
   ClosedAt: `2026-04-24 07:13:56.709 +08:00`
   Status: `closed`
   Result: `[正常关闭:(AutoTest hot-shadow gate 已能区分 attachment rigid semantic 成功与 skinned/native 未签收；dynamic_shadow_pressure strict gate 返回 hot-shadow-skinned-pending，model_runtime_probe hot gate 等到 semanticCoreAttachmentRigidResolved=96)]`
   Goal: `[进入 Phase 4 前半：修正正式 hot-shadow gate 对 attachment rigid semantic 成功与 skinned/native 未完成的区分；所有 War3 runtime 测试必须 isolated desktop + windowed，禁止全屏，禁止 snapshot/freeze/VB/IB 回退]`
   Notes: `本轮将 low_pressure_static_reuse / dynamic_shadow_pressure / semantic_cost_probe preset 默认改为 windowed=true；新增 wait_for_hot_shadow_frame summary-poll 模式与 attachment-rigid 阶段分类。fresh 工件：codex_dynamic_shadow_pressure_ready_poll_attachment_20260424_0718.json、codex_dynamic_shadow_pressure_summary_only_gate_20260424_0805.json、codex_model_runtime_probe_attachment_gate_after_autotest_fix_20260424_0818.json。关键指标：dynamic strict stage=hot-shadow-skinned-pending、semanticCoreAttachmentRigidResolved=96、semanticCoreSubmittedDrawCount=98、objectFallbackDrawCount=0、semanticCoreSkinnedResolved=0、upperLayerEmitted=0；model_runtime_probe hotOk=true 且 attachmentAccepted=true。下一轮继续追 semanticCoreSkinnedResolved / upperLayerEmitted / nativeD3D9BackendExecutedDrawCount。`

18. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 07:15:24.563 +08:00`
   LastHeartbeat: `2026-04-24 07:24:23.113 +08:00`
   ClosedAt: `2026-04-24 07:24:23.113 +08:00`
   Status: `closed`
   Result: `[正常关闭:(已新增 skinned candidate 分层 counters，并确认 skinned=0 的当前原因不是 group-palette 算法失败，而是 root/unit skinned renderable 没进入当前 manifest；当前 98 个候选全是 skinned resource，其中 96 个被 attachment rigid contract 合法接走)]`
   Goal: `[继续 Phase 4：定位 semanticCoreSkinnedResolved=0 / upperLayerEmitted=0 的具体 gate，是 manifest/resource/pose/palette 缺口还是 emission gate 阻断；不回退 snapshot/freeze/VB/IB]`
   Notes: `fresh 工件 codex_model_runtime_probe_skinned_candidate_counters_20260424_0725.json。关键指标：semanticCoreSkinnedCandidateCount=98、semanticCoreSkinnedCandidatePoseReadyCount=0、semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount=0、semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount=96、semanticCoreSkinnedResolved=0、semanticCoreAttachmentRigidResolved=96、matrixPaletteCount=44、shadowRuntimeModelCount=226。下一轮只查 root/unit skinned manifest 发布：当前 manifest unitCount=0、recordsWithRuntimeModel=0、recordsWithModelResource=0，semanticSceneSubmitted=0。`

19. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/render/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 07:27:20.728 +08:00`
   LastHeartbeat: `2026-04-24 09:19:17.741 +08:00`
   ClosedAt: `2026-04-24 09:19:17.741 +08:00`
   Status: `closed`
   Result: `[正常关闭:(root/unit supplemented manifest 已抬起；model_runtime_probe 中 unitCount=96、recordsWithRuntimeModel=96、recordsWithModelResource=96，并且 semanticCoreSkinnedResolved 首次从 0 抬到 31；objectFallbackDrawCount 仍为 0)]`
   Goal: `[继续 Phase 4：只查 root/unit skinned manifest 发布缺口，确认 unit-like capture 被 semantic scene 接管后是否缺少等价 manifest record；若缺 record 则补发布/诊断，不回退 snapshot/freeze/VB/IB]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_model_runtime_probe_root_unit_supplement_20260424_0919.json。关键指标：unitCount=96、recordsWithRuntimeModel=96、recordsWithModelResource=96、rootUnitSupplementAppended=96、semanticCoreSourceUnitCount=61、semanticCoreSkinnedResolved=31、semanticCoreSkinnedCandidatePoseReadyCount=59、semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount=31、semanticCoreAttachmentRigidResolved=213、semanticCoreSubmittedDrawCount=39、objectFallbackDrawCount=0。当前 blocker 已转为 semantic build freshness/perf：supplemented bundle + attachment supplemental workload 会扩到较大 build，isolated desktop control-plane observe 下 buildInProgress/pending 持续较久；下一轮只做 build 去重/限流和 freshness gate，不再重开 owner/source 路线。`

20. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 09:20:10.000 +08:00`
   LastHeartbeat: `2026-04-24 09:30:00.000 +08:00`
   ClosedAt: `2026-04-24 09:30:00.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic build workload 已阶段感知限流；root/unit manifest 存在时 attachment supplemental cap=16，buildRecordCount 从 603 降到 26/98，且 skinned path 仍可抬起到 semanticCoreSkinnedResolved=4)]`
   Goal: `[Phase 4 freshness/perf 收口：在保留 root/unit skinnedResolved>0 与 attachment rigid 的前提下，限制 semantic build 中 attachment supplemental/root-unit supplemental 重复膨胀，目标让 model_runtime_probe 的 semanticCoreFrameFresh=true 或至少 buildRecordCount 不再异常扩张]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_model_runtime_probe_semantic_build_limited_20260424_0930.json。关键指标：限流前 buildRecordCount=603；限流后 buildRecordCount=26，后续 root/unit buildRecordCount=98；semanticCoreAttachmentRigidSupplementalResolvedCount=16；semanticCoreSkinnedResolved=4；semanticCoreSkinnedCandidatePoseReadyCount=4；semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount=4；semanticCoreSubmittedDrawCount=42；objectFallbackDrawCount=0。当前 blocker 转为 build ordering / bounded multi-chunk observe：数据链已够用，但 semanticCoreFrameFresh 还没稳定为 true，upperLayerEmitted 仍为 0。`

21. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/render/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 09:35:15.093 +08:00`
   LastHeartbeat: `2026-04-24 10:05:41.000 +08:00`
   ClosedAt: `2026-04-24 10:05:41.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(Phase 4 freshness gate 已突破；root/unit 小闭环可 build 到 semanticCoreFrameFresh=true，core/native staging 可提交 24~32 个 semantic draw packet，objectFallbackDrawCount=0；下一 blocker 转为真实 render scene pass 消费 fresh semantic frame)]`
   Goal: `[Phase 4 freshness/perf 收口：只做 build ordering / bounded multi-chunk observe，让 buildRecordCount<=128 的 root/unit semantic build 能在 control-plane refresh 中安全追到 semanticCoreFrameFresh=true；继续保持 isolated desktop + windowed，禁止 full-screen，禁止 snapshot/freeze/VB/IB 回退]`
   Notes: `fresh 工件：codex_model_runtime_probe_root_cap16_samples_20260424_0956.json、codex_model_runtime_probe_scene_catchup_samples_20260424_1002.json、codex_dynamic_shadow_pressure_scene_catchup_smoke_20260424_1005.json。关键指标：semanticCoreFrameFresh=true、semanticCoreSourceUnitCount=16、semanticCoreSkinnedResolved=2(另一次 smoke 为0，仍需稳定)、semanticCoreAttachmentRigidResolved=20~30、semanticCoreSubmittedDrawCount=24~32、nativeD3D9BackendSubmittedDrawCount=31~32、nativeD3D9BackendExecutedDrawCount=0、semanticSceneSubmitted=4、objectFallbackDrawCount=0。已处理黑屏/雪花屏风险：本轮开始清掉 videoRestorePending，所有测试 enforce_video_baseline=false + windowed + isolated desktop，收尾 videoRestorePending=false 且无残留 War3。下一轮只追 render-thread scene consumption/frame advance，不再重开 owner/resource/pose 路线。`

22. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/tools/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 10:09:08.877 +08:00`
   LastHeartbeat: `2026-04-24 10:30:40.619 +08:00`
   ClosedAt: `2026-04-24 10:30:40.619 +08:00`
   Status: `closed`
   Result: `[正常关闭:(scene-consumption gate 已落地并验证；semantic core latest frame 与 DXVK scene pass 消费 revision 已可分离观测，当前 blocker 确认为隔离桌面尾帧无真实 render scene pass/frame advance，而不是 owner/resource/pose 数据缺口)]`
   Goal: `[Phase 4 render-thread scene consumption：确认真实 render tick 是否消费 fresh semantic frame；若隔离桌面尾帧无 render advance，则修正 AutoTest gate；若有 render advance 但 semanticSceneSubmitted 仍旧值，则查 War3ShouldSubmitSemanticPacket / unitsOnly/objectKind 过滤]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_scene_consumption_poll_20260424_1020.json、AutoTest/artifacts/codex_scene_consumed_gate_20260424_1029.json。关键指标：semanticSceneStatsPublishCount=1、semanticScenePopulateAttemptCount=1、semanticSceneLastSourcePublishRevision=37、semanticCoreSourcePublishRevision=38、semanticScenePublishRevisionLag=1、semanticSceneLastSubmittedDrawCount=4、frameIndex 停在 887/889、render.isInGame=false。新增 requireSemanticSceneConsumed gate 与 AutoTest core-fresh-waiting-render-scene 分类；ninja -C build32 -j1 与 python -m py_compile AutoTest/war3_autotest_mcp.py 通过。所有本轮手动 runtime 验证均 isolated desktop + windowed + enforce_video_baseline=false；收尾 videoRestorePending=false，无残留 War3。下一轮主线：修 wait_for_game_ready/hot-shadow 对 isInGame=false + frame stalled 的处理，必要时增加触发/等待真实 frame advance 的能力；不要重开 owner/resource/pose 路线。`

23. Thread: `native-main-codex`
   Role: 主线程
   Scope: `AutoTest/*`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 10:34:39.161 +08:00`
   LastHeartbeat: `2026-04-24 10:52:35.002 +08:00`
   ClosedAt: `2026-04-24 10:52:35.002 +08:00`
   Status: `closed`
   Result: `[正常关闭:(AutoTest hot-shadow gate 已能把 core 有 packet 但 render scene 未消费最新 revision 的情况稳定签出为 hot-shadow-render-scene-pending；dynamic_shadow_pressure strict visual gate 复现 runtimeRenderTailStalled=true + sceneLag=1，收尾无残留 War3 且 videoRestorePending=false)]`
   Goal: `[Phase 4 AutoTest gate 收口：修正 wait_for_game_ready/hot-shadow 对 render.isInGame=false + frame stalled + core fresh/scene stale 的阶段分类；visual gate 必须要求 scene consumed，model_runtime_probe 保持 core-contract 诊断通过路径；测试继续 isolated desktop + windowed，禁止全屏和视频基线改写]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_hot_gate_direct_probe_20260424_1044.json、AutoTest/artifacts/codex_hot_gate_dynamic_visual_probe_20260424_1058.json、AutoTest/artifacts/codex_run_quick_dynamic_scene_pending_20260424_1102.json。关键指标：model_runtime_probe direct gate hotOk=true、semanticCoreSubmittedDrawCount=5、sceneFresh=true；dynamic strict gate stage=hot-shadow-render-scene-pending、semanticShadowPhase=core-packets-waiting-render-scene、runtimeRenderTailStalled=true、semanticScenePublishRevisionLag=1、semanticCoreSubmittedDrawCount=1、semanticSceneLastSubmittedDrawCount=4。ninja -C build32 -j1 与 python -m py_compile AutoTest/war3_autotest_mcp.py 通过。下一轮只追安全 frame advance / render scene consumption；不要重开 owner/resource/pose/palette。`

24. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/hooks/war3_hook_render.cpp`, `src/d3d9/war3/platform/war3_runtime_bootstrap.*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/shadow/*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-24 14:37:53.173 +08:00`
   LastHeartbeat: `2026-04-24 19:16:59.205 +08:00`
   ClosedAt: `2026-04-24 19:16:59.205 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic skinned dynamic shadow 三场景首轮签收：dynamic_shadow_pressure / model_runtime_probe / low_pressure_static_reuse 均通过；semanticCoreSkinnedResolved=28~56，semanticSceneSubmittedSkinned=37~53，nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount=27~28，objectFallbackDrawCount=0)]`
   Goal: `[继续 Phase 4：让 Stage11/native semantic execute 消费 fresh supplemented frame，并把 native executed / DXVK scene pending / visual gate 分开验收；所有 War3 测试继续 isolated desktop + windowed + enforceVideoBaseline=false，禁止全屏，禁止 snapshot/freeze/VB/IB 回退]`
   Notes: `接续 35.11 / 9.11.19：上游 owner/resource/pose/palette 已不再是主 blocker；native D3D9 backend 已实际成功执行 semantic draws。当前只追两个点：1) Stage11 准备/执行是否吃到 fresh supplemented frame；2) 若 DXVK validation scene 仍旧 revision，则报告为 validation-host tail-frame 限制，不误判数据链失败。18:21 续跑：9.11.20 已证明 semantic rigid/native execute floor 稳定，下一步只追 TryResolveBestPoseForRenderable / resource-runtime-tree / runtime group palette 让 final ShadowSubmissionFrame 带 skinned draw。19:02 续跑：dynamic_shadow_pressure 已通过 semantic skinned strict gate，开始跑 model_runtime_probe / low_pressure_static_reuse 横向基线。19:16 收尾：fresh 工件 codex_model_runtime_probe_after_skinned_gate_20260424_190307.json、codex_low_pressure_static_reuse_tail_accept_20260424_190931.json、codex_dynamic_shadow_pressure_final_semantic_skinned_20260424_191212.json；当前 blocker 转为 isolated desktop + windowed perf report frameCount/avgFps 仍为 0，下一轮先修 FPS 采样，再做 palette/build dedupe 性能优化。`

25. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/core/war3_internal_test_config.h`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-24 19:19:00.000 +08:00`
   LastHeartbeat: `2026-04-24 19:28:09.452 +08:00`
   ClosedAt: `2026-04-24 19:28:09.452 +08:00`
   Status: `closed`
   Result: `[正常关闭:(手动进图白屏/未响应止血：semantic core validation、DXVK scene submission、bootstrap catchup、EndFrame flush、legacy-unit bypass、native semantic validation execute 已改为默认关闭；普通 smoke ready=control-plane elapsedSec=17.492，最新日志无 SemanticCore/SemanticShadow/NativeSemantic 刷屏)]`
   Goal: `[解释并止血普通游戏路径误触实验 semantic shadow consumer 的回归；默认路径恢复稳定，后续实验必须显式开启，不再污染手动游玩]`
   Notes: `用户反馈普通进图从几秒变一分钟、进图全白并未响应。根因判断：上一轮为 DXVK validation host 签收而默认开启 kShadowSemanticCoreSceneSubmissionEnabled / BootstrapCatchup / EndFrameFlush / NativeRendererHostExecuteValidation / NativeSemanticShadowWorldStageValidation，导致普通手动路径也持续 build semantic frame 并在 Stage11/BeforeUi/EndFrame 执行实验提交。止血补丁只关闭消费/提交/执行默认开关，保留上游数据采集 hook；fresh 工件 AutoTest/artifacts/codex_semantic_default_off_smoke_20260424_192626.json。`

26. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/core/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/hooks/war3_hook_render.cpp`, `src/d3d9/d3d9_device.cpp`, `AutoTest/*`, `docs/plan/automation_exchange/*`
   StartedAt: `2026-04-24 19:45:14 +08:00`
   LastHeartbeat: `2026-04-25 02:43:06 +08:00`
   ClosedAt: `2026-04-25 02:43:06 +08:00`
   Status: `closed`
   Result: `[正常关闭:(runtime profile 分块关闭开关已补齐；新增 semantic.data，上层数据链可独立关闭；render 别名可关闭 hook.render/render.queue/shadow/postfx/ssao/aa；ninja -C build32 -j4 与 AutoTest py_compile 通过)]`
   Goal: `[Phase 4 visual validation：把 semantic consumer 收成默认安全、显式预览开启的路径，然后用 光影测试低压.w3x 在 isolated desktop + windowed 下截图验收；禁止全屏，禁止 snapshot/freeze/VB/IB 回退]`
   Notes: `用户要求先把渲染层所有干涉关闭并补上上层数据层 config。已新增 DXVK_WAR3_DISABLE=semantic.data 与 render 别名；推荐按 dxvk_only -> render,semantic.data -> render -> shadow -> semantic.data -> full_default 顺序测试。当前不重开 IDA，不跑全屏；后续 runtime 测试仍必须 isolated desktop + windowed。`

27. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/war3/model/war3_model_hook.cpp`, `src/d3d9/war3/render/war3_renderer.cpp`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/platform/war3_runtime_bootstrap.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 03:07:38 +08:00`
   LastHeartbeat: `2026-04-25 03:07:38 +08:00`
   ClosedAt: `2026-04-25 03:07:38 +08:00`
   Status: `closed`
   Result: `[正常关闭:(用户实测已确认卡顿源头在上层 semantic data 链；本轮新增 semantic model/pose/attachment/registry/contract/consumer 编译期二分开关，并构建为旧渲染层开启、semantic 总开关开启、只关闭 model hook 包的第一刀测试版本)]`
   Goal: `[定位 semantic data 子链性能杀手：不再优先怀疑旧渲染层；先测 runtime model hook 包，再按 pose/attachment/frame registry/contract capture/consumer 顺序二分]`
   Notes: `当前配置：kWar3RuntimeConfigDisableRenderInterference=false，kWar3RuntimeConfigDisableSemanticData=false，kWar3RuntimeConfigDisableSemanticModelHooks=true，其余 semantic 子开关为 false。ninja -C build32 -j4 通过(no work to do)。下一步用户直接测当前 DLL：若恢复到约 100 FPS，继续拆 war3_model_hook 内 model/resource、pose、attachment；若仍未响应，改测 SemanticFrameRegistries / SemanticContractCapture / SemanticConsumer。`

28. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/core/*`, `src/d3d9/war3/model/*`, `src/d3d9/war3/render/*`, `src/d3d9/war3/shadow/*`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/tools/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 03:53:06 +08:00`
   LastHeartbeat: `2026-04-25 04:12:26 +08:00`
   ClosedAt: `2026-04-25 04:12:26 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic producer/consumer 半开启保护已落地；DisableSemanticModelHooks=true 时 frame registry/contract/consumer 有效态自动关闭，control-plane summary 暴露 semanticBuildSkippedReason=2，smoke 60 秒内 frameIndex=4020 且没有两帧后未响应)]`
   Goal: `[执行 War3 Semantic Shadow 夜间自动迭代计划：先修 semantic 子模块半开启导致的 build storm/缺数据空转，再加 semantic data perf counters，随后进入矩阵定位与热点优化；测试必须 isolated desktop + windowed，禁止全屏，禁止旧 snapshot/freeze 回主路径]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_semantic_dependency_guard_smoke_20260425.json；ninja -C build32 -j4、python -m py_compile AutoTest/war3_autotest_mcp.py、git diff --check 通过/仅 CRLF 提示。smoke 关键值：readyElapsedSec=1.447、render.isInGame=true、frameIndex=4020、semanticModelProducerEnabled=0、semanticFrameRegistriesEnabled=0、semanticContractCaptureEnabled=0、semanticConsumerEnabled=0、semanticBuildSkippedReason=2、semanticConsumerBuildCalls=0。下一轮精确入口：打开 model/resource 基础 hook，但继续关闭 pose/attachment/frame registry/contract/consumer，判断基础 model hook 是否就是性能杀手。`

29. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/model/war3_model_resource_cache.*`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `src/d3d9/war3/tools/war3_control_plane.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 04:12:26 +08:00`
   LastHeartbeat: `2026-04-25 06:20:58.607 +08:00`
   ClosedAt: `2026-04-25 06:20:58.607 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic consumer 第一轮性能热点已收口；runtime owner 全表扫描改为 O(1) 索引，attachment rigid 空转扫描加 cheap precheck，runtimeRoots 改可信 registry 指针收集；dynamic_shadow_pressure core build 从约 676ms 级别降至 143us，objectFallbackDrawCount=0)]`
   Goal: `[执行 War3 Semantic Shadow 夜间计划 Phase 3：修 semantic.data/consumer build storm 与热点，确保上层 semantic-only skinned draw 不靠旧 fallback，继续 isolated desktop + windowed 验证]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_semantic_consumer_preview_pose_subphase_smoke_20260425.json、codex_semantic_consumer_preview_runtime_owner_index_smoke_20260425.json、codex_low_pressure_attachment_precheck_20260425.json、codex_low_pressure_trusted_runtime_roots_20260425.json、codex_dynamic_shadow_pressure_trusted_runtime_roots_20260425.json。关键值：owner scan 修复后 model_runtime_probe semanticCoreBuildDurationUs=6975、fallback=0；attachment precheck 后低压图 semanticCoreBuildDurationUs=53266、semanticCoreSubmittedDrawCount=146、fallback=0；trusted runtime roots 后 dynamic_shadow_pressure ok=true、semanticCoreBuildDurationUs=143、semanticCoreSlowestRecordResolveUs=25、semanticCoreSkinnedResolved=30、semanticSceneSubmitted=30、fallback=0。低压图最新样本单条解析已压平，但 isolated desktop 尾帧 buildInProgress=true/currentIndex=64/141，下一轮精确入口：semantic build tail-frame drain / single-flight、contract capture skip counter、FPS report 采样修复；不要重开 owner/resource/palette 逆向。`

30. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 06:30:16.076 +08:00`
   LastHeartbeat: `2026-04-25 06:40:33.450 +08:00`
   ClosedAt: `2026-04-25 06:40:33.450 +08:00`
   Status: `closed`
   Result: `[正常关闭:(bounded control-plane drain 已修复低压图 tail-frame buildInProgress 卡住；model_runtime_probe / 光影测试低压 / dynamic_shadow_pressure 三场景均 ok=true，semantic skinned scene submission >0，control-plane objectFallbackDrawCount=0；AutoTest 增加 fpsSampleReliable 避免隔离桌面 1-2 frame perf report 误判)]`
   Goal: `[Phase 3/4 tail-frame drain：低压图单条解析已压平但 chunked semantic build 在 isolated desktop 尾帧停在 currentIndex=64/141；本轮只修 build single-flight / bounded control-plane drain / FPS 采样，不重开 owner/resource/palette 逆向]`
   Notes: `fresh 工件：AutoTest/artifacts/screenshots/war3_20260425_063412.png + E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_34_15.html；AutoTest/artifacts/screenshots/war3_20260425_063521.png + E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_35_20.html；AutoTest/artifacts/screenshots/war3_20260425_063612.png + E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_06_36_15.html。关键值：低压图 ready 时 build 46/138，hot-shadow 后 semanticCoreBuildInProgress=false、semanticCoreSubmittedDrawCount=143、semanticCoreSkinnedResolved=137、semanticSceneSubmittedSkinned=157、objectFallbackDrawCount=0；dynamic_shadow_pressure semanticCoreBuildDurationUs=115、skinned=30、sceneSkinned=30、fallback=0。ninja -C build32 -j4、python -m py_compile AutoTest/war3_autotest_mcp.py 通过；无残留 War3 进程；git diff --check 仅 CRLF 提示。下一轮精确入口：manifest revision single-flight / contract capture skip counter / 低压图 semanticConsumerBuildUs 与 semanticContractCaptureUs 累计成本压缩；真实 FPS 需用可见桌面专用 perf run，当前 isolated desktop windowed report 若 fpsSampleReliable=false 不可当性能结论。`

31. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `AutoTest/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 06:44:01.210 +08:00`
   LastHeartbeat: `2026-04-25 07:00:13.821 +08:00`
   ClosedAt: `2026-04-25 07:00:13.821 +08:00`
   Status: `closed`
   Result: `[正常关闭:(stale pending build 清理 + contract capture no-op skip 已落地并暴露 control-plane counters；低压图 core/contract/object fallback 均满足，剩余为 isolated desktop scene tail gate 与 consumer 累计成本；ninja -C build32 -j4、py_compile、diff check 均通过/仅 CRLF 提示)]`
   Goal: `[Phase 3/4 semantic 性能收口：在三场景 correctness 已通过后，压低 low_pressure 的 semanticConsumerBuildUs / semanticContractCaptureUs 累计成本；优先做 manifest revision single-flight、stale pending 清理、contract capture no-op counter/短路；不重开 owner/resource/palette 逆向]`
   Notes: `接续 9.11.30 / 35.18。新增 semanticCoreStalePendingBuildClearedCount 与 contractCaptureSkipped* counters；低压图最新 summary：buildInProgress=false、buildRequestPending=false、coreFrame=149、sceneFrame=148、skinned=144、attachmentRigid=6、sceneSkinned=139、fallback=0、staleCleared=5、stableSkip=8、duplicateSkip=17、contractCaptureUs=69915、consumerBuildUs=207979。dynamic_shadow_pressure hot-shadow 正确性通过：buildDurationUs=136、skinned=30、sceneSkinned=30、fallback=0；最终 tool ok=false 仅因 screenshot/report 后处理失败。AutoTest 已补 semanticTailSceneNearLatestAccepted，但需重载 MCP 后生效。下一轮精确入口：重载 MCP 复测低压 tail gate，然后继续压 semanticConsumerBuildUs，重点看 semanticLastHotFunctionTag=6 / semanticLastHotFunctionUs≈15917；不要重开 owner/resource/palette 逆向，不恢复 snapshot/freeze/VB/IB 主路径。`

32. Thread: `native-main-codex`
   Role: 主线程
   Scope: `AutoTest/war3_autotest_mcp.py`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 07:00:13.821 +08:00`
   LastHeartbeat: `2026-04-25 07:15:03.380 +08:00`
   ClosedAt: `2026-04-25 07:15:03.380 +08:00`
   Status: `closed`
   Result: `[正常关闭:(AutoTest named scenario 已显式携带 semantic validation env；显式 env 复测证明 model_runtime_probe hot-shadow ok=true、低压图 core/scene/skinned/fallback 指标均正确，当前失败归类为旧 MCP gate 和截图/report 后处理，不是上层数据链失败)]`
   Goal: `[修正 semantic preview 默认关闭后 AutoTest named scenario 没打开 validation env 导致 semanticBuildSkippedReason=6 的测试误判；保持普通手动进图默认安全关闭，验证场景显式打开]`
   Notes: `新增 SEMANTIC_SHADOW_VALIDATION_ENV，覆盖 model_runtime_probe / low_pressure_static_reuse / dynamic_shadow_pressure / semantic_cost_probe；run_named_scenario 现在合并 preset env + 调用方 env。py_compile 通过。当前 MCP 未重载，所以本轮用显式 env_overrides_json 复测：model_runtime_probe hot-shadow ok=true、semanticCoreSkinnedResolved=30、semanticSceneSubmittedSkinned=30、fallback=0；低压图 ready=7.321s、semanticCoreBuildDurationUs=46958、semanticCoreSkinnedResolved=138、semanticSceneSubmittedSkinned=146、fallback=0、semanticConsumerBuildUs=164037、semanticContractCaptureUs=58862。下一轮先重载 MCP，再跑 named scenario；随后修 isolated desktop screenshot/report 后处理，并继续压 semanticLastHotFunctionTag=6。`

33. Thread: `native-main-codex`
   Role: 主线程
   Scope: `AutoTest/war3_autotest_mcp.py`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 07:17:11.154 +08:00`
   LastHeartbeat: `2026-04-25 07:19:58.926 +08:00`
   ClosedAt: `2026-04-25 07:19:58.926 +08:00`
   Status: `closed`
   Result: `[正常关闭:(AutoTest hot-shadow control-plane gate 已补 same-frame semantic scene acceptance；py_compile、ninja -C build32 -j4、git diff --check 通过/仅 CRLF 提示；当前无 War3 残留进程。未在旧 MCP 进程上盲跑低压 preset，避免未重载旧 preset 的窗口化/全屏风险)]`
   Goal: `[继续 Phase 3/4：修 AutoTest hot-shadow gate 对低压图 same-frame semantic scene consumption 的误判，然后继续验证 semantic-only skinned + fallback=0；不重开 owner/resource/palette 逆向]`
   Notes: `接续 9.11.32 / 35.20。低压图显式 env 下已经有 semanticCoreSkinnedResolved=138、semanticSceneSubmittedSkinned=146、semanticSceneSubmitted=146、semanticSceneConsumptionFresh=true、semanticSceneSameFrameConsumed=true、objectFallbackDrawCount=0；本轮把 scene fresh/same-frame consumed + fallback=0 接成 semanticSceneOnlyAccepted。下一轮先重载 AutoTest MCP，再跑 model_runtime_probe / low_pressure_static_reuse；随后继续压 semanticLastHotFunctionTag=6 / semanticConsumerBuildUs，不恢复 snapshot/freeze/VB/IB。`

34. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `AutoTest/war3_autotest_mcp.py`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 07:21:17.724 +08:00`
   LastHeartbeat: `2026-04-25 11:35:26.496 +08:00`
   ClosedAt: `2026-04-25 11:35:26.496 +08:00`
   Status: `closed`
   Result: `[正常关闭:(root supplement resource/no-geoset 分桶已接入 control-plane；semantic key 小预算反查证明可把低压图 no-resource 从 735 级别降到约 50，但新 blocker 收敛为 ShadowModelResourceStore store-miss；热帧全量 rebuild store 已证伪，会导致 control-plane 超时)]`
   Goal: `[Phase 3/4 性能继续收口：针对 semanticLastHotFunctionTag=6 / semanticConsumerBuildUs 做 consumer build single-flight、manifest revision 复用和缺数据 cooldown；不重开 owner/resource/palette 逆向，不恢复 snapshot/freeze/VB/IB]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_resource_semantic_key_model_probe.json；AutoTest/artifacts/codex_resource_semantic_key_budget_low_pressure.json；AutoTest/artifacts/codex_resource_geoset_fallback_low_pressure.json；AutoTest/artifacts/codex_no_geoset_breakdown_low_pressure.json；AutoTest/artifacts/codex_refresh_store_on_supplement_miss_low_pressure.json。关键结论：resource miss 已显著下降，低压图 budget 版 rootUnitSupplementSkippedNoResource≈46~54；但 rootUnitSupplementSkippedNoGeoset≈849，且几乎全是 StoreMiss，ZeroCount=0、NotReady=0。semantic key 反查必须限流到小预算；全量 resource store rebuild 不能放在热帧。ninja -C build32 -j4 通过，git diff --check 无 whitespace error/仅 CRLF warning；无残留 War3/IDA 进程。下一轮精确入口：实现 ShadowModelResourceStore 增量 alias/add 或 cache->store 轻量同步，目标先压 rootUnitSupplementSkippedNoGeosetStoreMiss，而不是继续扩大深解析或恢复旧 snapshot/freeze/VB/IB。`

35. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_runtime_contract.*`, `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`, `src/d3d9/war3/tools/war3_control_plane.*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 11:38:18.422 +08:00`
   LastHeartbeat: `2026-04-25 12:15:06.995 +08:00`
   ClosedAt: `2026-04-25 12:15:06.995 +08:00`
   Status: `closed`
   Result: `[正常关闭:(root supplement geoset store-miss 已通过单条 cache overlay 明显压低；core store-miss 时也能从 resource cache 消费局部 geoset；dynamic pressure 已恢复 semantic core 部分提交且 fallback=0。剩余 blocker 转为 pending semantic build / render-scene consumption timing，而不是 geoset 数据缺失)]`
   Goal: `[Phase 3/4 root supplement store-miss 收口：实现 ShadowModelResourceStore 增量 alias/add 或 cache->store 轻量同步，目标压低 rootUnitSupplementSkippedNoGeosetStoreMiss；禁止热帧全量 rebuild store、禁止扩大 semantic key 深解析预算、禁止恢复 snapshot/freeze/VB/IB]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_incremental_geoset_overlay_model_probe.json；codex_incremental_geoset_overlay_dedupe_low_pressure.json；codex_core_cache_fallback_dynamic_pressure.json；codex_core_cache_fallback_low_pressure.json；codex_core_cache_fallback_pose_seed_dynamic_pressure.json。关键读数：低压图 StoreMiss 从约 848 降到 18，rootUnitSupplementGeosetCacheFallback=60，Appended=32；dynamic pressure 中 semanticCoreSubmittedDrawCount=6、semanticCoreSkinnedResolved=2、objectFallbackDrawCount=0。已证伪 control-plane 同步 drain pending build：会导致 pipe timeout，补丁已撤回。ninja -C build32 -j4 通过，git diff --check 无 whitespace error/仅 CRLF warning，无 War3/IDA 残留进程。下一轮精确入口：安全解决 semanticCoreBuildRequestPending/latest revision consumption，不要重开 geoset full rebuild、不要扩大 semantic key budget。`

36. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.*`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/render/war3_renderer.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 12:18:06.937 +08:00`
   LastHeartbeat: `2026-04-25 17:47:24.387 +08:00`
   ClosedAt: `2026-04-25 17:47:24.387 +08:00`
   Status: `closed`
   Result: `[正常关闭:(indexed runtime-owner hydrate 已落地并通过隔离桌面低压验证；semantic skinned direct coverage 从 66 提升到 84，no-pose 从 62 降到 44，core build 仍保持约 11.4ms、slowest record 347us、resource/pose lookup 0~2us，objectFallbackDrawCount=0。当前剩余 blocker 是剩余 direct pose 覆盖与 native/scene execute counter 仍为 0，不是 consumer 侧计算量 storm。)]`
   Goal: `[Phase 3/4 pending semantic build 消费收口：在 render thread/scene submission 安全推进 semanticCoreBuildRequestPending/latest revision，恢复 semanticSceneSubmittedSkinned>0 与 objectFallbackDrawCount=0；禁止 control-plane 同步大块 drain、禁止 full resource store rebuild、禁止扩大 semantic key budget]`
   Notes: `接续 9.11.36 / 35.23。本轮新增 ShadowModelResourceCache::findRuntimeModelOwnerIndexed 与 ShadowRuntimeContractCache manifest hydrate，只查 existing runtimeGeoset/Data owner index，不走全量扫描。fresh 工件：AutoTest/artifacts/codex_low_pressure_indexed_owner_hydrate_20260425.json、AutoTest/artifacts/screenshots/codex_low_pressure_indexed_owner_hydrate_20260425.png、E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_17_43_35.html。关键值：semanticCoreResolved=84、semanticCoreSkinnedResolved=84、semanticCoreSkippedNoPose=44、semanticCoreBuildDurationUs=11400、semanticCoreSlowestRecordResolveUs=347、semanticCoreSlowestResourceLookupUs=0、semanticCoreSlowestPoseResolveUs=2、objectFallbackDrawCount=0、nativeD3D9BackendSubmittedSkinnedDrawCount=84、nativeD3D9BackendExecuteAttemptCount=0。下一轮精确入口：补剩余 direct pose producer key；追 submitted semantic skinned packets 未 actual execute 的 native/scene 时机；不要恢复 consumer owner/pose fallback 扫描。`

37. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/war3_model_hook.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 20:22:00.000 +08:00`
   LastHeartbeat: `2026-04-25 20:40:55.000 +08:00`
   ClosedAt: `2026-04-25 20:40:55.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(render-off + semantic.data 稳定窗口已达 253~261 FPS；geoset capture 关闭时不再安装 CreateGeosetFromRawArrays hook；semanticFrameRegistryPublishCalls=0、semanticModelGeosetResourceCalls=0。上层数据层在不开渲染层口径下已不再是性能 blocker。)]`
   Goal: `[继续修复上层 semantic.data，直到关闭渲染层时 FPS 接近原版 250FPS；禁止把 visible finalizer/geoset capture 空转放回热路径]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_render_off_semantic_on_no_geoset_hook_perfonly2_20260425.json、AutoTest/artifacts/codex_render_off_semantic_on_stable_20260425.json；稳定报告：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_20_40_01.html avgFps=253.767，E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_20_40_13.html avgFps=260.970。render-off 模式 ready gate 会天然 timeout，性能测试必须取 perf-only 后段稳定窗口。下一轮精确入口：切回 full render/semantic shadow，排查实际绘制层 0.2FPS 来源和 shadow source distribution，不再把上层数据层当主 blocker。`

38. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/render/war3_renderer.cpp`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`, `src/d3d9/war3/core/war3_internal_test_config.h`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-25 22:04:00.000 +08:00`
   LastHeartbeat: `2026-04-25 22:36:30.000 +08:00`
   ClosedAt: `2026-04-25 22:36:30.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(full render 低压图已从 semantic scene storm 的 63 FPS 推到默认语义 skinned + fallback=0 的 89 FPS；same-frame submit single-flight、runtime geoset seed、legacy unit fallback bypass 默认化、endframe flush duplicate skip、semantic scene per-scope perf tracing 均已落地)]`
   Goal: `[继续把现有数据层真正接到动态阴影主路径，并把 full render low pressure 收口到 100 FPS+；禁止恢复旧 object fallback 作为主路径]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_low_pressure_manual_full_20260425.json、codex_low_pressure_manual_full_after_singleflight_20260425.json、codex_low_pressure_manual_full_after_runtime_geoset_seed_20260425.json、codex_low_pressure_after_endframe_flush_skip_20260425.json、codex_low_pressure_after_latest_shortcircuit_20260425.json、codex_low_pressure_default_bypass_perfscope_20260425.json。关键报告：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_25_22_09_44.html avgFps=63.771；...22_15_55.html avgFps=84.961；...22_33_57.html avgFps=89.036。关键值：shadowReadyGeosetCount=326、matrixPaletteCount=34、semanticSceneSubmittedSkinned=32、objectFallbackDrawCount=0。剩余热点：CaptureContract≈1.18ms/frame、SubmitFrame≈0.29ms/frame、BootstrapCatchup≈0.23ms/frame、EnsureLatestFrameBuilt≈0.23ms/frame、Other/UntrackedActive≈8.3ms/frame。下一轮精确入口：继续压跨帧稳定 contract 复用与 steady-state ensure/catchup，不要把 legacy fallback 再打开。`

39. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/*`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-26 02:34:35.662 +08:00`
   LastHeartbeat: `2026-04-26 03:14:42.000 +08:00`
   ClosedAt: `2026-04-26 03:14:42.000 +08:00`
   Status: `closed`
   Result: `[正常关闭:(低压图 semantic-only skinned path 已从 89 FPS 推到正式 gate 123.572 FPS / 短窗 127.120 FPS；semanticSceneSubmittedSkinned>0 且 objectFallbackDrawCount=0。旧 ShadowCapture 已在 semantic scene submission 下默认退出 object shadow 主路径；EndFrame build 默认关闭；DXVK-only AutoTest gate 不再误等 native executed draw。dynamic_shadow_pressure correctness 通过但 GPU/ShadowMap bound，下一轮转 shadow renderer 高压优化。)]`
   Goal: `执行 2026-04-26 War3 Semantic Shadow 夜间优化计划：优先压 full render low-pressure 语义动态阴影性能到 100FPS+，保持 semanticSceneSubmittedSkinned>0、objectFallbackDrawCount=0、semanticCoreSkippedNoPose=0，不启用 native preview，不恢复 VB/IB snapshot/freeze 主路径。`
   Notes: `fresh 工件：codex_low_pressure_disable_shadow_capture_probe_20260426.json、codex_low_pressure_pipeline_gate_capture_off_20260426.json、codex_low_pressure_no_endframe_build_probe_20260426.json、codex_low_pressure_semantic_optimized_gate_20260426.json、codex_low_pressure_static_reuse_semantic_optimized_20260426.json、codex_dynamic_shadow_pressure_semantic_optimized_20260426.json。关键报告：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_26_03_01_58.html / 03_09_30.html / 03_10_48.html。下一轮精确入口：CaptureContract direct pose copy/hash cache 与 War3ShadowReceiverPass::renderShadowMap skinned caster/cascade/GPU 优化；War3VK 剥离暂缓到 130 FPS 目标更稳定后。`

40. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_war3_shadow.cpp`, `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.*`, `E:\Mycode\Source\Repos\War3VK`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-26 04:12:46 +08:00`
   LastHeartbeat: `2026-04-26 05:50:00 +08:00`
   ClosedAt: `2026-04-26 05:50:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(skinned caster cascade cull/bounds padding 已落地，低压三轮 139.723/150.982/149.187 FPS，median=149.187，worst=139.723，objectFallbackDrawCount=0，semanticSceneSubmittedSkinned>0；dynamic pressure 提升到 114.928 FPS，ShadowMap GPU=2.878ms。War3VK 独立 Win32 DLL/ASI 骨架已创建并用 VS2026 MSBuild Release|Win32 构建通过；静态 CRT 后仅依赖 KERNEL32；War3VKLoadSmoke 可 LoadLibrary/GetStats；War3 根目录 War3VK.asi 在隔离桌面窗口化启动中生成 DllMain/Game.dll marker，SchattenBoost.dll 临时文件已删除。)]`
   Goal: `[执行 War3 Semantic Shadow 优化与 ASI 迁移无人值守计划：先修低 Z/远镜头 caster 缺底与 skinned caster/cascade ShadowMap 性能，低压图稳定追 130 FPS+ 且 objectFallbackDrawCount=0；达标后再创建 War3VK Win32 DLL/ASI late-inject 骨架并验证自动加载。]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_semantic_skinned_cascade_cull_20260426.json；AutoTest/artifacts/codex_war3vk_asi_bootstrap_20260426.json。关键代码：src/d3d9/d3d9_war3_shadow.cpp、src/d3d9/war3/core/war3_internal_test_config.h、AutoTest/war3_autotest_mcp.py、E:\Mycode\Source\Repos\War3VK\*. 下一轮精确入口：DXVK 侧可选 War3VK ABI bridge，默认关闭，先只 LoadLibrary/GetProcAddress/BindDevice/GetStats 生命周期 proof；不要搬 DXVK renderer 大对象，不恢复 legacy snapshot/freeze。`

41. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/war3/render/war3_visible_renderables.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-30 02:37:00 +08:00`
   LastHeartbeat: `2026-04-30 02:44:33 +08:00`
   ClosedAt: `2026-04-30 02:44:33 +08:00`
   Status: `closed`
   Result: `[正常关闭:(当前 semantic-only units 基线已复测恢复：低压图 avgFps=127.891、semanticSceneSubmittedSkinned=48504、semanticCoreSubmittedDrawCount=40、objectFallbackDrawCount=0；静态 hydrate 主清单路径被证伪，打开后 semanticSceneSubmittedSkinned=0、shadowReadyGeosetCount=25，并产生 crash dump，补丁已撤回。)]`
   Goal: `[继续项目推进：在不恢复 VB/IB snapshot/freeze 的前提下确认当前 semantic-only units 基线，并试探静态对象阴影下一步安全入口。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_42_45.html、AutoTest/artifacts/screenshots/war3_20260430_024246.png；失败证据：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_02_40_06.html、E:\Work\War3\WarVK\Crash\war3_crash_2026_04_30_02_40_06_678_pid11528_tid27112.dmp。下一轮精确入口：实现只读 static-caster observer，不修改 VisibleRenderableRegistry::Snapshot，不在 units-only 主路径打开 kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate。`

42. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/d3d9_device.h`, `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-30 03:28:00 +08:00`
   LastHeartbeat: `2026-04-30 04:18:44 +08:00`
   ClosedAt: `2026-04-30 04:18:44 +08:00`
   Status: `closed`
   Result: `[正常关闭:(stale semantic completed frame 导致的错影已修复；scene submission 改为 periodic capture/build + 每帧 submit-time live CModel palette refresh。低压 quick 2K 达 135.540/135.009 FPS，named low_pressure_static_reuse 达 155.758 FPS；objectFallbackDrawCount=0，semanticSceneSubmittedSkinned>0，semanticCoreSkippedNoPose=0。dynamic_shadow_pressure 通过但仍 GPU/ShadowMap bound：avgFps=40.689、ShadowMap GPU≈4.257ms。)]`
   Goal: `[用户视觉确认“一个 caster 的阴影被套到所有 caster”后，继续自主修复 semantic-only 动态阴影正确性与性能；禁止恢复 VB/IB snapshot/freeze，禁止重开 static hydrate 主清单路径。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_04_05_50.html、04_08_51.html、04_12_41.html、04_14_08.html、04_17_06.html；截图：AutoTest/artifacts/screenshots/war3_20260430_040551.png、040852.png、041218.png、041409.png、041643.png。关键代码：D3D9DeviceEx::War3TryPopulateSemanticShadowScene periodic contract/build；War3TryAppendSemanticShadowPacket submit-time CModel palette refresh；ShadowPacketResource matrixGroupSizes/matrixIndices。下一轮精确入口：补 live palette refresh/miss counters 到 summary；继续做 ShadowMap/cascade/GPU 优化；静态对象只走只读 observer/独立 candidate store，不写主 manifest。`

43. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/hooks/war3_hook_render.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-30 19:03:00 +08:00`
   LastHeartbeat: `2026-04-30 20:11:12 +08:00`
   ClosedAt: `2026-04-30 20:11:12 +08:00`
   Status: `closed`
   Result: `[正常关闭:(FillUnitIdentity 的 VirtualQuery 风暴已通过 TLS unit identity hot cache 清掉；submit-time live CModel palette refresh counters 已在 report/control-plane 中验证全命中；live palette 构建从全量 pose vector 解码改成按需解码 + 单调用 generation cache。低压图当前 87~94 FPS，objectFallbackDrawCount=0，semanticSceneSubmittedSkinned>0，semanticSceneSkinnedFullIndexFallbackCount=0。)]`
   Goal: `[接续 04:18 后自动推进：排查上午 7 点后挂住的 semantic-only 阴影性能/正确性，禁止恢复 VB/IB snapshot/freeze，禁止重开 static hydrate 主清单路径；所有 War3 测试 isolated desktop + windowed。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_20_02_16.html、20_09_11.html；截图：AutoTest/artifacts/screenshots/war3_20260430_200153.png。关键值：20_02_16 avgFps=94.386、semanticSceneLivePaletteRefreshHitCount=81583、Miss=0、FullIndexFallback=0、objectFallbackDrawCount=0；20_09_11 avgFps=87.795、semanticCoreSubmittedDrawCount=50、semanticSceneSubmittedSkinned=68230、FullIndexFallback=0、objectFallbackDrawCount=0。负收益试验：palette group count 裁剪到 maxVertexGroupSlot+1 后 FPS 约 87.605，已撤回。下一轮精确入口：拆 D3D9DeviceEx::War3TryAppendSemanticShadowPacket 的 SubmitFrame 子阶段；继续定位 Other/UntrackedActive 9~10ms；静态对象只读 observer/独立 store，不写 main visible manifest。`

44. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/war3_shadow_renderer_core.*`, `src/d3d9/war3/core/war3_internal_test_config.h`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-04-30 21:41:00 +08:00`
   LastHeartbeat: `2026-04-30 22:21:14 +08:00`
   ClosedAt: `2026-04-30 22:21:14 +08:00`
   Status: `closed`
   Result: `[正常关闭:(semantic submit cap 从临时 cappedFrame 拷贝改为 ShadowRendererCore::submitFrameLimited 内联限额；默认 cap=64，新增 DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP runtime override。低压图 108.835 FPS、semanticSceneSubmittedSkinned=105205、live palette miss=0、fullIndexFallback=0、objectFallbackDrawCount=0；高压 cap=64 达 29.134 FPS，cap=32 试验达 31.028 FPS。AutoTest perf-only-ready-timeout 已落地并 py_compile 通过；no-shadow perf-only 报告仍被判定非进图，因为 inGameRenderReady=false/visibleCount=0。)]`
   Goal: `[接续上午自动推进挂住点，继续 semantic-only skinned 正确性和性能收口；优先确认 owned dynamic index / no-fallback 仍稳定，并压高压 SubmitFrame/submit cap；禁止恢复 VB/IB snapshot/freeze，禁止 static hydrate 写 main visible manifest。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_04_30_21_43_12.html、21_46_20.html、22_05_19.html、22_06_36.html、22_13_29.html、22_21_14.html；截图：AutoTest/artifacts/screenshots/war3_20260430_214249.png、214557.png、220456.png、220637.png、221331.png。下一轮精确入口：补 AutoTest shadow-off in-map/world-ready marker；启用/新增低侵入 MainLoop active gap 采样解释 Other/UntrackedActive 5.86ms->8.58ms；继续拆 War3TryAppendSemanticShadowPacket 的 append self。`

45. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/core/war3_internal_test_config.h`, `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 01:46:00 +08:00`
   LastHeartbeat: `2026-05-01 02:30:00 +08:00`
   ClosedAt: `2026-05-01 02:30:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(动态阴影锁初始化姿态的根因已定位：submit-time CModel +0x5C/+0x60 live palette 是静态/非动画源，且会覆盖 ShadowRendererCore packet runtimeGroupPalette。新增 motion counters 证明 raw hash 6~7 万样本不变；禁用默认 live refresh 后，低压图 skinned semantic shadow 仍提交、fallback=0，并通过 4 张间隔截图确认单位动作与阴影 silhouette 同步变化。frame-local dynamic mesh rescue 未命中当前主路，已恢复为显式诊断 opt-in。)]`
   Goal: `[优先解决用户反馈的“阴影锁在模型初始化姿态”问题；禁止继续只凹 FPS，先证明动态姿态来源正确；所有重要发现写入状态文档避免后续反复打开错误路径。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_01_52_50.html、01_58_27.html、02_17_56.html、02_27_04.html；截图：AutoTest/artifacts/screenshots/dynamic_pose_check_20260501_022220/pose_01.png、pose_04.png、AutoTest/artifacts/screenshots/war3_20260501_022642.png。关键值：semanticSceneLivePaletteMotionRawChangedCount=0、RawStable=66481/69557；最终正式路径 semanticSceneLivePaletteRefreshAttemptCount=0、semanticSceneSubmittedSkinned=73350、semanticSceneSubmittedFrameLocal=0、objectFallbackDrawCount=0。下一轮精确入口：基于 packet runtimeGroupPalette 增加 motion counter；检查单位脚下 shadow alignment、low-Z bounds/cascade；性能优化不得默认恢复 DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH。`

46. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/war3/model/war3_model_hook.cpp`, `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`, `src/d3d9/war3/core/war3_internal_test_config.h`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 03:45:00 +08:00`
   LastHeartbeat: `2026-05-01 04:43:04 +08:00`
   ClosedAt: `2026-05-01 04:43:04 +08:00`
   Status: `closed`
   Result: `[正常关闭:(用户反馈“阴影仍初始化姿态 + 约 3 秒消失”后，修正 matrix producer：0x6F12FF50 被确认为 flush/reset，已禁止写 PoseRegistry；0x6F12FDC0 source-range copy 作为 authoritative palette producer；pose-only contract 不再 bump publishRevision 或请求 build；submit live refresh 改为优先消费 PoseRegistry source-range palette。低压复测 semanticCoreFrameFresh=true、semanticCoreSubmittedDrawCount=47、semanticSceneLivePaletteRefreshHit=58904、PaletteMotionChanged=58887、objectFallbackDrawCount=0。)]`
   Goal: `[优先解决动态阴影静态/bind-pose 和周期性空窗问题；不要再只追 FPS；所有重要逆向/运行结论必须写入状态文档防止复开错误路径。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_03_58_58.html、04_10_51.html、04_40_56.html；截图：AutoTest/artifacts/screenshots/war3_20260501_044032.png。关键值：03_58 full capture dirty avgFps=9.615 / ManifestCopy=76.855ms；04_40 source-range pose live refresh avgFps=83.046、semanticCoreFrameFresh=true、semanticSceneSubmittedSkinned=58904、semanticSceneLivePaletteRefreshMissCount=0、objectFallbackDrawCount=0。下一轮精确入口：给 PoseRegistry live refresh 增 keyed cache；加 runtimeMatrixRangeCopyPalettePublishHit/Miss 和 flush suppressed counters；用多帧视觉序列确认 silhouette 彻底脱离 bind pose。`

47. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 06:20:00 +08:00`
   LastHeartbeat: `2026-05-01 06:42:00 +08:00`
   ClosedAt: `2026-05-01 06:42:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(用户最新反馈“性能可接受但阴影仍为初始姿态、约 3 秒消失”后，复测确认 source-range pose producer 是活的：runtimeMatrixRangeCopyPalettePublishHitCount 持续增长且 CModel fallback=0；真正卡点是 scene/core fresh gate 只看 publishRevision，pose-only contract 保持 revision 不变导致 semanticCoreFrameSerial 卡首帧。已修 D3D9DeviceEx::War3TryPopulateSemanticShadowScene fresh/catchup/complete 判断加入 frameSerial；War3ShouldPreferSemanticSceneFrame 与 ShouldPreferRenderableSubmissionFrame 同 revision 时优先更新 frameSerial；steady build 改为 2 passes/frame。复测 semanticCoreFrameSerial 已连续推进，lag 多数 1-4，objectFallbackDrawCount=0。)]`
   Goal: `[优先解决动态阴影 bind-pose/static pose；持续记录关键结论，禁止复开 submit-time CModel+0x60 live refresh 默认路径。]`
   Notes: `fresh 工件：AutoTest/artifacts/codex_pose_only_rebuild_20260501/result.json（修复前 frameSerial 卡 145）、AutoTest/artifacts/codex_pose_frame_freshness_20260501/result.json（修复后 frameSerial 推进到 959）、AutoTest/artifacts/codex_pose_visual_probe_20260501/frame_1.png~frame_3.png。下一轮精确入口：给 semantic scene reusable frame forced/eligible-zero 增 counter；给 War3TryAppendSemanticShadowPacket 增 packet runtimeGroupPaletteHash motion counter；若用户仍看到静态，只查 palette upload/cache 和 shadow map draw，不再重开 owner/CModel+0x60。`

48. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `src/d3d9/d3d9_war3_shadow.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 06:55:00 +08:00`
   LastHeartbeat: `2026-05-01 07:12:00 +08:00`
   ClosedAt: `2026-05-01 07:12:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(用户反馈“性能可接受但阴影仍初始化姿态、约 3 秒消失”后，定位到 ShadowMap adaptive reuse 把部分 formal semantic skinned caster 当成静态：dynamicSkinnedOutputCount/dynamicPoseSignature 只在 unitLikeObject 分支更新。已改成任意 skinned packet 都更新动态签名并纳入 submittedRuntimeGroupPaletteHash；ShadowMap reuse 只要看到 dynamicSkinnedOutputCount/semanticSceneSubmittedSkinned/capturedUnitVertexBlend 非零就禁用。低压复测 objectFallbackDrawCount=0、semanticSceneSubmittedSkinned=66296、PaletteMotionChanged=66279、ShadowMap callsPerFrame=0.999；连续 6 张截图确认单位动作和阴影 silhouette 均变化。)]`
   Goal: `[继续优先解决动态阴影静态/bind-pose 与周期性空窗；不恢复 VB/IB snapshot/freeze，不重开 CModel+0x60 submit refresh。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_07_21.html；截图序列：AutoTest/artifacts/codex_dynamic_pose_sequence_20260501/pose_seq_01.png~pose_seq_06.png；结果：AutoTest/artifacts/codex_dynamic_skinned_shadowmap_20260501/result.json、AutoTest/artifacts/codex_dynamic_pose_sequence_20260501/result.json。当前性能回落 avgFps=84.032，是正确性优先下禁用动态 skinned ShadowMap 复用造成；下一轮精确入口：基于 dynamicPoseSignature/palette hash 做安全复用，而不是按 caster count/camera 稳定盲复用。`

49. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_device.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 07:18:00 +08:00`
   LastHeartbeat: `2026-05-01 07:26:00 +08:00`
   ClosedAt: `2026-05-01 07:26:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(为避免后续恢复 ShadowMap reuse 时重现“单位移动但阴影位置几秒才更新”，已将 skinned/dynamic packet 的 dynamicPoseSignature 从 palette/pose hash 扩展到 draw.worldMatrix + boundsCenter/boundsRadius。低压短窗构建与测试通过，semanticSceneSubmittedSkinned=30732、dynamicSkinnedOutputCount=30732、objectFallbackDrawCount=0；6 张隔离桌面截图序列均成功，逐帧 summary 显示 semanticSceneSubmittedSkinned=34、SubmittedPaletteMotionChanged=34、fallback=0。)]`
   Goal: `[继续优先修动态阴影正确性；在允许安全复用 ShadowMap 前，dynamicPoseSignature 必须覆盖 palette 和 world-space placement。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_20_48.html；截图序列：AutoTest/artifacts/codex_dynamic_pose_sequence_after_worldhash_20260501/pose_worldhash_01.png~pose_worldhash_06.png；结果：AutoTest/artifacts/codex_dynamic_pose_retest_20260501/result.json、AutoTest/artifacts/codex_dynamic_pose_sequence_after_worldhash_20260501/result.json。下一轮精确入口：War3ShadowReceiverPass::Run 可尝试基于 dynamicPoseStable 恢复安全 reuse，但不得回到 caster count/camera-only reuse。`

50. Thread: `native-main-codex`
   Role: 主线程
   Scope: `src/d3d9/d3d9_war3_shadow.cpp`, `docs/plan/automation_exchange/*`, `docs/plan/semantic_shadow_control_plane_status_2026_04_19.md`
   StartedAt: `2026-05-01 07:30:00 +08:00`
   LastHeartbeat: `2026-05-01 07:38:00 +08:00`
   ClosedAt: `2026-05-01 07:38:00 +08:00`
   Status: `closed`
   Result: `[正常关闭:(基于已包含 world transform 的 dynamicPoseSignature，恢复 ShadowMap safe reuse：有 dynamic skinned caster 时仅允许 signature 非零且稳定才复用，禁止 caster-count/camera-only 复用。低压短窗 objectFallbackDrawCount=0、semanticSceneSubmittedSkinned=35560、dynamicSkinnedOutputCount=35560、ShadowMap callsPerFrame=0.583；6 张安全复用截图序列未复现周期性阴影消失。)]`
   Goal: `[在保证动态 skinned 正确的前提下恢复安全复用，继续 no-fallback semantic-only 主路径。]`
   Notes: `fresh 工件：E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_05_01_07_32_41.html；截图序列：AutoTest/artifacts/codex_dynamic_pose_sequence_safe_reuse_20260501/pose_safe_reuse_01.png~pose_safe_reuse_06.png；结果：AutoTest/artifacts/codex_dynamic_signature_reuse_20260501/result.json、AutoTest/artifacts/codex_dynamic_pose_sequence_safe_reuse_20260501/result.json。下一轮精确入口：拆 SubmitFrame/PaletteIndex 与 Other/UntrackedActive；若用户仍反馈脚下 alignment 错，优先查 bounds/worldMatrix handedness，而不是回退 pose producer。`
