#include "../war3_persistent_gpu_package_observer.h"

#include <cstdio>
#include <limits>
#include <memory>

namespace {

using Observer =
    dxvk::war3::gpu_skin::War3PersistentGpuPackageObserver;
using Catalog = Observer::PackageProofCatalog;

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

uint64_t PrimitiveAggregate(
    const dxvk::war3::gpu_skin::GpuSkinStaticPrimitiveProof& primitive) {
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

Catalog::PreparedPackage PreparedPackage(bool second) {
  Catalog::PreparedPackage prepared = {};
  prepared.key.schema = Catalog::kSchemaVersion;
  prepared.key.mapEpoch = 3u;
  prepared.key.deviceEpoch = 5u;
  prepared.key.packageGeneration = second ? 41u : 11u;
  prepared.key.immutableModelGeneration = second ? 43u : 13u;
  prepared.key.geosetData = second ? uintptr_t(0x234500u) :
      uintptr_t(0x123400u);
  prepared.key.contentHash = second ? 47u : 19u;
  prepared.key.layoutGeneration = 53u;
  prepared.key.primitiveOrdinal = 0u;

  auto& proof = prepared.packageProof;
  proof.mapEpoch = prepared.key.mapEpoch;
  proof.deviceEpoch = prepared.key.deviceEpoch;
  proof.packageGeneration = prepared.key.packageGeneration;
  proof.geosetData = prepared.key.geosetData;
  proof.contentHash = prepared.key.contentHash;
  proof.positionContentHash = second ? 59u : 61u;
  proof.normalContentHash = second ? 67u : 71u;
  proof.vertexGroupContentHash = second ? 73u : 79u;
  proof.uv0ContentHash = second ? 83u : 89u;
  proof.indexContentHash = second ? 97u : 101u;
  proof.localBoundsHash = second ? 109u : 113u;
  proof.layoutGeneration = prepared.key.layoutGeneration;
  proof.vertexCount = 4u;
  proof.indexCount = 3u;
  proof.uvLayerCount = 1u;
  proof.primitiveProofCount = 1u;
  proof.indexType = VK_INDEX_TYPE_UINT16;
  proof.localMinX = -1.0f;
  proof.localMinY = -2.0f;
  proof.localMinZ = -3.0f;
  proof.localMaxX = 4.0f;
  proof.localMaxY = 5.0f;
  proof.localMaxZ = 6.0f;
  proof.staticByteOffset = 64u;
  proof.staticByteLength = 132u;
  proof.indexByteOffset = 224u;
  proof.indexByteLength = 6u;

  auto& primitive = prepared.primitiveProof;
  primitive.indexContentHash = proof.indexContentHash;
  primitive.ordinal = 0u;
  primitive.primitiveTypeOrMaterialSlot = second ? 137u : 139u;
  primitive.firstIndex = 0u;
  primitive.indexCount = 3u;
  primitive.minVertex = 0u;
  primitive.maxVertex = 3u;
  proof.primitiveProofHash = PrimitiveAggregate(primitive);
  return prepared;
}

Catalog::DrawEvidence DrawEvidence(
    const Catalog::SharedSnapshot& snapshot,
    const Catalog::PreparedPackage& prepared,
    uint64_t identityToken,
    uint64_t sourceToken) {
  Catalog::DrawEvidence evidence = {};
  const auto* entry = snapshot->find(prepared.key);
  CHECK(entry != nullptr);
  evidence.frameSerial = 101u;
  evidence.policyRevision = 7u;
  evidence.stage = Catalog::kRequiredStage;
  evidence.catalogInstanceGeneration = snapshot->instanceGeneration();
  evidence.catalogSnapshotRevision = snapshot->revision();
  evidence.canonicalDigest = entry != nullptr
      ? entry->value.canonicalDigest
      : 0u;
  evidence.currentDrawSourceGeneration =
      prepared.key.packageGeneration == 41u ? 223u : 211u;
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
  evidence.streamHashes.localBounds = prepared.packageProof.localBoundsHash;
  evidence.primitiveDomain.firstIndex = prepared.primitiveProof.firstIndex;
  evidence.primitiveDomain.indexCount = prepared.primitiveProof.indexCount;
  evidence.primitiveDomain.minVertex = prepared.primitiveProof.minVertex;
  evidence.primitiveDomain.maxVertex = prepared.primitiveProof.maxVertex;
  evidence.primitiveDomain.wholeIndexCount = prepared.packageProof.indexCount;
  evidence.primitiveDomain.vertexCount = prepared.packageProof.vertexCount;
  evidence.primitiveDomain.indexType = prepared.packageProof.indexType;
  evidence.identity = {identityToken, identityToken};
  evidence.source = {sourceToken, sourceToken};
  evidence.material = {23u, 23u};
  evidence.alpha = {29u, 29u};
  evidence.world = {37u, 37u};
  evidence.bounds = {31u, 31u};
  evidence.identityExact = true;
  evidence.sourceExact = true;
  evidence.staticRigid = true;
  evidence.fresh = true;
  return evidence;
}

struct ProofFixture {
  Catalog catalog;
  Catalog::PreparedPackage first = PreparedPackage(false);
  Catalog::PreparedPackage second = PreparedPackage(true);
  Catalog::SharedSnapshot snapshot;
  Catalog::PackageContentDecision firstDecision;
  Catalog::PackageContentDecision secondDecision;

  ProofFixture() {
    CHECK(catalog.publishPrepared(first) == Catalog::MutationResult::Accepted);
    CHECK(catalog.publishPrepared(second) == Catalog::MutationResult::Accepted);
    const Catalog::ProducerFencePoint firstFence{149u, 151u};
    const Catalog::ProducerFencePoint secondFence{157u, 163u};
    CHECK(catalog.publishUploadSubmitted(first.key, firstFence) ==
          Catalog::MutationResult::Accepted);
    CHECK(catalog.publishUploadCompleted(
              first.key,
              {firstFence.identity, firstFence.value, true}) ==
          Catalog::MutationResult::Accepted);
    CHECK(catalog.publishUploadSubmitted(second.key, secondFence) ==
          Catalog::MutationResult::Accepted);
    CHECK(catalog.publishUploadCompleted(
              second.key,
              {secondFence.identity, secondFence.value, true}) ==
          Catalog::MutationResult::Accepted);
    snapshot = catalog.snapshot();
    firstDecision = Catalog::validateDrawEvidence(
        snapshot, {101u, 7u, Catalog::kRequiredStage},
        DrawEvidence(snapshot, first, 17u, 19u));
    secondDecision = Catalog::validateDrawEvidence(
        snapshot, {101u, 7u, Catalog::kRequiredStage},
        DrawEvidence(snapshot, second, 167u, 173u));
    CHECK(firstDecision.ready());
    CHECK(secondDecision.ready());
  }
};

const ProofFixture& Proofs() {
  static const ProofFixture fixture;
  return fixture;
}

Observer::FrameContext ValidContext(
    Observer::Mode mode = Observer::Mode::Observe) {
  Observer::FrameContext context = {};
  context.requestedMode = mode;
  context.frameSerial = 101u;
  context.policyRevision = 7u;
  context.stage = Observer::kRequiredStage;
  context.mapEpoch = 3u;
  context.deviceEpoch = 5u;
  context.catalogInstanceGeneration =
      Proofs().snapshot->instanceGeneration();
  context.catalogSnapshotRevision = Proofs().snapshot->revision();
  context.canonicalWorkload = 900u;
  return context;
}

Observer::SealInput ValidInput(bool second = false) {
  const auto& proofs = Proofs();
  const auto& prepared = second ? proofs.second : proofs.first;
  Observer::SealInput input = {};
  input.frameSerial = 101u;
  input.policyRevision = 7u;
  input.stage = Observer::kRequiredStage;
  input.mapEpoch = 3u;
  input.deviceEpoch = 5u;
  input.catalogInstanceGeneration = proofs.snapshot->instanceGeneration();
  input.catalogSnapshotRevision = proofs.snapshot->revision();
  input.packageGeneration = prepared.key.packageGeneration;
  input.immutableModelGeneration = prepared.key.immutableModelGeneration;
  input.currentDrawSourceGeneration = second ? 223u : 211u;
  input.packageKey = prepared.key;
  input.packageContentDecision = second
      ? proofs.secondDecision
      : proofs.firstDecision;
  input.identityToken = second ? 167u : 17u;
  input.sourceToken = second ? 173u : 19u;
  input.materialToken = 23u;
  input.alphaToken = 29u;
  input.worldToken = 37u;
  input.boundsToken = 31u;
  input.requestedConsumerMask = Observer::ConsumerMain |
      Observer::ConsumerCsm0 | Observer::ConsumerCsm1 |
      Observer::ConsumerCsm2 | Observer::ConsumerCsm3 |
      Observer::ConsumerPointShadow |
      Observer::ConsumerGeometryOutline;
  input.bounds = {-1.0f, -2.0f, -3.0f, 4.0f, 5.0f, 6.0f};
  input.identityKnown = true;
  input.identityExact = true;
  input.sourceKnown = true;
  input.sourceExact = true;
  input.materialKnown = true;
  input.materialExact = true;
  input.alphaKnown = true;
  input.alphaExact = true;
  input.worldKnown = true;
  input.worldExact = true;
  input.boundsKnown = true;
  input.boundsExact = true;
  input.staticRigidProven = true;
  return input;
}

Observer::ActualConsumerNote NoteFor(
    const Observer::SealDecision& decision,
    const Observer::SealInput& input,
    uint32_t consumerBit) {
  Observer::ActualConsumerNote note = {};
  note.tableIndex = decision.tableIndex;
  note.frameSerial = input.frameSerial;
  note.policyRevision = input.policyRevision;
  note.stage = input.stage;
  note.mapEpoch = input.mapEpoch;
  note.deviceEpoch = input.deviceEpoch;
  note.catalogInstanceGeneration = input.catalogInstanceGeneration;
  note.catalogSnapshotRevision = input.catalogSnapshotRevision;
  note.packageGeneration = input.packageGeneration;
  note.immutableModelGeneration = input.immutableModelGeneration;
  note.currentDrawSourceGeneration = input.currentDrawSourceGeneration;
  note.packageKey = input.packageKey;
  note.identityToken = input.identityToken;
  note.sourceToken = input.sourceToken;
  note.materialToken = input.materialToken;
  note.alphaToken = input.alphaToken;
  note.worldToken = input.worldToken;
  note.boundsToken = input.boundsToken;
  note.consumerBit = consumerBit;
  return note;
}

void TestOffAndObservePreserveCanonicalWorkload() {
  auto observer = std::make_unique<Observer>();
  auto context = ValidContext(Observer::Mode::Off);
  const auto offBegin = observer->beginFrame(context);
  const auto off = observer->seal(ValidInput());
  CHECK(offBegin.effectiveMode == Observer::Mode::Off);
  CHECK(off.disposition == Observer::Disposition::ModeOff);
  CHECK(off.effectiveConsumerMask == off.requestedConsumerMask);
  CHECK(!off.drawMutationAllowed);
  CHECK(!off.atlasBindingAllowed);
  CHECK(!off.writesConsumerLastUse);
  CHECK(observer->effectiveCanonicalWorkload() == 900u);
  CHECK(observer->size() == 0u);

  context.requestedMode = Observer::Mode::Observe;
  const auto observeBegin = observer->beginFrame(context);
  const auto observed = observer->seal(ValidInput());
  CHECK(observeBegin.effectiveMode == Observer::Mode::Observe);
  CHECK(observed.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(observed.recorded);
  CHECK(observed.effectiveConsumerMask == observed.requestedConsumerMask);
  CHECK(observed.eligibleConsumerMask == observed.requestedConsumerMask);
  CHECK(observed.wouldUseConsumerMask == 0u);
  CHECK(observed.actualConsumerMask == 0u);
  CHECK(observer->entry(observed.tableIndex)->actualConsumerMask == 0u);
  CHECK(!observed.drawMutationAllowed);
  CHECK(!observed.atlasBindingAllowed);
  CHECK(!observed.writesConsumerLastUse);
  CHECK(observer->effectiveCanonicalWorkload() == 900u);
  CHECK(observer->diagnostics().eligibleEntries == 1u);
  CHECK(observer->diagnostics().wouldUseEntries == 0u);
  CHECK(observer->diagnostics().actualConsumerEntries == 0u);
}

void TestConsumeIsHardDisabled() {
  auto observer = std::make_unique<Observer>();
  const auto begin = observer->beginFrame(
      ValidContext(Observer::Mode::Consume));
  const auto result = observer->seal(ValidInput());
  CHECK(!Observer::kConsumeAdmissionGranted);
  CHECK(begin.requestedMode == Observer::Mode::Consume);
  CHECK(begin.effectiveMode == Observer::Mode::Off);
  CHECK(begin.disposition == Observer::Disposition::ConsumeNotAdmitted);
  CHECK(result.disposition == Observer::Disposition::ConsumeNotAdmitted);
  CHECK(result.proofState == Observer::ProofState::Rejected);
  CHECK(result.effectiveConsumerMask == result.requestedConsumerMask);
  CHECK(result.wouldUseConsumerMask == 0u);
  CHECK(!result.recorded);
  CHECK(observer->size() == 0u);
}

void TestExactGenerationAndFrameGates() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());

  auto input = ValidInput();
  input.frameSerial += 1u;
  auto result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::Rejected);
  CHECK(result.disposition == Observer::Disposition::FrameMismatch);

  input = ValidInput();
  input.policyRevision += 1u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::PolicyMismatch);

  input = ValidInput();
  input.stage = 10u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::StageMismatch);

  input = ValidInput();
  input.catalogInstanceGeneration += 1u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::GenerationMismatch);

  input = ValidInput();
  input.catalogSnapshotRevision += 1u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::GenerationMismatch);

  // Per-entry package/source generations are not frame-global. Two exact
  // packages may coexist in one Stage11 frame and both must be recordable.
  input = ValidInput(true);
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::Recorded);
  CHECK(result.recorded);
  CHECK(result.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(observer->diagnostics().rejected == 5u);
}

void TestProofStateCeilings() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());

  auto input = ValidInput();
  input.identityKnown = false;
  auto result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::IdentityOnly);
  CHECK(result.wouldUseConsumerMask == 0u);

  input = ValidInput();
  input.packageContentDecision = {};
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::ContentPending);

  input = ValidInput();
  input.packageContentDecision = Proofs().secondDecision;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::ContentPending);
  CHECK(!observer->entry(result.tableIndex)->packageDecisionBound);
  CHECK(observer->entry(result.tableIndex)->packagePublicationRevision == 0u);
  CHECK(observer->entry(result.tableIndex)->packageCanonicalDigest == 0u);
  CHECK(observer->diagnostics().packageDecisionBindingMismatch == 1u);

  input = ValidInput();
  input.materialKnown = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.alphaExact = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.worldKnown = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.dynamic = true;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.skinned = true;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.staticRigidProven = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.boundsKnown = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  CHECK(observer->diagnostics().fullyEquivalent == 0u);
  CHECK(observer->diagnostics().packageInputReady == 7u);
}

void TestNonFiniteBoundsReject() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());
  auto input = ValidInput();
  input.bounds.maxX = std::numeric_limits<float>::infinity();
  const auto result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::Rejected);
  CHECK(result.disposition == Observer::Disposition::NonFiniteBounds);
  CHECK(result.wouldUseConsumerMask == 0u);
  CHECK(result.effectiveConsumerMask == input.requestedConsumerMask);
}

void TestActualConsumersRequireExplicitDrawEvidence() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());
  const auto input = ValidInput();
  const auto sealed = observer->seal(input);
  CHECK(sealed.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(sealed.eligibleConsumerMask == input.requestedConsumerMask);
  CHECK(sealed.wouldUseConsumerMask == 0u);
  CHECK(sealed.actualConsumerMask == 0u);

  auto note = NoteFor(sealed, input, Observer::ConsumerCsm2);
  auto noteResult = observer->noteActualConsumer(note);
  CHECK(noteResult == Observer::ActualNoteResult::Recorded);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask ==
        Observer::ConsumerCsm2);
  CHECK(observer->entry(sealed.tableIndex)->wouldUseConsumerMask ==
        Observer::ConsumerCsm2);
  CHECK(observer->diagnostics().actualConsumerEntries == 1u);
  CHECK(observer->diagnostics().actualConsumerBits == 1u);
  CHECK(observer->diagnostics().actualConsumerMaskOr ==
        Observer::ConsumerCsm2);
  CHECK(observer->diagnostics().wouldUseEntries == 1u);
  CHECK(observer->diagnostics().wouldUseConsumerMaskOr ==
        Observer::ConsumerCsm2);

  noteResult = observer->noteActualConsumer(note);
  CHECK(noteResult == Observer::ActualNoteResult::Duplicate);
  CHECK(observer->diagnostics().actualConsumerBits == 1u);
  CHECK(observer->diagnostics().actualNotesDuplicate == 1u);

  note.consumerBit = Observer::ConsumerPointShadow;
  noteResult = observer->noteActualConsumer(note);
  CHECK(noteResult == Observer::ActualNoteResult::Recorded);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask ==
        (Observer::ConsumerCsm2 | Observer::ConsumerPointShadow));
  CHECK(observer->entry(sealed.tableIndex)->wouldUseConsumerMask ==
        (Observer::ConsumerCsm2 | Observer::ConsumerPointShadow));
  CHECK(observer->diagnostics().actualConsumerBits == 2u);
}

void TestMainAlsoRequiresExplicitBoundDrawEvidence() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());
  auto input = ValidInput();
  input.requestedConsumerMask = Observer::ConsumerMain;
  const auto result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(result.actualConsumerMask == 0u);
  CHECK(result.wouldUseConsumerMask == 0u);

  auto note = NoteFor(result, input, Observer::ConsumerMain);
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::Recorded);
  CHECK(observer->entry(result.tableIndex)->actualConsumerMask ==
        Observer::ConsumerMain);
  CHECK(observer->entry(result.tableIndex)->wouldUseConsumerMask ==
        Observer::ConsumerMain);
}

void TestActualNotesRequireExactCurrentEntry() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());
  auto input = ValidInput();
  input.requestedConsumerMask = Observer::ConsumerMain |
      Observer::ConsumerGeometryOutline;
  const auto sealed = observer->seal(input);
  auto note = NoteFor(
      sealed, input, Observer::ConsumerGeometryOutline);

  note.packageGeneration += 1u;
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::GenerationMismatch);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  note = NoteFor(sealed, input, 1u << 31u);
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::UnknownConsumerBit);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  note = NoteFor(sealed, input, Observer::ConsumerCsm0);
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::ConsumerNotRequested);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  note = NoteFor(sealed, input, Observer::ConsumerGeometryOutline);
  note.packageKey.contentHash += 1u;
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::EntryMismatch);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  note = NoteFor(sealed, input, Observer::ConsumerGeometryOutline);
  note.materialToken += 1u;
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::EntryMismatch);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  note = NoteFor(sealed, input, Observer::ConsumerGeometryOutline);
  note.identityToken += 1u;
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::EntryMismatch);
  CHECK(observer->entry(sealed.tableIndex)->actualConsumerMask == 0u);

  observer->beginFrame(ValidContext(Observer::Mode::Off));
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::ModeOff);
  CHECK(observer->size() == 0u);

  observer->beginFrame(ValidContext(Observer::Mode::Consume));
  CHECK(observer->noteActualConsumer(note) ==
        Observer::ActualNoteResult::ConsumeNotAdmitted);
  CHECK(observer->size() == 0u);
}

void TestFixedCapacityAndBudgetAreDeferred() {
  auto observer = std::make_unique<Observer>();
  auto context = ValidContext();
  context.observationBudget = 2u;
  observer->beginFrame(context);
  CHECK(observer->seal(ValidInput()).recorded);
  CHECK(observer->seal(ValidInput()).recorded);
  const auto budget = observer->seal(ValidInput());
  CHECK(!budget.recorded);
  CHECK(budget.disposition == Observer::Disposition::DeferredBudget);
  CHECK(observer->size() == 2u);
  CHECK(observer->diagnostics().deferredBudget == 1u);

  context.observationBudget = Observer::kCapacity;
  observer->beginFrame(context);
  for (size_t i = 0u; i < Observer::kCapacity; ++i)
    CHECK(observer->seal(ValidInput()).recorded);
  const auto capacity = observer->seal(ValidInput());
  CHECK(!capacity.recorded);
  CHECK(capacity.disposition == Observer::Disposition::DeferredCapacity);
  CHECK(observer->size() == Observer::kCapacity);
  CHECK(observer->entry(Observer::kCapacity - 1u) != nullptr);
  CHECK(observer->entry(Observer::kCapacity) == nullptr);
  CHECK(observer->diagnostics().deferredCapacity == 1u);
}

void TestUnknownConsumerAndInvalidFrameFailClosed() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());
  auto input = ValidInput();
  input.requestedConsumerMask = 1u << 31u;
  const auto invalidConsumer = observer->seal(input);
  CHECK(invalidConsumer.proofState == Observer::ProofState::Rejected);
  CHECK(invalidConsumer.disposition ==
        Observer::Disposition::InvalidConsumerMask);
  CHECK(invalidConsumer.effectiveConsumerMask == input.requestedConsumerMask);

  auto context = ValidContext();
  context.catalogSnapshotRevision = 0u;
  const auto begin = observer->beginFrame(context);
  const auto invalidFrame = observer->seal(ValidInput());
  CHECK(begin.effectiveMode == Observer::Mode::Off);
  CHECK(begin.disposition == Observer::Disposition::InvalidFrameContext);
  CHECK(invalidFrame.disposition ==
        Observer::Disposition::InvalidFrameContext);
  CHECK(!invalidFrame.recorded);
}

}  // namespace

int main() {
  TestOffAndObservePreserveCanonicalWorkload();
  TestConsumeIsHardDisabled();
  TestExactGenerationAndFrameGates();
  TestProofStateCeilings();
  TestNonFiniteBoundsReject();
  TestActualConsumersRequireExplicitDrawEvidence();
  TestMainAlsoRequiresExplicitBoundDrawEvidence();
  TestActualNotesRequireExactCurrentEntry();
  TestFixedCapacityAndBudgetAreDeferred();
  TestUnknownConsumerAndInvalidFrameFailClosed();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d persistent package observer checks failed\n",
                 g_failures);
    return 1;
  }
  std::puts("war3_persistent_gpu_package_observer_test: PASS");
  return 0;
}
