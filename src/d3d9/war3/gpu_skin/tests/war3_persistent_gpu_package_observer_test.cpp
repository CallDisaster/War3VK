#include "../war3_persistent_gpu_package_observer.h"

#include <cstdio>
#include <limits>
#include <memory>

namespace {

using Observer =
    dxvk::war3::gpu_skin::War3PersistentGpuPackageObserver;

int g_failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                   __LINE__, #condition);                                    \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

Observer::FrameContext ValidContext(
    Observer::Mode mode = Observer::Mode::Observe) {
  Observer::FrameContext context = {};
  context.requestedMode = mode;
  context.frameSerial = 101u;
  context.policyRevision = 7u;
  context.stage = Observer::kRequiredStage;
  context.mapEpoch = 3u;
  context.deviceEpoch = 5u;
  context.packageGeneration = 11u;
  context.sourceGeneration = 13u;
  context.canonicalWorkload = 900u;
  return context;
}

Observer::SealInput ValidInput() {
  Observer::SealInput input = {};
  input.frameSerial = 101u;
  input.policyRevision = 7u;
  input.stage = Observer::kRequiredStage;
  input.mapEpoch = 3u;
  input.deviceEpoch = 5u;
  input.packageGeneration = 11u;
  input.sourceGeneration = 13u;
  input.identityToken = 17u;
  input.sourceToken = 19u;
  input.materialToken = 23u;
  input.alphaToken = 29u;
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
  input.packageContentReady = true;
  input.materialKnown = true;
  input.materialExact = true;
  input.alphaKnown = true;
  input.alphaExact = true;
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
  note.packageGeneration = input.packageGeneration;
  note.sourceGeneration = input.sourceGeneration;
  note.identityToken = input.identityToken;
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
  input.packageGeneration += 1u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::GenerationMismatch);

  input = ValidInput();
  input.sourceGeneration += 1u;
  result = observer->seal(input);
  CHECK(result.disposition == Observer::Disposition::GenerationMismatch);
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
  input.packageContentReady = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::ContentPending);

  input = ValidInput();
  input.materialKnown = false;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);

  input = ValidInput();
  input.alphaExact = false;
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
  CHECK(observer->diagnostics().packageInputReady == 6u);
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

void TestPreSubmittedMainIsSeparateFromProof() {
  auto observer = std::make_unique<Observer>();
  observer->beginFrame(ValidContext());

  auto input = ValidInput();
  input.materialKnown = false;
  input.preSubmittedConsumerMask = Observer::ConsumerMain;
  auto result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::PackageInputReady);
  CHECK(result.eligibleConsumerMask == 0u);
  CHECK(result.preSubmittedAccepted);
  CHECK(result.actualConsumerMask == Observer::ConsumerMain);
  CHECK(result.wouldUseConsumerMask == 0u);
  CHECK((result.actualConsumerMask &
        (Observer::ConsumerCsm0 | Observer::ConsumerCsm1 |
         Observer::ConsumerCsm2 | Observer::ConsumerCsm3 |
         Observer::ConsumerPointShadow |
         Observer::ConsumerGeometryOutline)) == 0u);

  observer->beginFrame(ValidContext());
  input = ValidInput();
  input.requestedConsumerMask = Observer::ConsumerCsm0;
  input.preSubmittedConsumerMask = Observer::ConsumerMain;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(!result.preSubmittedAccepted);
  CHECK(result.actualConsumerMask == 0u);
  CHECK(observer->diagnostics().invalidPreSubmittedMasks == 1u);

  observer->beginFrame(ValidContext());
  input = ValidInput();
  input.preSubmittedConsumerMask = Observer::ConsumerCsm1;
  result = observer->seal(input);
  CHECK(result.proofState == Observer::ProofState::FullyEquivalent);
  CHECK(!result.preSubmittedAccepted);
  CHECK(result.actualConsumerMask == 0u);
  CHECK(observer->diagnostics().invalidPreSubmittedMasks == 1u);
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
  context.packageGeneration = 0u;
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
  TestPreSubmittedMainIsSeparateFromProof();
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
