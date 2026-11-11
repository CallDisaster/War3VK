"""Production retirement wiring gate; CPU/GPU behavior is not inferred from text.

Checks operate on comment/literal-stripped C++ function bodies. The separate
production C++ test covers allocator exceptions and ownership lifetimes. Optional
--mutations exercises twelve in-memory disconnect/order mutations; no source is
rewritten and no build or game process is launched.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
DEVICE_HEADER = ROOT / "src/d3d9/d3d9_device.h"
RETIREMENT = ROOT / "src/d3d9/war3/shadow/war3_shadow_session_retirement.h"
PREPARE = "War3PreparedShadowRetirement prepared(m_war3RetiredShadowSessions);"
READY_OFF = "m_war3ShadowSessionReady.store(false,std::memory_order_release);"
OWNERS = {
    "shadowAllocators": "m_war3ShadowAllocators",
    "shadowMappedAllocators": "m_war3ShadowMappedAllocators",
    "frozenGeometryCaches": "m_war3ShadowFrozenGeometryCaches",
    "frameFreezeCatalog": "m_war3FrameFreezeCatalog",
    "geometryRegistry": "m_war3ShadowGeometryRegistry",
    "persistentGeometries": "m_war3ShadowPersistentGeometries",
    "stage13RetainedCasters": "m_war3Stage13RetainedCasters",
    "persistentExpiryQueue": "m_war3ShadowPersistentExpiryQueue",
    "drawTimeVbCache": "m_war3DrawTimeVBCache",
    "s1TerrainCasterStash": "m_war3S1TerrainCasterStash",
    "s1TerrainEarlyCache": "m_war3S1TerrainEarlyCache",
}


def code_only(source: str) -> str:
    """Blank comments and ordinary/raw literals, retaining delimiters/offsets."""
    tokens = re.compile(
        r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})'
        r'\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return tokens.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), source)


def compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def closing(source: str, opening: int, left: str = "{", right: str = "}") -> int:
    if opening < 0 or source[opening] != left:
        raise AssertionError("missing opening delimiter")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == left:
            depth += 1
        elif source[index] == right:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError("unclosed production delimiter")


def braced(source: str, pattern: str) -> tuple[str, int, int]:
    match = re.search(pattern, source)
    if match is None:
        raise AssertionError("missing production block: " + pattern)
    start = source.find("{", match.start())
    end = closing(source, start)
    return source[start + 1:end], start + 1, end


def function(source: str, name: str) -> tuple[str, int, int]:
    """Extract a real class-qualified definition, not a call-site text window."""
    matches = list(re.finditer(r"\bD3D9DeviceEx::" + re.escape(name) + r"\s*\(", source))
    definitions = []
    for match in matches:
        start = source.index("(", match.start())
        params_end = closing(source, start, "(", ")")
        body_start = source.find("{", params_end + 1)
        if body_start < 0 or ";" in source[params_end + 1:body_start]:
            continue
        body_end = closing(source, body_start)
        definitions.append((source[body_start + 1:body_end], body_start + 1, body_end))
    if len(definitions) != 1:
        raise AssertionError(f"expected one {name} definition, found {len(definitions)}")
    return definitions[0]


def top_level(source: str) -> str:
    """Do not let a Ready store in an earlier conditional satisfy an outer gate."""
    depth = 0
    result = []
    for char in source:
        if char == "{":
            depth += 1
            result.append(" ")
        elif char == "}":
            depth -= 1
            result.append(" ")
        else:
            result.append(char if depth == 0 or char == "\n" else " ")
    return "".join(result)


@dataclass
class Report:
    checks: int = 0
    failures: list[str] = field(default_factory=list)

    def check(self, condition: bool, label: str) -> None:
        self.checks += 1
        if not condition:
            self.failures.append(label)

    def ordered(self, source: str, label: str, *tokens: str) -> None:
        positions = [source.find(token) for token in tokens]
        self.check(all(p >= 0 for p in positions) and positions == sorted(set(positions)), label)


def audit(device_raw: str, header_raw: str, retirement_raw: str) -> Report:
    report = Report()
    device, header, retirement = map(code_only, (device_raw, header_raw, retirement_raw))
    try:
        functions = {
            name: function(device, name)[0]
            for name in (
                "War3ApplyShadowMapEpochResetAtPresent", "War3ApplyShadowDeviceEpochTransitionAtPresent",
                "War3DrainShadowCasterTombstones", "War3ResetShadowSessionState", "PresentEx",
                "War3QuarantineShadowSessionAtPresent", "War3InvalidateShadowReceiverEpochOnCs",
                "War3GcShadowPersistentGeometry", "War3FindOrCreateShadowPersistentGeometry",
                "War3CreateShadowPersistentGeometryAfterMiss",
                "War3TryCaptureShadowCaster", "War3CollectRetiredShadowSessions",
                "War3TryPopulateSemanticShadowScene", "War3TryPopulateDrawTimeSemanticProducer",
            )
        }
        c = {name: compact(body) for name, body in functions.items()}
        prep = compact(PREPARE)
        report.check(compact(device).count(prep) == 3, "calls.exactly-three-prepared-transitions")
        report.check(len(re.findall(r"War3ResetShadowSessionState\([^;{}]*,\s*prepared\s*\)", device)) == 3,
                     "calls.all-three-reset-calls-pass-the-prepared-guard")
        for name, lane, ack in (
            ("War3ApplyShadowMapEpochResetAtPresent", "map", "m_war3ShadowMapResetAppliedSerial=requested;"),
            ("War3ApplyShadowDeviceEpochTransitionAtPresent", "device", "m_war3ShadowDeviceEpochApplied.store(requested,"),
        ):
            body = c[name]
            outer = compact(top_level(functions[name]))
            report.ordered(outer, f"{lane}.ready-closed-before-prepare", READY_OFF, prep)
            report.check("if(!prepared)returnfalse;" in body, f"{lane}.prepare-failure-returns-without-ack")
            report.ordered(body, f"{lane}.prepare-before-quarantine",
                           prep, "if(!prepared)returnfalse;", "War3QuarantineShadowSessionAtPresent(retireSerial);")
            report.ordered(body, f"{lane}.reset-before-ack",
                           "War3QuarantineShadowSessionAtPresent(retireSerial);",
                           "War3ResetShadowSessionState(retireSerial,prepared);",
                           "War3InvalidateShadowReceiverEpochOnCs(", ack)
            report.check(body.count(ack) == 1, f"{lane}.ack-has-one-post-reset-site")
            prefix = body[:body.find(prep)] if prep in body else body
            report.check(not re.search(r"m_war3ShadowMapResetAppliedSerial=|"
                                       r"m_war3ShadowDeviceEpochApplied\.store\(|"
                                       r"m_war3ShadowDeviceRebindPending\.store\(false,", prefix),
                         f"{lane}.no-map-device-or-rebind-ack-before-preparation")
        map_body = c["War3ApplyShadowMapEpochResetAtPresent"]
        report.ordered(map_body, "map.semantic-and-receiver-order-unchanged",
                       "War3QuarantineShadowSessionAtPresent(retireSerial);",
                       "War3LightningRuntime::instance().reset();",
                       "War3ResetShadowSessionState(retireSerial,prepared);",
                       "War3ResetGpuSkinMapEpoch();", "War3InvalidateShadowReceiverEpochOnCs(")
        report.check("if(requested==m_war3ShadowMapResetAppliedSerial)returnfalse;" in map_body,
                     "map.requested-applied-coalescing-preserved")
        report.ordered(map_body, "map.subsumed-device-ack-only-after-receiver",
                       "War3ResetShadowSessionState(retireSerial,prepared);",
                       "War3InvalidateShadowReceiverEpochOnCs(",
                       "m_war3ShadowDeviceEpochApplied.store(deviceEpoch,")
        device_body = c["War3ApplyShadowDeviceEpochTransitionAtPresent"]
        report.check("if(requested==0u||requested!=m_war3GpuSkinDeviceEpoch||!War3GpuSkinDeviceReady())" in device_body,
                     "device.committed-epoch-and-ready-domain-policy-preserved")
        report.ordered(device_body, "device.rebind-ack-after-receiver",
                       "War3InvalidateShadowReceiverEpochOnCs(", "m_war3ShadowDeviceEpochApplied.store(requested,",
                       "m_war3ShadowDeviceRebindPending.store(false,")

        drain = functions["War3DrainShadowCasterTombstones"]
        overflow, _, _ = braced(drain, r"\bif\s*\(\s*historyOverflowed\s*\)\s*\{")
        overflow_c = compact(overflow)
        fail, _, _ = braced(overflow, r"\bif\s*\(\s*!prepared\s*\)\s*\{")
        report.check(compact(fail) == "returnfalse;", "tombstone.failed-prepare-does-not-ack-or-clear-pending")
        report.ordered(overflow_c, "tombstone.pending-and-ready-before-prepare",
                       "m_war3ShadowTombstoneResetPending=true;", READY_OFF, prep,
                       "if(!prepared)", "War3ResetShadowSessionState(m_war3ShadowPersistentFrameSerial+1u,prepared);")
        report.ordered(compact(drain), "tombstone.seen-only-after-reset",
                       "War3ResetShadowSessionState(m_war3ShadowPersistentFrameSerial+1u,prepared);",
                       "m_war3ShadowTombstoneSerialSeen=publishedSerial;")
        report.check(compact(drain).count("m_war3ShadowTombstoneSerialSeen=publishedSerial;") == 1,
                     "tombstone.seen-has-one-ack-site")

        present = functions["PresentEx"]
        present_c = compact(present)
        pending_branch, _, _ = braced(present,
            r"if\s*\(\s*mapEpochTransitionPending\s*\|\|\s*deviceEpochTransitionPending\s*\|\|\s*m_war3ShadowTombstoneResetPending\s*\)\s*\{")
        report.check("RunWithActiveDevicePublication(" in compact(pending_branch),
                     "present.pending-transition-uses-active-device-owner")
        owner_lambda, _, _ = braced(pending_branch,
            r"\[this\s*,\s*retiringFrameSerial\s*,\s*&mapEpochTransition\s*,\s*&deviceEpochTransition\]\s*\(\)\s*\{")
        report.check("if(m_war3ShadowTombstoneResetPending)War3DrainShadowCasterTombstones();" in compact(owner_lambda),
                     "present.pending-tombstone-retries-inside-owner-safe-point")
        ready_expressions = re.findall(r"const\s+bool\s+resetFullyApplied\s*=\s*([^;]+);", present)
        expected_ready = ("!m_war3ShadowTombstoneResetPending&&"
                          "m_war3ShadowMapResetRequestedSerial.load(std::memory_order_acquire)==m_war3ShadowMapResetAppliedSerial&&"
                          "m_war3ShadowDeviceEpochRequested.load(std::memory_order_acquire)==m_war3ShadowDeviceEpochApplied.load(std::memory_order_acquire)&&"
                          "!m_war3ShadowDeviceRebindPending.load(std::memory_order_acquire)")
        report.check(len(ready_expressions) == 2 and all(compact(expr) == expected_ready for expr in ready_expressions),
                     "present.both-reopen-predicates-require-no-pending-and-complete-epochs")
        report.check("m_war3ShadowSessionReady.store(arenaReady&&resetFullyApplied,std::memory_order_release);" in present_c and
                     "m_war3ShadowSessionReady.store(resetFullyApplied,std::memory_order_release);" in present_c,
                     "present.both-ready-writes-use-the-gated-predicate")

        reset = functions["War3ResetShadowSessionState"]
        reset_c = compact(reset)
        transfer, _, transfer_end = braced(reset, r"\[this\]\s*\(War3RetiredShadowSession&\s*target\)\s*noexcept\s*\{")
        transfer_c = compact(transfer)
        call_tail = re.match(r"\}\s*\)\s*;", reset[transfer_end:])
        report.check(call_tail is not None, "owners.transfer-is-the-noexcept-commit-callback")
        commit_end = transfer_end + (call_tail.end() if call_tail else 0)
        report.check("prepared.commit(m_war3ShadowPersistentCharges,m_war3GpuSkinMapEpoch,retireSerial," in reset_c,
                     "ledger.reset-commits-current-live-charge-with-session-identity")
        session_body, _, _ = braced(header, r"\bstruct\s+War3RetiredShadowSession\s*\{")
        declared_owners = set()
        for declaration in session_body.split(";"):
            if re.search(r"\b(?:std::|War3)", declaration):
                tail = re.search(r"\b(\w+)\s*$", declaration)
                if tail:
                    declared_owners.add(tail.group(1))
        report.check(declared_owners == set(OWNERS), "owners.retired-payload-declares-exactly-all-eleven-owner-groups")
        direct_expected = {(target, source) for target, source in OWNERS.items()
                           if target not in ("shadowAllocators", "shadowMappedAllocators")}
        direct_actual = re.findall(r"TransferShadowSessionOwner\(target\.(\w+),(\w+)\);", transfer_c)
        report.check(len(direct_actual) == 9 and set(direct_actual) == direct_expected,
                     "owners.all-nine-direct-owner-transfers-are-complete-and-correct")
        for target in ("shadowAllocators", "shadowMappedAllocators"):
            loop, _, _ = braced(transfer, r"for\s*\([^{}]*target\." + target + r"\.size\(\)[^{}]*\)\s*\{")
            loop_c = compact(loop)
            report.check(f"auto&from={OWNERS[target]}[i];" in loop_c and f"auto&to=target.{target}[i];" in loop_c,
                         f"owners.{target}.each-index-pairs-the-correct-live-and-retired-allocator")
            report.ordered(loop_c, f"owners.{target}.chunks-before-audited-empty-Rc-move",
                           "TransferShadowSessionOwner(to.chunks,from.chunks);", "assert(to.currentChunk==nullptr);",
                           "to.currentChunk=std::move(from.currentChunk);", "to.currentOffset=std::exchange(from.currentOffset,0);")
            if target == "shadowMappedAllocators":
                report.check("to.currentMapPtr=std::exchange(from.currentMapPtr,nullptr);" in loop_c,
                             "owners.mapped-allocator-detaches-the-live-map-pointer")
        for cleanup in ("m_war3DrawTimeActiveLedger.resetSession();", "War3ResetStage11SnapshotPages();",
                        "War3ResetDirectPacketMapCaches();", "ResetCurrentDrawContractCache();"):
            position = reset.find(cleanup)
            report.check(position > commit_end, "reset.cleanup-after-commit:" + cleanup)
        for source in OWNERS.values():
            found = re.search(re.escape(source) + r"\s*=\s*\{\}\s*;", reset)
            report.check(found is not None and found.start() > commit_end,
                         "reset.live-owner-clear-after-commit:" + source)
        report.check("m_war3ShadowTombstoneResetPending=false;" in compact(reset[commit_end:]),
                     "reset.pending-clears-only-in-post-commit-cleanup")
        pending_clear = "m_war3ShadowTombstoneResetPending=false;"
        final_cleanup = "dxvk_war3_alpha_test_internal::ClearPayloadsForLifecycleOverflow();"
        report.check(reset_c.count(pending_clear) == 1 and final_cleanup in reset_c and
                     reset_c.rfind(final_cleanup) < reset_c.find(pending_clear),
                     "reset.pending-clears-only-after-final-lifecycle-payload-cleanup")
        report.check(not re.search(r"m_war3ShadowPersistentCharges\.(?:live|retired)\s*(?:=|\+=|-=)", reset),
                     "ledger.reset-does-not-zero-or-rewrite-live-retired-charges")

        # Drain belongs to the outer Populate function; the draw-time producer
        # is invoked later by this already-gated caller, not vice versa.
        populate = functions["War3TryPopulateSemanticShadowScene"]
        populate_c = compact(populate)
        drain_call = "War3DrainShadowCasterTombstones();"
        post_drain_guard = "if(!m_war3ShadowSessionReady.load(std::memory_order_acquire))return0u;"
        report.check(drain_call + post_drain_guard in compact(top_level(populate)),
                     "populate.Drain-immediately-rechecks-Ready-and-returns-before-any-reuse")
        guard_end = populate_c.find(post_drain_guard) + len(post_drain_guard)
        for reuse in ("m_war3SemanticSceneLastReusableFrame",
                      "populateReturnReason=kPopulateReturnSameFrameReuse;",
                      "populateReturnReason=kPopulateReturnStableSubmittedReuse;",
                      "War3TryPopulateDrawTimeSemanticProducer(exactSubmittedManifestRecords);"):
            report.check(post_drain_guard in populate_c and populate_c.find(reuse) >= guard_end,
                         "populate.post-Drain-guard-precedes-reuse:" + reuse)

        gc = c["War3GcShadowPersistentGeometry"]
        create = c["War3CreateShadowPersistentGeometryAfterMiss"]
        capture = c["War3TryCaptureShadowCaster"]
        collect = c["War3CollectRetiredShadowSessions"]
        report.check(gc.count("m_war3ShadowPersistentCharges.releaseLive(bytes);") == 2,
                     "ledger.both-live-GC-paths-release-only-live-charge")
        report.check("m_war3ShadowPersistentCharges.total()<=capBytes" in gc and
                     not re.search(r"m_war3ShadowPersistentCharges\.retired(?:=|\+=|-=)", gc),
                     "ledger.GC-uses-total-for-capacity-without-consuming-retired-charge")
        report.check("m_war3ShadowPersistentCharges.canAdmit(bytesNeeded,capBytes)" in create,
                     "ledger.shared-create-admission-counts-live-and-retired")
        report.check("returnWar3CreateShadowPersistentGeometryAfterMiss(domain,key,candidate,uploads,outGeometryId,outGeometry,outCreatedNew);"
                     in c["War3FindOrCreateShadowPersistentGeometry"],
                     "ledger.find-or-create-miss-delegates-to-the-shared-admission-path")
        report.check("m_war3ShadowPersistentCharges.canAdmit(fastBytesNeeded,persistentCapBytes)" in capture,
                     "ledger.fast-admission-counts-live-and-retired")
        report.check("if(!upload.retainSource)bytesNeeded+=War3AlignPersistentBytes(upload.bytes);" in create and
                     "if(!upload.retainSource)fastBytesNeeded+=War3AlignPersistentBytes(upload.bytes);" in capture,
                     "ledger.both-admission-byte-sums-preserve-zero-extra-charge-for-retained-sources")
        report.check(compact(device).count("m_war3ShadowPersistentCharges.canAdmit(") == 2,
                     "ledger.both-production-capacity-sites-use-the-common-ledger")
        report.check("PublishPersistentGeometry(m_war3ShadowPersistentGeometries,m_war3ShadowGeometryRegistry,"
                     "m_war3ShadowPersistentExpiryQueue,m_war3ShadowPersistentCharges.live," in create,
                     "ledger.publication-charges-only-live-after-transaction")
        report.check("CollectRetiredShadowSessions(m_war3RetiredShadowSessions,m_war3ShadowPersistentCharges,completedSerial)" in collect,
                     "ledger.collection-uses-the-shared-retired-charge-and-completion-function")
        report.check("m_war3ShadowPersistentBytesUsed" not in device + header and
                     "std::list<War3RetiredShadowSession>m_war3RetiredShadowSessions;" in compact(header) and
                     "PersistentGeometryChargesm_war3ShadowPersistentCharges;" in compact(header),
                     "ledger.stable-list-and-two-part-charge-replace-the-old-live-only-field")

        quarantine = c["War3QuarantineShadowSessionAtPresent"]
        report.ordered(quarantine, "fence.quarantine-precedes-CS-signal-and-serial-publication",
                       "ShadowArena_QuarantineCurrentGeneration(retireSerial);", "EmitCs(",
                       "ctx->signal(cShadowArenaFence,cRetireSerial);",
                       "m_war3ShadowArenaQuarantinedRetireSerial=retireSerial;")
        report.check("if(dxvk::war3::memory::ShadowArena_IsOwnedBy(m_dxvkDevice.ptr()))"
                     "dxvk::war3::memory::ShadowArena_QuarantineCurrentGeneration(retireSerial);" in quarantine,
                     "fence.quarantine-keeps-the-existing-device-owner-guard")
        report.check("EmitCs([receiver,mapEpoch,deviceEpoch](DxvkContext*){"
                     "receiver->InvalidateMapEpoch(mapEpoch,deviceEpoch);});"
                     in c["War3InvalidateShadowReceiverEpochOnCs"],
                     "fence.receiver-invalidation-remains-CS-ordered-not-an-immediate-call")
        report.ordered(present_c, "fence.Present-end-signal-collect-begin-order-preserved",
                       "ShadowArena_EndFrame(retiringFrameSerial);", "ctx->signal(cShadowArenaFence,cRetiringFrameSerial);",
                       "War3CollectRetiredShadowSessions(completedSerial);", "ShadowArena_BeginFrame(nextFrameSerial,completedSerial)")
        report.check("m_war3ShadowArenaFence!=nullptr?m_war3ShadowArenaFence->value():0u" in present_c,
                     "fence.capture-enabled-collection-uses-the-real-fence-value-or-zero")
        report.ordered(reset_c, "domain.reset-owner-audit-precedes-retirement-commit",
                       "ShadowGeometryOwnerAccepts(slot.domain,slot.geometryId,geomIt->second.domain,geomIt->first)",
                       "prepared.commit(")
        report.ordered(create, "domain.publication-owner-check-remains-before-GC-and-capacity",
                       "DecideShadowGeometryOwner(domain,0u,existingSlotIt->second.domain,existingSlotIt->second.geometryId)",
                       "War3GcShadowPersistentGeometry();", "m_war3ShadowPersistentCharges.canAdmit(")
        report.check("DecideShadowGeometryOwner(domain,geometryId,slotIt->second.domain,slotIt->second.geometryId)=="
                     "dxvk::war3::shadow::ShadowGeometryOwnerDecision::Accept" in gc,
                     "domain.GC-still-checks-both-domain-and-geometry-ID-before-slot-erase")
        helper = compact(retirement)
        report.check("static_assert(std::is_nothrow_move_assignable_v<T>," in helper and
                     "destination=std::move(source);" in helper,
                     "owners.common-owner-transfer-enforces-nothrow-move-assignment")
        report.check("usingQueue=std::list<Session,Allocator>;" in helper and
                     "if(m_prepared&&!m_committed)m_queue.erase(m_slot);" in helper,
                     "helper.uncommitted-list-node-is-guarded-without-moving-older-sessions")
        report.check("it->retireSerial==0||it->retireSerial>completedSerial||it->persistentChargeBytes>charges.retired" in helper,
                     "helper.unknown-incomplete-or-inconsistent-charge-cannot-release-owners")
    except AssertionError as error:
        report.check(False, "syntax.production-boundary:" + str(error))
    return report


def change_function(source: str, name: str, edit) -> str:
    body, start, end = function(source, name)
    changed = edit(body)
    if changed == body:
        raise AssertionError("mutation did not change " + name)
    return source[:start] + changed + source[end:]


def replace_once(source: str, pattern: str, replacement: str = "") -> str:
    changed, count = re.subn(pattern, lambda _: replacement, source, count=1)
    if count != 1:
        raise AssertionError("mutation target missing: " + pattern)
    return changed


def move_before(source: str, statement: str, anchor: str) -> str:
    match = re.search(statement, source)
    if match is None:
        raise AssertionError("move target missing: " + statement)
    value = match.group()
    source = source[:match.start()] + source[match.end():]
    match = re.search(anchor, source)
    if match is None:
        raise AssertionError("move anchor missing: " + anchor)
    return source[:match.start()] + value + "\n" + source[match.start():]


def mutation_suite(device_raw: str, header_raw: str, retirement_raw: str) -> tuple[int, int]:
    source = code_only(device_raw)
    map_name = "War3ApplyShadowMapEpochResetAtPresent"
    drain_name = "War3DrainShadowCasterTombstones"
    mutants = [
        ("map-prepare-failure-no-return", map_name,
         lambda b: replace_once(b, r"if\s*\(!prepared\)\s*return\s+false\s*;"), "map.prepare-failure"),
        ("quarantine-before-prepare", map_name,
         lambda b: move_before(b, r"War3QuarantineShadowSessionAtPresent\(retireSerial\);", r"War3PreparedShadowRetirement\s+prepared"),
         "map.prepare-before-quarantine"),
        ("map-ack-before-reset", map_name,
         lambda b: move_before(b, r"m_war3ShadowMapResetAppliedSerial\s*=\s*requested\s*;", r"War3PreparedShadowRetirement\s+prepared"),
         "map.reset-before-ack"),
        ("tombstone-pending-not-latched", drain_name,
         lambda b: replace_once(b, r"m_war3ShadowTombstoneResetPending\s*=\s*true\s*;"), "tombstone.pending-and-ready"),
        ("tombstone-seen-before-reset", drain_name,
         lambda b: move_before(b, r"m_war3ShadowTombstoneSerialSeen\s*=\s*publishedSerial\s*;", r"War3ResetShadowSessionState\("),
         "tombstone.seen-only-after-reset"),
        ("Present-pending-retry-disconnected", "PresentEx",
         lambda b: replace_once(b, r"if\s*\(m_war3ShadowTombstoneResetPending\)\s*War3DrainShadowCasterTombstones\(\)\s*;"),
         "present.pending-tombstone-retries"),
        ("one-reopen-predicate-forgets-pending", "PresentEx",
         lambda b: replace_once(b, r"!m_war3ShadowTombstoneResetPending\s*&&"), "present.both-reopen-predicates"),
        ("persistent-owner-transfer-disconnected", "War3ResetShadowSessionState",
         lambda b: replace_once(b, r"TransferShadowSessionOwner\(target\.persistentGeometries,\s*m_war3ShadowPersistentGeometries\);"),
         "owners.all-nine-direct-owner-transfers"),
        ("cleanup-moved-before-commit", "War3ResetShadowSessionState",
         lambda b: move_before(b, r"m_war3DrawTimeActiveLedger\.resetSession\(\);", r"auto&\s+retired\s*=\s*prepared\.commit"),
         "reset.cleanup-after-commit"),
        ("fast-capacity-bypasses-retired-charge", "War3TryCaptureShadowCaster",
         lambda b: replace_once(b, r"m_war3ShadowPersistentCharges\.canAdmit\(\s*fastBytesNeeded,\s*persistentCapBytes\)",
                                "(fastBytesNeeded <= persistentCapBytes)"), "ledger.fast-admission"),
        ("pending-cleared-before-final-cleanup", "War3ResetShadowSessionState",
         lambda b: move_before(b, r"m_war3ShadowTombstoneResetPending\s*=\s*false\s*;",
                              r"m_war3DrawTimeActiveLedger\.resetSession\(\);"),
         "reset.pending-clears-only-after-final-lifecycle-payload-cleanup"),
        ("post-Drain-Ready-check-deleted", "War3TryPopulateSemanticShadowScene",
         lambda b: replace_once(b, r"War3DrainShadowCasterTombstones\(\);\s*"
                                r"if\s*\(!m_war3ShadowSessionReady\.load\(std::memory_order_acquire\)\)\s*return\s+0u\s*;",
                                "War3DrainShadowCasterTombstones();"),
         "populate.Drain-immediately-rechecks-Ready"),
    ]
    killed = 0
    for name, function_name, edit, expected_gate in mutants:
        try:
            mutant = change_function(source, function_name, edit)
            result = audit(mutant, header_raw, retirement_raw)
            rejected = any(label.startswith(expected_gate) for label in result.failures)
        except AssertionError as error:
            rejected = False
            print(f"MUTATION_SETUP_FAILURE {name}: {error}")
        killed += bool(rejected)
        print(f"MUTATION {name}: {'REJECTED' if rejected else 'SURVIVED'}")
    return killed, len(mutants)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true", help="exercise twelve mutations only in memory")
    args = parser.parse_args()
    sources = tuple(path.read_text(encoding="utf-8") for path in (DEVICE, DEVICE_HEADER, RETIREMENT))
    result = audit(*sources)
    for failure in result.failures:
        print("FAIL", failure)
    print(f"SHADOW_SESSION_RETIREMENT_STATIC checks={result.checks} failures={len(result.failures)} SOURCE_ONLY")
    if result.failures:
        return 1
    if args.mutations:
        killed, total = mutation_suite(*sources)
        print(f"MEMORY_MUTATIONS rejected={killed}/{total} productionFilesWritten=0")
        if killed != total:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
