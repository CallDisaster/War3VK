#include "../war3_persistent_gpu_package_recording_authority.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_fault {
std::atomic<bool> failNext {false};
}

void* operator new(std::size_t size) {
  if (allocation_fault::failNext.exchange(false, std::memory_order_acq_rel))
    throw std::bad_alloc();
  if (void* allocation = std::malloc(size != 0u ? size : 1u))
    return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept {
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

namespace {

using Authority =
    dxvk::war3::gpu_skin::War3PersistentGpuPackageRecordingAuthority;
using Catalog = Authority::ProofCatalog;

void hashU64(uint64_t& hash, uint64_t value) {
  for (uint32_t byte = 0u; byte < 8u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= 0x100000001b3ull;
  }
}

void hashU32(uint64_t& hash, uint32_t value) {
  for (uint32_t byte = 0u; byte < 4u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= 0x100000001b3ull;
  }
}

uint64_t primitiveAggregate(const Catalog::PreparedPackage& prepared) {
  const auto& primitive = prepared.primitiveProof;
  uint64_t hash = 0xcbf29ce484222325ull;
  hashU64(hash, primitive.indexContentHash);
  hashU32(hash, primitive.ordinal);
  hashU32(hash, primitive.primitiveTypeOrMaterialSlot);
  hashU32(hash, primitive.firstIndex);
  hashU32(hash, primitive.indexCount);
  hashU32(hash, primitive.minVertex);
  hashU32(hash, primitive.maxVertex);
  return hash;
}

Authority::RecordingContext makeContext(uint64_t ownerSubmissionSerial) {
  Authority::RecordingContext context;
  context.mapEpoch = 3u;
  context.deviceEpoch = 5u;
  context.frameSerial = 101u + ownerSubmissionSerial;
  context.policyRevision = 7u;
  context.stageGeneration = 11u + ownerSubmissionSerial;
  context.recordingOwnerToken = 13u;
  context.recordingSessionGeneration = 17u + ownerSubmissionSerial;
  context.commandListGeneration = 19u + ownerSubmissionSerial;
  context.emitCsSerial = 23u + ownerSubmissionSerial;
  context.canonicalBatchToken = 29u + ownerSubmissionSerial;
  context.ownerSubmissionSerial = ownerSubmissionSerial;
  context.stage = Authority::kRequiredStage;
  return context;
}

Catalog::PreparedPackage makePreparedPackage(
    const Authority::RecordingContext& context, uint64_t salt) {
  Catalog::PreparedPackage prepared = {};
  prepared.key.schema = Catalog::kSchemaVersion;
  prepared.key.mapEpoch = context.mapEpoch;
  prepared.key.deviceEpoch = context.deviceEpoch;
  prepared.key.packageGeneration = 101u + salt;
  prepared.key.immutableModelGeneration = 103u + salt;
  prepared.key.geosetData = uintptr_t(0x123400u + salt * 0x100u);
  prepared.key.contentHash = 107u + salt;
  prepared.key.layoutGeneration = 109u;
  prepared.key.primitiveOrdinal = Authority::kGrantedPrimitiveOrdinal;

  auto& proof = prepared.packageProof;
  proof.mapEpoch = prepared.key.mapEpoch;
  proof.deviceEpoch = prepared.key.deviceEpoch;
  proof.packageGeneration = prepared.key.packageGeneration;
  proof.geosetData = prepared.key.geosetData;
  proof.contentHash = prepared.key.contentHash;
  proof.immutableModelGeneration = prepared.key.immutableModelGeneration;
  proof.positionContentHash = 113u + salt;
  proof.normalContentHash = 127u + salt;
  proof.vertexGroupContentHash = 131u + salt;
  proof.uv0ContentHash = 137u + salt;
  proof.uv1ContentHash = 0u;
  proof.indexContentHash = 139u + salt;
  proof.localBoundsHash = 149u + salt;
  proof.layoutGeneration = prepared.key.layoutGeneration;
  proof.vertexCount = 4u;
  proof.indexCount = 3u;
  proof.uvLayerCount = 1u;
  proof.primitiveProofCount = 1u;
  proof.indexType = VK_INDEX_TYPE_UINT16;
  proof.localMinX = -1.0f;
  proof.localMinY = -2.0f;
  proof.localMinZ = -3.0f;
  proof.localMaxX = 1.0f;
  proof.localMaxY = 2.0f;
  proof.localMaxZ = 3.0f;
  proof.staticByteOffset = 64u;
  proof.staticByteLength = 132u;
  proof.indexByteOffset = 224u;
  proof.indexByteLength = 6u;

  auto& primitive = prepared.primitiveProof;
  primitive.indexContentHash = proof.indexContentHash;
  primitive.ordinal = Authority::kGrantedPrimitiveOrdinal;
  primitive.primitiveTypeOrMaterialSlot = 151u + uint32_t(salt);
  primitive.firstIndex = 0u;
  primitive.indexCount = 3u;
  primitive.minVertex = 0u;
  primitive.maxVertex = 3u;
  proof.primitiveProofHash = primitiveAggregate(prepared);
  return prepared;
}

struct ReadyProof {
  Catalog::PreparedPackage prepared;
  Catalog::SharedSnapshot snapshot;
  Catalog::DrawEvidence evidence;
  Catalog::PackageContentDecision decision;
};

ReadyProof makeReadyProof(
    const Authority::RecordingContext& context, uint64_t salt = 0u) {
  Catalog catalog;
  ReadyProof proof;
  proof.prepared = makePreparedPackage(context, salt);
  assert(catalog.publishPrepared(proof.prepared) ==
         Catalog::MutationResult::Accepted);
  const Catalog::ProducerFencePoint fence {157u + salt, 163u + salt};
  assert(catalog.publishUploadSubmitted(proof.prepared.key, fence) ==
         Catalog::MutationResult::Accepted);
  assert(catalog.publishUploadCompleted(
             proof.prepared.key,
             {fence.identity, fence.value, true}) ==
         Catalog::MutationResult::Accepted);
  proof.snapshot = catalog.snapshot();
  const Catalog::Entry* entry = proof.snapshot->find(proof.prepared.key);
  assert(entry != nullptr);

  auto& evidence = proof.evidence;
  evidence.frameSerial = context.frameSerial;
  evidence.policyRevision = context.policyRevision;
  evidence.stage = context.stage;
  evidence.catalogInstanceGeneration = proof.snapshot->instanceGeneration();
  evidence.catalogSnapshotRevision = proof.snapshot->revision();
  evidence.canonicalDigest = entry->value.canonicalDigest;
  evidence.currentDrawSourceGeneration = 167u + salt;
  evidence.key = proof.prepared.key;
  evidence.packageProof = proof.prepared.packageProof;
  evidence.primitiveProof = proof.prepared.primitiveProof;
  evidence.streamHashes.position = proof.prepared.packageProof.positionContentHash;
  evidence.streamHashes.normal = proof.prepared.packageProof.normalContentHash;
  evidence.streamHashes.vertexGroup =
      proof.prepared.packageProof.vertexGroupContentHash;
  evidence.streamHashes.uv0 = proof.prepared.packageProof.uv0ContentHash;
  evidence.streamHashes.uv1 = proof.prepared.packageProof.uv1ContentHash;
  evidence.streamHashes.index = proof.prepared.packageProof.indexContentHash;
  evidence.streamHashes.primitiveAggregate =
      proof.prepared.packageProof.primitiveProofHash;
  evidence.streamHashes.localBounds =
      proof.prepared.packageProof.localBoundsHash;
  evidence.primitiveDomain.firstIndex = proof.prepared.primitiveProof.firstIndex;
  evidence.primitiveDomain.indexCount = proof.prepared.primitiveProof.indexCount;
  evidence.primitiveDomain.minVertex = proof.prepared.primitiveProof.minVertex;
  evidence.primitiveDomain.maxVertex = proof.prepared.primitiveProof.maxVertex;
  evidence.primitiveDomain.wholeIndexCount = proof.prepared.packageProof.indexCount;
  evidence.primitiveDomain.vertexCount = proof.prepared.packageProof.vertexCount;
  evidence.primitiveDomain.indexType = proof.prepared.packageProof.indexType;
  evidence.identity = {173u + salt, 173u + salt};
  evidence.source = {179u + salt, 179u + salt};
  evidence.material = {181u + salt, 181u + salt};
  evidence.alpha = {191u + salt, 191u + salt};
  evidence.world = {193u + salt, 193u + salt};
  evidence.bounds = {197u + salt, 197u + salt};
  evidence.identityExact = true;
  evidence.sourceExact = true;
  evidence.staticRigid = true;
  evidence.fresh = true;

  proof.decision = Catalog::validateDrawEvidence(
      proof.snapshot,
      {context.frameSerial, context.policyRevision, context.stage},
      evidence);
  assert(proof.decision.ready());
  assert(proof.decision.matches(
      proof.prepared.key, proof.snapshot->instanceGeneration(),
      proof.snapshot->revision(), context.frameSerial, context.policyRevision,
      context.stage, evidence.identity.drawToken, evidence.source.drawToken,
      evidence.currentDrawSourceGeneration, evidence.material.drawToken,
      evidence.alpha.drawToken, evidence.world.drawToken,
      evidence.bounds.drawToken));
  return proof;
}

Authority::RecordInput makeRecord(
    const Authority::RecordingContext& context,
    const ReadyProof& proof, uint32_t ordinal, uint64_t salt = 0u) {
  Authority::RecordInput input;
  input.context = context;
  input.source.mapEpoch = proof.prepared.key.mapEpoch;
  input.source.deviceEpoch = proof.prepared.key.deviceEpoch;
  input.source.frameSerial = context.frameSerial;
  input.source.sourceFrameSerial = context.frameSerial;
  input.source.evidenceFrameSerial = context.frameSerial;
  input.source.policyRevision = context.policyRevision;
  input.source.stageGeneration = context.stageGeneration;
  input.source.currentDrawSourceGeneration =
      proof.evidence.currentDrawSourceGeneration;
  input.source.immutableModelGeneration =
      proof.prepared.key.immutableModelGeneration;
  input.source.packageGeneration = proof.prepared.key.packageGeneration;
  input.source.packageContentHash = proof.prepared.key.contentHash;
  input.source.geosetDataIdentity = proof.prepared.key.geosetData;
  input.source.storeInstanceAuthority = 42u + ordinal + salt;
  input.source.frozenDescriptorIdentity =
      static_cast<uintptr_t>(0x1000u + ordinal + salt);
  input.source.cacheSnapshotIdentity =
      static_cast<uintptr_t>(0x2000u + ordinal + salt);
  input.source.catalogSnapshot = proof.snapshot;
  input.source.packageContentDecision = proof.decision;
  input.source.catalogInstanceGeneration = proof.snapshot->instanceGeneration();
  input.source.catalogSnapshotRevision = proof.snapshot->revision();
  input.source.packagePublicationRevision = proof.decision.publicationRevision();
  input.source.packageCanonicalDigest = proof.decision.canonicalDigest();
  input.source.identityToken = proof.evidence.identity.drawToken;
  input.source.sourceToken = proof.evidence.source.drawToken;
  input.source.materialToken = proof.evidence.material.drawToken;
  input.source.alphaToken = proof.evidence.alpha.drawToken;
  input.source.worldToken = proof.evidence.world.drawToken;
  input.source.boundsToken = proof.evidence.bounds.drawToken;
  input.source.packageSchema = proof.prepared.key.schema;
  input.source.packageLayoutGeneration = proof.prepared.key.layoutGeneration;
  input.source.packagePrimitiveOrdinal = proof.prepared.key.primitiveOrdinal;
  input.source.stage = context.stage;
  input.source.captureComplete = true;
  input.source.exactCurrentAllocation = true;
  input.source.fresh = true;
  input.source.grace = false;
  input.source.rejected = false;
  input.recordIdentityToken = 71u + ordinal + salt;
  input.commandIdentityToken = 73u + ordinal + salt;
  input.commandPayloadDigest = 79u + ordinal + salt;
  input.ordinal = ordinal;
  input.primitiveOrdinal = Authority::kGrantedPrimitiveOrdinal;
  input.consumerMask = 1u << (ordinal % 7u);
  return input;
}

Authority::RecordInput makeRecord(
    const Authority::RecordingContext& context,
    uint32_t ordinal, uint64_t salt = 0u) {
  const ReadyProof proof = makeReadyProof(context, salt);
  return makeRecord(context, proof, ordinal, salt);
}

uint64_t expectedDigest(
    const Authority::RecordingContext& context,
    const std::vector<Authority::RecordInput>& records) {
  uint64_t digest = Authority::beginRecordDigest(
      context, static_cast<uint32_t>(records.size()));
  for (const Authority::RecordInput& record : records)
    digest = Authority::appendRecordDigest(digest, record);
  return digest;
}

struct Batch {
  Authority::RecordingContext context;
  std::vector<Authority::RecordInput> records;
  Authority::CreateInput create;
};

Batch makeBatch(uint64_t ownerSubmissionSerial, uint32_t count) {
  Batch batch;
  batch.context = makeContext(ownerSubmissionSerial);
  const ReadyProof proof = makeReadyProof(batch.context, ownerSubmissionSerial);
  batch.records.reserve(count);
  for (uint32_t i = 0u; i < count; i++)
    batch.records.push_back(makeRecord(batch.context, proof, i));
  batch.create.context = batch.context;
  batch.create.expectedRecords = batch.records;
  batch.create.expectedRecordCount = count;
  batch.create.expectedRecordDigest = expectedDigest(
      batch.context, batch.records);
  return batch;
}

struct EmitCapture {
  std::atomic<uint32_t> calls {0u};
  Authority::RecordingContext expectedContext;
  uint32_t expectedCount = 0u;
  uint64_t expectedDigest = 0u;
  uint64_t expectedFirstPayloadDigest = 0u;
  bool succeed = true;
};

bool captureEmit(
    void* opaque, const Authority::SealedBatchView& view) {
  auto* capture = static_cast<EmitCapture*>(opaque);
  capture->calls.fetch_add(1u, std::memory_order_relaxed);
  assert(view.context.ownerSubmissionSerial ==
         capture->expectedContext.ownerSubmissionSerial);
  assert(view.context.commandListGeneration ==
         capture->expectedContext.commandListGeneration);
  assert(view.context.stageGeneration ==
         capture->expectedContext.stageGeneration);
  assert(view.recordCount == capture->expectedCount);
  assert(view.recordDigest == capture->expectedDigest);
  assert(view.commandPlan.size() == view.recordCount);
  assert(!view.commandPlan.empty());
  assert(view.commandPlan.data() != nullptr);
  assert(view.commandPlan.record(view.recordCount) == nullptr);
  for (uint32_t ordinal = 0u; ordinal < view.commandPlan.size(); ++ordinal) {
    const Authority::RecordInput* record = view.commandPlan.record(ordinal);
    assert(record != nullptr);
    assert(record->ordinal == ordinal);
    assert(record->source.catalogSnapshot != nullptr);
    assert(record->source.packageContentDecision.ready());
    Catalog::Key key;
    key.schema = record->source.packageSchema;
    key.mapEpoch = record->source.mapEpoch;
    key.deviceEpoch = record->source.deviceEpoch;
    key.packageGeneration = record->source.packageGeneration;
    key.immutableModelGeneration = record->source.immutableModelGeneration;
    key.geosetData = record->source.geosetDataIdentity;
    key.contentHash = record->source.packageContentHash;
    key.layoutGeneration = record->source.packageLayoutGeneration;
    key.primitiveOrdinal = record->source.packagePrimitiveOrdinal;
    assert(record->source.packageContentDecision.matches(
        key, record->source.catalogSnapshot->instanceGeneration(),
        record->source.catalogSnapshot->revision(),
        record->source.frameSerial, record->source.policyRevision,
        record->source.stage, record->source.identityToken,
        record->source.sourceToken,
        record->source.currentDrawSourceGeneration,
        record->source.materialToken, record->source.alphaToken,
        record->source.worldToken, record->source.boundsToken));
  }
  if (capture->expectedFirstPayloadDigest != 0u) {
    assert(view.commandPlan.record(0u)->commandPayloadDigest ==
           capture->expectedFirstPayloadDigest);
  }
  assert(view.authorityInstanceGeneration != 0u);
  assert(view.transactionGeneration != 0u);
  assert(view.sealGeneration != 0u);
  return capture->succeed;
}

Authority::SealedTicket prepareSealed(
    Authority& authority, const Batch& batch,
    Authority::TransactionTicket* transactionOut = nullptr) {
  const Authority::CreateDecision created = authority.create(batch.create);
  assert(created.result == Authority::CreateResult::Created);
  assert(created.ticket.valid());
  for (const Authority::RecordInput& record : batch.records)
    assert(authority.record(created.ticket, record) ==
           Authority::RecordResult::Recorded);
  const Authority::SealDecision sealed = authority.seal(
      created.ticket, batch.context);
  assert(sealed.result == Authority::SealResult::Sealed);
  assert(sealed.ticket.valid());
  if (transactionOut != nullptr)
    *transactionOut = created.ticket;
  return sealed.ticket;
}

void testValidOneShotPath() {
  static_assert(!std::is_constructible_v<
      Authority::ImmutableCommandPlanView,
      const Authority::RecordInput*, uint32_t>);
  static_assert(!std::is_nothrow_invocable_r_v<
      bool, Authority::EmitCallback, void*,
      const Authority::SealedBatchView&>);
  Authority authority;
  const Batch batch = makeBatch(1u, 3u);
  Authority::TransactionTicket transaction;
  const Authority::SealedTicket sealed = prepareSealed(
      authority, batch, &transaction);

  EmitCapture capture;
  capture.expectedContext = batch.context;
  capture.expectedCount = static_cast<uint32_t>(batch.records.size());
  capture.expectedDigest = batch.create.expectedRecordDigest;
  assert(authority.emitSealed(
             sealed, batch.context, &captureEmit, &capture) ==
         Authority::EmitResult::Emitted);
  assert(capture.calls.load(std::memory_order_relaxed) == 1u);
  assert(authority.emitSealed(
             sealed, batch.context, &captureEmit, &capture) ==
         Authority::EmitResult::AlreadyTerminal);
  assert(authority.abort(transaction) ==
         Authority::AbortResult::AlreadyTerminal);

  const Authority::Snapshot snapshot = authority.snapshot();
  assert(snapshot.state == Authority::State::Emitted);
  assert(snapshot.boundRecordingOwnerToken ==
         batch.context.recordingOwnerToken);
  assert(snapshot.recordCount == 3u);
  assert(snapshot.recordDigest == batch.create.expectedRecordDigest);
  const Authority::Diagnostics diagnostics = authority.diagnostics();
  assert(diagnostics.created == 1u);
  assert(diagnostics.recordsAccepted == 3u);
  assert(diagnostics.sealsAccepted == 1u);
  assert(diagnostics.callbacksStarted == 1u);
  assert(diagnostics.callbacksSucceeded == 1u);
}

void testP1ReadyDecisionCannotBeForgedFromRawFields() {
  using Decision = Catalog::PackageContentDecision;
  using Reason = Decision::Reason;
  static_assert(!std::is_aggregate_v<Decision>);
  static_assert(!std::is_constructible_v<
      Decision, bool, Reason, uint64_t, uint64_t, uint64_t,
      Catalog::Key, uint64_t, uint64_t, uint32_t, uint64_t,
      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>);

  {
    Authority authority;
    Batch forged = makeBatch(1u, 1u);
    // Every old public POD field remains plausible and non-zero. The default
    // decision is nevertheless not Ready and cannot grant recording.
    forged.records[0].source.packageContentDecision = Decision{};
    forged.create.expectedRecords = forged.records;
    forged.create.expectedRecordDigest = expectedDigest(
        forged.context, forged.records);
    assert(authority.create(forged.create).result ==
           Authority::CreateResult::InvalidExpectedBatch);
  }
  {
    Authority authority;
    Batch batch = makeBatch(1u, 1u);
    const Batch other = makeBatch(2u, 1u);
    // A genuine Ready decision for another immutable snapshot/tuple is not a
    // transferable capability.
    batch.records[0].source.packageContentDecision =
        other.records[0].source.packageContentDecision;
    batch.create.expectedRecords = batch.records;
    batch.create.expectedRecordDigest = expectedDigest(
        batch.context, batch.records);
    assert(authority.create(batch.create).result ==
           Authority::CreateResult::InvalidExpectedBatch);
  }
}

void testSealedPlanRetainsAuthorityOwnedImmutableRecords() {
  Authority authority;
  Batch batch = makeBatch(1u, 2u);
  const uint64_t frozenPayload = batch.records[0].commandPayloadDigest;
  const Authority::SealedTicket sealed = prepareSealed(authority, batch);

  // Mutate both caller-owned copies after seal. EmitCs must expose the
  // authority's retained copy, not either mutable external vector.
  batch.records[0].commandPayloadDigest ^= 0x55aa55aaull;
  batch.records[0].source.catalogSnapshot.reset();
  batch.records[0].source.packageContentDecision = {};
  batch.create.expectedRecords[0].commandPayloadDigest ^= 0xaa55aa55ull;
  batch.create.expectedRecords[0].source.catalogSnapshot.reset();
  batch.create.expectedRecords[0].source.packageContentDecision = {};

  EmitCapture capture;
  capture.expectedContext = batch.context;
  capture.expectedCount = 2u;
  capture.expectedDigest = batch.create.expectedRecordDigest;
  capture.expectedFirstPayloadDigest = frozenPayload;
  assert(authority.emitSealed(
             sealed, batch.context, &captureEmit, &capture) ==
         Authority::EmitResult::Emitted);
  assert(capture.calls.load(std::memory_order_relaxed) == 1u);
}

void testCapacityBoundaryAndAllocationFailure() {
  static_assert(Authority::kMaxRecords == 4096u);
  {
    Authority authority;
    const Batch maximum = makeBatch(1u, Authority::kMaxRecords);
    const Authority::CreateDecision created = authority.create(maximum.create);
    assert(created.result == Authority::CreateResult::Created);
    for (const Authority::RecordInput& record : maximum.records)
      assert(authority.record(created.ticket, record) ==
             Authority::RecordResult::Recorded);
    const Authority::SealDecision sealed = authority.seal(
        created.ticket, maximum.context);
    assert(sealed.result == Authority::SealResult::Sealed);
    EmitCapture capture;
    capture.expectedContext = maximum.context;
    capture.expectedCount = Authority::kMaxRecords;
    capture.expectedDigest = maximum.create.expectedRecordDigest;
    assert(authority.emitSealed(
               sealed.ticket, maximum.context, &captureEmit, &capture) ==
           Authority::EmitResult::Emitted);
  }
  {
    Authority authority;
    const Batch over = makeBatch(1u, Authority::kMaxRecords + 1u);
    assert(authority.create(over.create).result ==
           Authority::CreateResult::InvalidExpectedBatch);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    allocation_fault::failNext.store(true, std::memory_order_release);
    const Authority::CreateDecision failed = authority.create(batch.create);
    assert(failed.result == Authority::CreateResult::AllocationFailed);
    assert(!allocation_fault::failNext.load(std::memory_order_acquire));
    assert(authority.snapshot().state == Authority::State::Idle);
    const Authority::CreateDecision recovered = authority.create(batch.create);
    assert(recovered.result == Authority::CreateResult::Created);
    assert(authority.abort(recovered.ticket) == Authority::AbortResult::Aborted);
  }
}

void testCreateValidationAndMonotonicOwner() {
  Authority authority;
  Batch invalid = makeBatch(1u, 1u);
  invalid.create.context.stage = 10u;
  assert(authority.create(invalid.create).result ==
         Authority::CreateResult::InvalidContext);

  invalid = makeBatch(1u, 1u);
  invalid.create.expectedRecordCount = 0u;
  assert(authority.create(invalid.create).result ==
         Authority::CreateResult::InvalidExpectedBatch);
  invalid = makeBatch(1u, 1u);
  invalid.create.expectedRecordDigest = 0u;
  assert(authority.create(invalid.create).result ==
         Authority::CreateResult::InvalidExpectedBatch);

  const Batch first = makeBatch(1u, 1u);
  const Authority::CreateDecision created = authority.create(first.create);
  assert(created.result == Authority::CreateResult::Created);
  assert(authority.create(makeBatch(2u, 1u).create).result ==
         Authority::CreateResult::Busy);
  assert(authority.abort(created.ticket) == Authority::AbortResult::Aborted);
  assert(authority.create(first.create).result ==
         Authority::CreateResult::StaleOwnerSubmission);
  const Authority::CreateDecision second = authority.create(
      makeBatch(2u, 1u).create);
  assert(second.result == Authority::CreateResult::Created);
  assert(second.ticket.transactionGeneration() >
         created.ticket.transactionGeneration());
  assert(authority.abort(second.ticket) == Authority::AbortResult::Aborted);

  Batch changedOwner = makeBatch(3u, 1u);
  changedOwner.context.recordingOwnerToken += 1u;
  changedOwner.records[0] = makeRecord(changedOwner.context, 0u);
  changedOwner.create.context = changedOwner.context;
  changedOwner.create.expectedRecords = changedOwner.records;
  changedOwner.create.expectedRecordDigest = expectedDigest(
      changedOwner.context, changedOwner.records);
  assert(authority.create(changedOwner.create).result ==
         Authority::CreateResult::WrongRecordingOwner);

  const Batch third = makeBatch(3u, 1u);
  std::atomic<uint32_t> foreignCreateResult {0u};
  std::thread foreignOwner([&] {
    foreignCreateResult.store(
        static_cast<uint32_t>(authority.create(third.create).result),
        std::memory_order_release);
  });
  foreignOwner.join();
  assert(static_cast<Authority::CreateResult>(
             foreignCreateResult.load(std::memory_order_acquire)) ==
         Authority::CreateResult::WrongRecordingOwner);
  const Authority::CreateDecision thirdCreated = authority.create(
      third.create);
  assert(thirdCreated.result == Authority::CreateResult::Created);
  assert(authority.abort(thirdCreated.ticket) ==
         Authority::AbortResult::Aborted);
}

void testCurrentStageSourceFailClosed() {
  for (uint32_t variant = 0u; variant < 32u; variant++) {
    Authority authority;
    Batch batch = makeBatch(1u, 1u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(created.result == Authority::CreateResult::Created);
    Authority::RecordInput broken = batch.records.front();
    switch (variant) {
      case 0u: broken.source.mapEpoch += 1u; break;
      case 1u: broken.source.deviceEpoch += 1u; break;
      case 2u: broken.source.frameSerial += 1u; break;
      case 3u: broken.source.sourceFrameSerial += 1u; break;
      case 4u: broken.source.evidenceFrameSerial += 1u; break;
      case 5u: broken.source.policyRevision += 1u; break;
      case 6u: broken.source.stageGeneration += 1u; break;
      case 7u: broken.source.currentDrawSourceGeneration = 0u; break;
      case 8u: broken.source.immutableModelGeneration = 0u; break;
      case 9u: broken.source.packageGeneration = 0u; break;
      case 10u: broken.source.packageContentHash = 0u; break;
      case 11u: broken.source.geosetDataIdentity = 0u; break;
      case 12u: broken.source.storeInstanceAuthority = 0u; break;
      case 13u: broken.source.frozenDescriptorIdentity = 0u; break;
      case 14u: broken.source.cacheSnapshotIdentity = 0u; break;
      case 15u: broken.source.catalogSnapshotRevision = 0u; break;
      case 16u: broken.source.packageCanonicalDigest = 0u; break;
      case 17u: broken.source.packageSchema = 0u; break;
      case 18u: broken.source.stage = 10u; break;
      case 19u: broken.source.captureComplete = false; break;
      case 20u: broken.source.exactCurrentAllocation = false; break;
      case 21u: broken.source.fresh = false; break;
      case 22u: broken.source.grace = true; break;
      case 23u: broken.source.rejected = true; break;
      case 24u: broken.recordIdentityToken = 0u; break;
      case 25u: broken.consumerMask = 0u; break;
      case 26u: broken.consumerMask = 0x80000000u; break;
      case 27u: broken.primitiveOrdinal = 1u; break;
      case 28u: broken.source.catalogSnapshot.reset(); break;
      case 29u: broken.source.packageContentDecision = {}; break;
      case 30u: broken.source.catalogInstanceGeneration = 0u; break;
      case 31u: broken.source.packagePublicationRevision = 0u; break;
      default: assert(false); break;
    }
    assert(authority.record(created.ticket, broken) ==
           Authority::RecordResult::InvalidCurrentStageSource);
    assert(authority.snapshot().state == Authority::State::Aborted);
    assert(authority.record(created.ticket, batch.records.front()) ==
           Authority::RecordResult::NotRecording);
  }
}

void testContextToctouFailClosed() {
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::CreateDecision created = authority.create(batch.create);
    Authority::RecordInput changed = batch.records.front();
    changed.context.commandListGeneration += 1u;
    assert(authority.record(created.ticket, changed) ==
           Authority::RecordResult::ContextMismatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(authority.record(created.ticket, batch.records.front()) ==
           Authority::RecordResult::Recorded);
    Authority::RecordingContext changed = batch.context;
    changed.recordingSessionGeneration += 1u;
    assert(authority.seal(created.ticket, changed).result ==
           Authority::SealResult::ContextMismatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    Authority::RecordingContext changed = batch.context;
    changed.emitCsSerial += 1u;
    EmitCapture capture;
    assert(authority.emitSealed(
               sealed, changed, &captureEmit, &capture) ==
           Authority::EmitResult::ContextMismatch);
    assert(capture.calls.load(std::memory_order_relaxed) == 0u);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
}

void testEveryRecordingContextDimensionIsExact() {
  for (uint32_t variant = 0u; variant < 12u; variant++) {
    Authority authority;
    Batch batch = makeBatch(1u, 1u);
    Authority::RecordInput changed = batch.records.front();
    switch (variant) {
      case 0u: changed.context.mapEpoch += 1u; break;
      case 1u: changed.context.deviceEpoch += 1u; break;
      case 2u: changed.context.frameSerial += 1u; break;
      case 3u: changed.context.policyRevision += 1u; break;
      case 4u: changed.context.stageGeneration += 1u; break;
      case 5u: changed.context.recordingOwnerToken += 1u; break;
      case 6u: changed.context.recordingSessionGeneration += 1u; break;
      case 7u: changed.context.commandListGeneration += 1u; break;
      case 8u: changed.context.emitCsSerial += 1u; break;
      case 9u: changed.context.canonicalBatchToken += 1u; break;
      case 10u: changed.context.ownerSubmissionSerial += 1u; break;
      case 11u: changed.context.stage = 10u; break;
      default: assert(false); break;
    }
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(created.result == Authority::CreateResult::Created);
    assert(authority.record(created.ticket, changed) ==
           Authority::RecordResult::ContextMismatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
}

void testEveryCanonicalRecordDimensionIsExact() {
  for (uint32_t variant = 0u; variant < 24u; variant++) {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(created.result == Authority::CreateResult::Created);
    Authority::RecordInput changed = batch.records.front();
    switch (variant) {
      case 0u: changed.source.currentDrawSourceGeneration += 1u; break;
      case 1u: changed.source.immutableModelGeneration += 1u; break;
      case 2u: changed.source.packageGeneration += 1u; break;
      case 3u: changed.source.packageContentHash += 1u; break;
      case 4u: changed.source.geosetDataIdentity += 1u; break;
      case 5u: changed.source.storeInstanceAuthority += 1u; break;
      case 6u: changed.source.frozenDescriptorIdentity += 1u; break;
      case 7u: changed.source.cacheSnapshotIdentity += 1u; break;
      case 8u: changed.source.catalogInstanceGeneration += 1u; break;
      case 9u: changed.source.catalogSnapshotRevision += 1u; break;
      case 10u: changed.source.packagePublicationRevision += 1u; break;
      case 11u: changed.source.packageCanonicalDigest += 1u; break;
      case 12u: changed.source.identityToken += 1u; break;
      case 13u: changed.source.sourceToken += 1u; break;
      case 14u: changed.source.materialToken += 1u; break;
      case 15u: changed.source.alphaToken += 1u; break;
      case 16u: changed.source.worldToken += 1u; break;
      case 17u: changed.source.boundsToken += 1u; break;
      case 18u: changed.source.packageSchema += 1u; break;
      case 19u: changed.source.packageLayoutGeneration += 1u; break;
      case 20u: changed.recordIdentityToken += 1u; break;
      case 21u: changed.commandIdentityToken += 1u; break;
      case 22u: changed.commandPayloadDigest += 1u; break;
      case 23u: changed.consumerMask = 2u; break;
      default: assert(false); break;
    }
    const bool invalidatesP1ReadyTuple =
        variant <= 4u || (variant >= 8u && variant <= 19u);
    const Authority::RecordResult expected = invalidatesP1ReadyTuple
        ? Authority::RecordResult::InvalidCurrentStageSource
        : Authority::RecordResult::ExpectedRecordMismatch;
    assert(authority.record(created.ticket, changed) == expected);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
}

void testOrdinalCompletenessAndDigest() {
  {
    Authority authority;
    Batch batch = makeBatch(1u, 2u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(authority.record(created.ticket, batch.records[1]) ==
           Authority::RecordResult::OrdinalMismatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 2u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(authority.record(created.ticket, batch.records[0]) ==
           Authority::RecordResult::Recorded);
    assert(authority.record(created.ticket, batch.records[0]) ==
           Authority::RecordResult::OrdinalMismatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 2u);
    const Authority::CreateDecision created = authority.create(batch.create);
    assert(authority.record(created.ticket, batch.records[0]) ==
           Authority::RecordResult::Recorded);
    assert(authority.seal(created.ticket, batch.context).result ==
           Authority::SealResult::IncompleteBatch);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    Batch batch = makeBatch(1u, 1u);
    batch.create.expectedRecordDigest ^= 0x100u;
    assert(authority.create(batch.create).result ==
           Authority::CreateResult::InvalidExpectedBatch);
  }
}

void testPostSealMutationAndDuplicateSealInvalidate() {
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    Authority::TransactionTicket transaction;
    const Authority::SealedTicket sealed = prepareSealed(
        authority, batch, &transaction);
    assert(authority.record(transaction, batch.records.front()) ==
           Authority::RecordResult::NotRecording);
    assert(authority.snapshot().state == Authority::State::Aborted);
    EmitCapture capture;
    assert(authority.emitSealed(
               sealed, batch.context, &captureEmit, &capture) ==
           Authority::EmitResult::AlreadyTerminal);
    assert(capture.calls.load(std::memory_order_relaxed) == 0u);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    Authority::TransactionTicket transaction;
    const Authority::SealedTicket sealed = prepareSealed(
        authority, batch, &transaction);
    assert(authority.seal(transaction, batch.context).result ==
           Authority::SealResult::NotRecording);
    assert(authority.snapshot().state == Authority::State::Aborted);
    EmitCapture capture;
    assert(authority.emitSealed(
               sealed, batch.context, &captureEmit, &capture) ==
           Authority::EmitResult::AlreadyTerminal);
  }
}

void testWrongThreadAbortsCurrentTransaction() {
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::CreateDecision created = authority.create(batch.create);
    std::atomic<uint32_t> result {0u};
    std::thread adversary([&] {
      result.store(
          static_cast<uint32_t>(authority.record(
              created.ticket, batch.records.front())),
          std::memory_order_release);
    });
    adversary.join();
    assert(static_cast<Authority::RecordResult>(
               result.load(std::memory_order_acquire)) ==
           Authority::RecordResult::WrongOwnerThread);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    EmitCapture capture;
    std::atomic<uint32_t> result {0u};
    std::thread adversary([&] {
      result.store(
          static_cast<uint32_t>(authority.emitSealed(
              sealed, batch.context, &captureEmit, &capture)),
          std::memory_order_release);
    });
    adversary.join();
    assert(static_cast<Authority::EmitResult>(
               result.load(std::memory_order_acquire)) ==
           Authority::EmitResult::WrongOwnerThread);
    assert(capture.calls.load(std::memory_order_relaxed) == 0u);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
}

void testStaleTicketsCannotPoisonCurrentTransaction() {
  Authority authority;
  const Batch oldBatch = makeBatch(1u, 1u);
  Authority::TransactionTicket oldTransaction;
  const Authority::SealedTicket oldSealed = prepareSealed(
      authority, oldBatch, &oldTransaction);
  EmitCapture oldCapture;
  oldCapture.expectedContext = oldBatch.context;
  oldCapture.expectedCount = 1u;
  oldCapture.expectedDigest = oldBatch.create.expectedRecordDigest;
  assert(authority.emitSealed(
             oldSealed, oldBatch.context, &captureEmit, &oldCapture) ==
         Authority::EmitResult::Emitted);

  const Batch currentBatch = makeBatch(2u, 2u);
  const Authority::CreateDecision current = authority.create(
      currentBatch.create);
  assert(current.result == Authority::CreateResult::Created);
  std::atomic<uint32_t> badResults {0u};
  std::array<std::thread, 8u> adversaries;
  for (std::thread& adversary : adversaries) {
    adversary = std::thread([&] {
      for (uint32_t i = 0u; i < 100u; i++) {
        if (authority.record(oldTransaction, oldBatch.records.front()) !=
            Authority::RecordResult::StaleTicket)
          badResults.fetch_add(1u, std::memory_order_relaxed);
        if (authority.abort(oldTransaction) !=
            Authority::AbortResult::StaleTicket)
          badResults.fetch_add(1u, std::memory_order_relaxed);
        if (authority.emitSealed(
                oldSealed, oldBatch.context, &captureEmit, &oldCapture) !=
            Authority::EmitResult::StaleTicket)
          badResults.fetch_add(1u, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& adversary : adversaries)
    adversary.join();
  assert(badResults.load(std::memory_order_relaxed) == 0u);
  assert(authority.snapshot().state == Authority::State::Recording);

  for (const Authority::RecordInput& record : currentBatch.records)
    assert(authority.record(current.ticket, record) ==
           Authority::RecordResult::Recorded);
  const Authority::SealDecision sealed = authority.seal(
      current.ticket, currentBatch.context);
  assert(sealed.result == Authority::SealResult::Sealed);
  EmitCapture capture;
  capture.expectedContext = currentBatch.context;
  capture.expectedCount = 2u;
  capture.expectedDigest = currentBatch.create.expectedRecordDigest;
  assert(authority.emitSealed(
             sealed.ticket, currentBatch.context,
             &captureEmit, &capture) == Authority::EmitResult::Emitted);
  assert(capture.calls.load(std::memory_order_relaxed) == 1u);
}

struct ReentrantCapture {
  Authority* authority = nullptr;
  Authority::SealedTicket ticket;
  Authority::RecordingContext context;
  std::atomic<uint32_t> callbacks {0u};
  std::atomic<uint32_t> innerResult {0u};
};

bool reentrantEmit(
    void* opaque, const Authority::SealedBatchView&) {
  auto* capture = static_cast<ReentrantCapture*>(opaque);
  capture->callbacks.fetch_add(1u, std::memory_order_relaxed);
  capture->innerResult.store(
      static_cast<uint32_t>(capture->authority->emitSealed(
          capture->ticket, capture->context, &reentrantEmit, capture)),
      std::memory_order_release);
  return true;
}

bool throwingEmit(void*, const Authority::SealedBatchView& view) {
  assert(view.commandPlan.size() == view.recordCount);
  throw std::runtime_error("EmitCs injected callback failure");
}

void testReentrantAndFailedCallbackAreTerminal() {
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    ReentrantCapture capture;
    capture.authority = &authority;
    capture.ticket = sealed;
    capture.context = batch.context;
    assert(authority.emitSealed(
               sealed, batch.context, &reentrantEmit, &capture) ==
           Authority::EmitResult::Emitted);
    assert(capture.callbacks.load(std::memory_order_relaxed) == 1u);
    assert(static_cast<Authority::EmitResult>(
               capture.innerResult.load(std::memory_order_acquire)) ==
           Authority::EmitResult::EmitInProgress);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    EmitCapture capture;
    capture.expectedContext = batch.context;
    capture.expectedCount = 1u;
    capture.expectedDigest = batch.create.expectedRecordDigest;
    capture.succeed = false;
    assert(authority.emitSealed(
               sealed, batch.context, &captureEmit, &capture) ==
           Authority::EmitResult::CallbackFailed);
    assert(authority.snapshot().state == Authority::State::Aborted);
    capture.succeed = true;
    assert(authority.emitSealed(
               sealed, batch.context, &captureEmit, &capture) ==
           Authority::EmitResult::AlreadyTerminal);
    assert(capture.calls.load(std::memory_order_relaxed) == 1u);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    assert(authority.emitSealed(
               sealed, batch.context, &throwingEmit, nullptr) ==
           Authority::EmitResult::CallbackFailed);
    assert(authority.snapshot().state == Authority::State::Aborted);
    assert(authority.diagnostics().callbacksFailed == 1u);
    assert(authority.emitSealed(
               sealed, batch.context, &throwingEmit, nullptr) ==
           Authority::EmitResult::AlreadyTerminal);
  }
  {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    assert(authority.emitSealed(
               sealed, batch.context, nullptr, nullptr) ==
           Authority::EmitResult::MissingCallback);
    assert(authority.snapshot().state == Authority::State::Aborted);
  }
}

struct BlockingCapture {
  std::atomic<bool> entered {false};
  std::atomic<bool> release {false};
  std::atomic<uint32_t> calls {0u};
};

bool blockingEmit(
    void* opaque, const Authority::SealedBatchView&) {
  auto* capture = static_cast<BlockingCapture*>(opaque);
  capture->entered.store(true, std::memory_order_release);
  while (!capture->release.load(std::memory_order_acquire))
    std::this_thread::yield();
  capture->calls.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

void testAbortCannotRevokeCallbackAlreadyInProgress() {
  Authority authority;
  const Batch batch = makeBatch(1u, 1u);
  const Authority::SealedTicket sealed = prepareSealed(authority, batch);
  BlockingCapture capture;
  std::atomic<uint32_t> abortResult {0u};
  std::thread canceller([&] {
    while (!capture.entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    abortResult.store(
        static_cast<uint32_t>(authority.abort(sealed)),
        std::memory_order_release);
    capture.release.store(true, std::memory_order_release);
  });
  assert(authority.emitSealed(
             sealed, batch.context, &blockingEmit, &capture) ==
         Authority::EmitResult::Emitted);
  canceller.join();
  assert(static_cast<Authority::AbortResult>(
             abortResult.load(std::memory_order_acquire)) ==
         Authority::AbortResult::EmitInProgress);
  assert(capture.calls.load(std::memory_order_relaxed) == 1u);
  assert(authority.snapshot().state == Authority::State::Emitted);
}

void testAdversarialEmitRaceAtMostOnce() {
  constexpr uint32_t kRounds = 200u;
  for (uint32_t round = 0u; round < kRounds; round++) {
    Authority authority;
    const Batch batch = makeBatch(1u, 1u);
    const Authority::SealedTicket sealed = prepareSealed(authority, batch);
    EmitCapture capture;
    capture.expectedContext = batch.context;
    capture.expectedCount = 1u;
    capture.expectedDigest = batch.create.expectedRecordDigest;
    std::atomic<bool> start {false};
    std::array<std::thread, 3u> adversaries;
    for (std::thread& adversary : adversaries) {
      adversary = std::thread([&] {
        while (!start.load(std::memory_order_acquire))
          std::this_thread::yield();
        const Authority::EmitResult result = authority.emitSealed(
            sealed, batch.context, &captureEmit, &capture);
        assert(result == Authority::EmitResult::WrongOwnerThread ||
               result == Authority::EmitResult::EmitInProgress ||
               result == Authority::EmitResult::AlreadyTerminal);
      });
    }
    start.store(true, std::memory_order_release);
    const Authority::EmitResult ownerResult = authority.emitSealed(
        sealed, batch.context, &captureEmit, &capture);
    for (std::thread& adversary : adversaries)
      adversary.join();

    const uint32_t calls = capture.calls.load(std::memory_order_relaxed);
    assert(calls <= 1u);
    const Authority::State state = authority.snapshot().state;
    assert(state == Authority::State::Emitted ||
           state == Authority::State::Aborted);
    if (ownerResult == Authority::EmitResult::Emitted) {
      assert(calls == 1u);
      assert(state == Authority::State::Emitted);
    } else {
      assert(ownerResult == Authority::EmitResult::AlreadyTerminal);
      assert(calls == 0u);
      assert(state == Authority::State::Aborted);
    }
  }
}

}  // namespace

int main() {
  testValidOneShotPath();
  testP1ReadyDecisionCannotBeForgedFromRawFields();
  testSealedPlanRetainsAuthorityOwnedImmutableRecords();
  testCapacityBoundaryAndAllocationFailure();
  testCreateValidationAndMonotonicOwner();
  testCurrentStageSourceFailClosed();
  testContextToctouFailClosed();
  testEveryRecordingContextDimensionIsExact();
  testEveryCanonicalRecordDimensionIsExact();
  testOrdinalCompletenessAndDigest();
  testPostSealMutationAndDuplicateSealInvalidate();
  testWrongThreadAbortsCurrentTransaction();
  testStaleTicketsCannotPoisonCurrentTransaction();
  testReentrantAndFailedCallbackAreTerminal();
  testAbortCannotRevokeCallbackAlreadyInProgress();
  testAdversarialEmitRaceAtMostOnce();
  return 0;
}
