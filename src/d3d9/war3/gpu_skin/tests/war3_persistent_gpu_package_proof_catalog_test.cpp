#include "../war3_persistent_gpu_package_proof_catalog.h"

#include <cstdio>
#include <thread>
#include <type_traits>

namespace {

using Catalog =
    dxvk::war3::gpu_skin::War3PersistentGpuPackageProofCatalog;
using Mutation = Catalog::MutationResult;
using State = Catalog::PublicationState;
using Reason = Catalog::PackageContentDecision::Reason;

int g_failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                   __LINE__, #condition);                                    \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

void HashU64(uint64_t& hash, uint64_t value) {
  for (uint32_t byte = 0u; byte < 8u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= 0x100000001b3ull;
  }
}

void HashU32(uint64_t& hash, uint32_t value) {
  for (uint32_t byte = 0u; byte < 4u; ++byte) {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= 0x100000001b3ull;
  }
}

uint64_t PrimitiveAggregate(const Catalog::PreparedPackage& prepared) {
  const auto& primitive = prepared.primitiveProof;
  uint64_t hash = 0xcbf29ce484222325ull;
  HashU64(hash, primitive.indexContentHash);
  HashU32(hash, primitive.ordinal);
  HashU32(hash, primitive.primitiveTypeOrMaterialSlot);
  HashU32(hash, primitive.firstIndex);
  HashU32(hash, primitive.indexCount);
  HashU32(hash, primitive.minVertex);
  HashU32(hash, primitive.maxVertex);
  return hash;
}

Catalog::PreparedPackage ValidPrepared(uint32_t packageVariant = 0u) {
  Catalog::PreparedPackage prepared = {};
  prepared.key.schema = Catalog::kSchemaVersion;
  prepared.key.mapEpoch = 3u;
  prepared.key.deviceEpoch = 5u;
  prepared.key.packageGeneration = 7u + packageVariant;
  prepared.key.immutableModelGeneration = 11u + packageVariant;
  prepared.key.geosetData = uintptr_t(0x123400u + packageVariant * 0x100u);
  prepared.key.contentHash = 13u + packageVariant;
  prepared.key.layoutGeneration = 17u;
  prepared.key.primitiveOrdinal = 0u;

  auto& proof = prepared.packageProof;
  proof.mapEpoch = prepared.key.mapEpoch;
  proof.deviceEpoch = prepared.key.deviceEpoch;
  proof.packageGeneration = prepared.key.packageGeneration;
  proof.geosetData = prepared.key.geosetData;
  proof.contentHash = prepared.key.contentHash;
  proof.positionContentHash = 19u + packageVariant;
  proof.normalContentHash = 23u + packageVariant;
  proof.vertexGroupContentHash = 29u + packageVariant;
  proof.uv0ContentHash = 31u + packageVariant;
  proof.uv1ContentHash = 0u;
  proof.indexContentHash = 37u + packageVariant;
  proof.localBoundsHash = 43u + packageVariant;
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
  primitive.ordinal = 0u;
  primitive.primitiveTypeOrMaterialSlot = 59u + packageVariant;
  primitive.firstIndex = 0u;
  primitive.indexCount = 3u;
  primitive.minVertex = 0u;
  primitive.maxVertex = 3u;
  proof.primitiveProofHash = PrimitiveAggregate(prepared);
  return prepared;
}

Catalog::DrawEvidence ValidEvidence(
    const Catalog::SharedSnapshot& snapshot,
    const Catalog::PreparedPackage& prepared) {
  Catalog::DrawEvidence evidence = {};
  const Catalog::Entry* entry = snapshot->find(prepared.key);
  CHECK(entry != nullptr);
  evidence.frameSerial = 101u;
  evidence.policyRevision = 103u;
  evidence.stage = Catalog::kRequiredStage;
  evidence.catalogInstanceGeneration = snapshot->instanceGeneration();
  evidence.catalogSnapshotRevision = snapshot->revision();
  evidence.currentDrawSourceGeneration = 181u;
  evidence.canonicalDigest = entry != nullptr
      ? entry->value.canonicalDigest
      : 0u;
  evidence.key = prepared.key;
  evidence.packageProof = prepared.packageProof;
  evidence.primitiveProof = prepared.primitiveProof;
  evidence.streamHashes.position = prepared.packageProof.positionContentHash;
  evidence.streamHashes.normal = prepared.packageProof.normalContentHash;
  evidence.streamHashes.vertexGroup =
      prepared.packageProof.vertexGroupContentHash;
  evidence.streamHashes.uv0 = prepared.packageProof.uv0ContentHash;
  evidence.streamHashes.uv1 = prepared.packageProof.uv1ContentHash;
  evidence.streamHashes.index = prepared.packageProof.indexContentHash;
  evidence.streamHashes.primitiveAggregate =
      prepared.packageProof.primitiveProofHash;
  evidence.streamHashes.localBounds =
      prepared.packageProof.localBoundsHash;
  evidence.primitiveDomain.firstIndex = prepared.primitiveProof.firstIndex;
  evidence.primitiveDomain.indexCount = prepared.primitiveProof.indexCount;
  evidence.primitiveDomain.minVertex = prepared.primitiveProof.minVertex;
  evidence.primitiveDomain.maxVertex = prepared.primitiveProof.maxVertex;
  evidence.primitiveDomain.wholeIndexCount = prepared.packageProof.indexCount;
  evidence.primitiveDomain.vertexCount = prepared.packageProof.vertexCount;
  evidence.primitiveDomain.indexType = prepared.packageProof.indexType;
  evidence.identity = {179u, 179u};
  evidence.source = {181u, 181u};
  evidence.material = {107u, 107u};
  evidence.alpha = {109u, 109u};
  evidence.world = {113u, 113u};
  evidence.bounds = {127u, 127u};
  evidence.identityExact = true;
  evidence.sourceExact = true;
  evidence.staticRigid = true;
  evidence.fresh = true;
  return evidence;
}

Catalog::ValidationContext ValidContext() {
  return {101u, 103u, Catalog::kRequiredStage};
}

Catalog::ProducerFenceObservation Completed(
    const Catalog::ProducerFencePoint& fence) {
  return {fence.identity, fence.value, true};
}

bool DecisionMatches(
    const Catalog::PackageContentDecision& decision,
    const Catalog::Key& key,
    const Catalog::SharedSnapshot& snapshot,
    const Catalog::DrawEvidence& evidence) {
  return decision.matches(
      key, snapshot->instanceGeneration(), snapshot->revision(),
      evidence.frameSerial,
      evidence.policyRevision, evidence.stage,
      evidence.identity.drawToken, evidence.source.drawToken,
      evidence.currentDrawSourceGeneration,
      evidence.material.drawToken,
      evidence.alpha.drawToken, evidence.world.drawToken,
      evidence.bounds.drawToken);
}

void TestValueOnlyPolicyAndUnforgeableDecision() {
  static_assert(!Catalog::kRuntimeInstantiated);
  static_assert(!Catalog::kBindsAtlas);
  static_assert(!Catalog::kConsumeAdmissionGranted);
  static_assert(!Catalog::kStorePublicationAuthorityIntegrated);
  static_assert(!Catalog::kMultiPrimitivePublicationGranted);
  static_assert(!std::is_copy_constructible_v<Catalog>);
  static_assert(!std::is_copy_assignable_v<Catalog>);
  static_assert(std::is_default_constructible_v<
      Catalog::PackageContentDecision>);
  static_assert(!std::is_aggregate_v<Catalog::PackageContentDecision>);
  static_assert(!std::is_constructible_v<
      Catalog::PackageContentDecision, bool, Reason, uint64_t, uint64_t,
      uint64_t, Catalog::Key, uint64_t, uint64_t, uint32_t, uint64_t,
      uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>);

  const Catalog::PackageContentDecision defaultDecision;
  CHECK(!defaultDecision.ready());
  CHECK(defaultDecision.reason() == Reason::MissingSnapshot);
  const auto prepared = ValidPrepared();
  CHECK(!defaultDecision.matches(
      prepared.key, 1u, 1u, 1u, 1u, Catalog::kRequiredStage,
      1u, 1u, 1u, 1u, 1u, 1u, 1u));

  Catalog catalog;
  auto incomplete = ValidPrepared();
  incomplete.packageProof.primitiveProofCount = 2u;
  CHECK(catalog.publishPrepared(incomplete) == Mutation::Invalid);

  auto contradictoryIndex = ValidPrepared(1u);
  contradictoryIndex.primitiveProof.indexContentHash += 1u;
  contradictoryIndex.packageProof.primitiveProofHash =
      PrimitiveAggregate(contradictoryIndex);
  CHECK(catalog.publishPrepared(contradictoryIndex) == Mutation::Invalid);
}

void TestSortedImmutableSnapshotsAndSingleWriter() {
  Catalog catalog;
  const auto higher = ValidPrepared(1u);
  const auto lower = ValidPrepared(0u);
  CHECK(catalog.snapshot()->instanceGeneration() != 0u);
  CHECK(catalog.snapshot()->revision() == 1u);
  CHECK(catalog.publishPrepared(higher) == Mutation::Accepted);
  const auto one = catalog.snapshot();
  CHECK(one->size() == 1u);
  CHECK(one->entry(0u)->value.state == State::Prepared);

  Mutation otherThreadResult = Mutation::Accepted;
  std::thread other([&] {
    otherThreadResult = catalog.publishPrepared(lower);
  });
  other.join();
  CHECK(otherThreadResult == Mutation::WrongWriter);
  CHECK(catalog.snapshot()->revision() == one->revision());

  CHECK(catalog.publishPrepared(lower) == Mutation::Accepted);
  const auto sorted = catalog.snapshot();
  CHECK(sorted->size() == 2u);
  CHECK(sorted->entry(0u)->key.packageGeneration == 7u);
  CHECK(sorted->entry(1u)->key.packageGeneration == 8u);
  CHECK(one->size() == 1u);
  CHECK(one->find(lower.key) == nullptr);
}

void TestStateMachineAndSnapshotImmutability() {
  Catalog catalog;
  const auto prepared = ValidPrepared();
  const Catalog::ProducerFencePoint fence{131u, 137u};
  CHECK(catalog.publishPrepared(prepared) == Mutation::Accepted);
  CHECK(catalog.publishPrepared(prepared) == Mutation::Duplicate);
  const auto preparedSnapshot = catalog.snapshot();

  CHECK(catalog.publishUploadCompleted(prepared.key, Completed(fence)) ==
        Mutation::StateConflict);
  CHECK(catalog.publishUploadSubmitted(prepared.key, {0u, 1u}) ==
        Mutation::Invalid);
  CHECK(catalog.publishUploadSubmitted(prepared.key, fence) ==
        Mutation::Accepted);
  const auto submittedSnapshot = catalog.snapshot();
  CHECK(preparedSnapshot->find(prepared.key)->value.state == State::Prepared);
  CHECK(submittedSnapshot->find(prepared.key)->value.state ==
        State::UploadSubmitted);
  CHECK(catalog.publishUploadSubmitted(prepared.key, fence) ==
        Mutation::Duplicate);
  CHECK(catalog.publishUploadCompleted(
            prepared.key, {fence.identity + 1u, fence.value, true}) ==
        Mutation::FenceMismatch);
  CHECK(catalog.publishUploadCompleted(
            prepared.key, {fence.identity, fence.value - 1u, true}) ==
        Mutation::CompletionNotObserved);
  CHECK(catalog.publishUploadCompleted(
            prepared.key, {fence.identity, fence.value, false}) ==
        Mutation::CompletionNotObserved);
  CHECK(catalog.publishUploadCompleted(prepared.key, Completed(fence)) ==
        Mutation::Accepted);
  const auto completedSnapshot = catalog.snapshot();
  const auto* completed = completedSnapshot->find(prepared.key);
  CHECK(completed != nullptr);
  CHECK(completed->value.state == State::UploadCompleted);
  CHECK(completed->value.producerFence.identity == fence.identity);
  CHECK(completed->value.producerFence.value == fence.value);
  CHECK(submittedSnapshot->find(prepared.key)->value.state ==
        State::UploadSubmitted);
}

void TestValidatorRequiresCompleteExactEvidence() {
  Catalog catalog;
  const auto prepared = ValidPrepared();
  const Catalog::ProducerFencePoint fence{139u, 149u};
  CHECK(catalog.publishPrepared(prepared) == Mutation::Accepted);
  const auto notReadySnapshot = catalog.snapshot();
  auto evidence = ValidEvidence(notReadySnapshot, prepared);
  auto decision = Catalog::validateDrawEvidence(
      notReadySnapshot, ValidContext(), evidence);
  CHECK(!decision.ready());
  CHECK(decision.reason() == Reason::UploadNotCompleted);

  CHECK(catalog.publishUploadSubmitted(prepared.key, fence) ==
        Mutation::Accepted);
  CHECK(catalog.publishUploadCompleted(prepared.key, Completed(fence)) ==
        Mutation::Accepted);
  const auto readySnapshot = catalog.snapshot();
  evidence = ValidEvidence(readySnapshot, prepared);
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), evidence);
  CHECK(decision.ready());
  CHECK(decision.reason() == Reason::Ready);
  CHECK(DecisionMatches(decision, prepared.key, readySnapshot, evidence));
  auto wrongKey = prepared.key;
  wrongKey.primitiveOrdinal += 1u;
  CHECK(!DecisionMatches(decision, wrongKey, readySnapshot, evidence));
  CHECK(!decision.matches(
      prepared.key, readySnapshot->instanceGeneration(),
      readySnapshot->revision() + 1u,
      evidence.frameSerial, evidence.policyRevision, evidence.stage,
      evidence.identity.drawToken, evidence.source.drawToken,
      evidence.currentDrawSourceGeneration,
      evidence.material.drawToken, evidence.alpha.drawToken,
      evidence.world.drawToken, evidence.bounds.drawToken));
  auto wrongIdentity = evidence;
  wrongIdentity.identity.drawToken += 1u;
  CHECK(!DecisionMatches(
      decision, prepared.key, readySnapshot, wrongIdentity));
  auto wrongContext = evidence;
  wrongContext.frameSerial += 1u;
  CHECK(!DecisionMatches(
      decision, prepared.key, readySnapshot, wrongContext));
  wrongContext = evidence;
  wrongContext.policyRevision += 1u;
  CHECK(!DecisionMatches(
      decision, prepared.key, readySnapshot, wrongContext));
  wrongContext = evidence;
  wrongContext.stage = 10u;
  CHECK(!DecisionMatches(
      decision, prepared.key, readySnapshot, wrongContext));
  auto wrongDrawGeneration = evidence;
  wrongDrawGeneration.currentDrawSourceGeneration += 1u;
  CHECK(!DecisionMatches(
      decision, prepared.key, readySnapshot, wrongDrawGeneration));

  auto changed = evidence;
  changed.packageProof.positionContentHash += 1u;
  // Keep the catalog digest unchanged deliberately. A digest-only validator
  // would accept this collision/spoof, while the complete proof must reject.
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(!decision.ready());
  CHECK(decision.reason() == Reason::PackageProofMismatch);

  changed = evidence;
  changed.primitiveProof.maxVertex -= 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::PrimitiveProofMismatch);

  changed = evidence;
  changed.streamHashes.index += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::StreamHashMismatch);

  changed = evidence;
  changed.primitiveDomain.firstIndex += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::PrimitiveDomainMismatch);

  changed = evidence;
  changed.grace = true;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::RouteRejected);

  changed = evidence;
  changed.material.drawToken += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::ExactTokenMismatch);

  changed = evidence;
  changed.catalogInstanceGeneration += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::CatalogInstanceGenerationMismatch);

  changed = evidence;
  changed.catalogSnapshotRevision -= 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::CatalogSnapshotRevisionMismatch);

  changed = evidence;
  changed.canonicalDigest += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::DigestMismatch);

  changed = evidence;
  changed.key.immutableModelGeneration += 1u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::EntryNotFound);

  changed = evidence;
  changed.stage = 10u;
  decision = Catalog::validateDrawEvidence(
      readySnapshot, ValidContext(), changed);
  CHECK(decision.reason() == Reason::InvalidDrawContext);
}

void TestOneSnapshotValidatesMultiplePublicationRevisions() {
  Catalog catalog;
  const auto first = ValidPrepared(0u);
  const auto second = ValidPrepared(1u);
  const Catalog::ProducerFencePoint firstFence{163u, 167u};
  const Catalog::ProducerFencePoint secondFence{173u, 179u};
  CHECK(catalog.publishPrepared(first) == Mutation::Accepted);
  CHECK(catalog.publishPrepared(second) == Mutation::Accepted);
  CHECK(catalog.publishUploadSubmitted(first.key, firstFence) ==
        Mutation::Accepted);
  CHECK(catalog.publishUploadCompleted(first.key, Completed(firstFence)) ==
        Mutation::Accepted);
  CHECK(catalog.publishUploadSubmitted(second.key, secondFence) ==
        Mutation::Accepted);
  CHECK(catalog.publishUploadCompleted(second.key, Completed(secondFence)) ==
        Mutation::Accepted);

  const auto shared = catalog.snapshot();
  const auto* firstEntry = shared->find(first.key);
  const auto* secondEntry = shared->find(second.key);
  CHECK(firstEntry != nullptr);
  CHECK(secondEntry != nullptr);
  CHECK(firstEntry->value.publicationRevision !=
        secondEntry->value.publicationRevision);

  const auto firstDecision = Catalog::validateDrawEvidence(
      shared, ValidContext(), ValidEvidence(shared, first));
  const auto secondDecision = Catalog::validateDrawEvidence(
      shared, ValidContext(), ValidEvidence(shared, second));
  CHECK(firstDecision.ready());
  CHECK(secondDecision.ready());
  const auto firstEvidence = ValidEvidence(shared, first);
  const auto secondEvidence = ValidEvidence(shared, second);
  CHECK(DecisionMatches(firstDecision, first.key, shared, firstEvidence));
  CHECK(!firstDecision.matches(
      first.key, shared->instanceGeneration() + 1u, shared->revision(),
      firstEvidence.frameSerial, firstEvidence.policyRevision,
      firstEvidence.stage, firstEvidence.identity.drawToken,
      firstEvidence.source.drawToken,
      firstEvidence.currentDrawSourceGeneration,
      firstEvidence.material.drawToken, firstEvidence.alpha.drawToken,
      firstEvidence.world.drawToken, firstEvidence.bounds.drawToken));
  CHECK(DecisionMatches(secondDecision, second.key, shared, secondEvidence));
  CHECK(!DecisionMatches(
      firstDecision, second.key, shared, secondEvidence));
}

void TestInvalidationIsTerminalAndImmutableConflictFailsClosed() {
  Catalog catalog;
  auto prepared = ValidPrepared();
  const Catalog::ProducerFencePoint fence{151u, 157u};
  CHECK(catalog.publishPrepared(prepared) == Mutation::Accepted);

  auto conflict = prepared;
  conflict.packageProof.normalContentHash += 1u;
  CHECK(catalog.publishPrepared(conflict) == Mutation::ImmutableConflict);

  CHECK(catalog.publishUploadSubmitted(prepared.key, fence) ==
        Mutation::Accepted);
  CHECK(catalog.publishUploadCompleted(prepared.key, Completed(fence)) ==
        Mutation::Accepted);
  const auto completed = catalog.snapshot();
  auto evidence = ValidEvidence(completed, prepared);
  CHECK(Catalog::validateDrawEvidence(
      completed, ValidContext(), evidence).ready());

  CHECK(catalog.publishInvalidated(prepared.key) == Mutation::Accepted);
  const auto invalidated = catalog.snapshot();
  evidence = ValidEvidence(invalidated, prepared);
  const auto decision = Catalog::validateDrawEvidence(
      invalidated, ValidContext(), evidence);
  CHECK(!decision.ready());
  CHECK(decision.reason() == Reason::UploadNotCompleted);
  CHECK(catalog.publishInvalidated(prepared.key) == Mutation::Duplicate);
  CHECK(catalog.publishPrepared(prepared) == Mutation::StateConflict);
  CHECK(completed->find(prepared.key)->value.state == State::UploadCompleted);
}

}  // namespace

int main() {
  TestValueOnlyPolicyAndUnforgeableDecision();
  TestSortedImmutableSnapshotsAndSingleWriter();
  TestStateMachineAndSnapshotImmutability();
  TestValidatorRequiresCompleteExactEvidence();
  TestOneSnapshotValidatesMultiplePublicationRevisions();
  TestInvalidationIsTerminalAndImmutableConflictFailsClosed();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d proof catalog checks failed\n", g_failures);
    return 1;
  }
  std::puts("war3_persistent_gpu_package_proof_catalog_test: PASS");
  return 0;
}
