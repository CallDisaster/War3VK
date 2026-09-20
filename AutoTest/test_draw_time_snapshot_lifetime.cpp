#include "../src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h"
#include <memory>
#include <iostream>
#include <stdexcept>

using namespace dxvk::war3::render;
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) throw std::runtime_error(#x); } while(false)

// CPU reference owners instantiate the same transition implementation used by
// the device. They prove cache reference/field behavior, NOT GPU completion.
struct Resource {};
struct Entry {
  std::shared_ptr<Resource> positionBuffer, positionPinnedAllocation, positionSnapshotPage, positionSnapshotLease;
  std::shared_ptr<Resource> indexBuffer, indexPinnedAllocation, indexSnapshotPage;
  std::shared_ptr<Resource> uvBuffer, uvPinnedAllocation, uvSnapshotPage, uvSnapshotLease;
  uint64_t positionSnapshotOffset=0, uvSnapshotOffset=0;
  uint64_t positionCapacity=0, indexCapacity=0, uvCapacity=0, ownedGpuBytes=0;
  uint64_t positionInfo=0, uvInfo=0;
  uint32_t uvStride=0, uvOffset=0, uvFormat=0;
  uint64_t uvSourceProof=0;
  uint64_t frameSerial=10, lastAccessFrameSerial=10, lastAttemptFrameSerial=0;
  bool captureComplete=true, uvSharesPositionBuffer=false, indexed=true;
  bool HasCompleteBacking() const {
    return captureComplete&&positionBuffer&&positionInfo&&(!indexed||indexBuffer);
  }
};

int main(){try{
  Entry e;
  // Position succeeds, index fails 100 times: success age never refreshes,
  // retained position allocation remains visible in the byte account.
  e.positionBuffer=std::make_shared<Resource>();
  e.positionSnapshotPage=std::make_shared<Resource>();
  const auto buffer=e.positionBuffer, page=e.positionSnapshotPage;
  for(uint64_t frame=11;frame!=111;++frame) {
    {
      War3DrawTimeSnapshotCaptureAttempt attempt(e,frame);
      CHECK(!e.captureComplete);CHECK(e.lastAttemptFrameSerial==frame);
      e.positionCapacity=512*1024;e.positionInfo=512*1024;
      CHECK(!attempt.commit());
    }
    CHECK(e.frameSerial==10&&e.lastAccessFrameSerial==10);
    CHECK(e.ownedGpuBytes==512*1024&&!e.captureComplete);
    CHECK(e.positionBuffer==buffer&&e.positionSnapshotPage==page);
  }
  // Recovery reuses the retained backing and publishes only after all required
  // inputs succeed. Duplicate accounting excludes a position/UV alias.
  {
    War3DrawTimeSnapshotCaptureAttempt attempt(e,111);
    e.indexBuffer=std::make_shared<Resource>();e.indexCapacity=256;
    e.uvSharesPositionBuffer=true;e.uvCapacity=512*1024;
    e.captureComplete=true;CHECK(attempt.commit());
  }
  CHECK(e.frameSerial==111&&e.lastAccessFrameSerial==111&&e.captureComplete);
  CHECK(e.ownedGpuBytes==512*1024+256);
  // Exception before explicit commit is not a successfully refreshed capture.
  try {
    War3DrawTimeSnapshotCaptureAttempt attempt(e,112);
    e.captureComplete=true;e.indexCapacity=512;
    throw std::runtime_error("injected");
  } catch(const std::runtime_error&) {}
  CHECK(!e.captureComplete&&e.frameSerial==111&&e.lastAccessFrameSerial==111);
  CHECK(e.ownedGpuBytes==512*1024+512);
  // Releasing the position invalidates its alias, Page, pin and offset too.
  // Simulate queued commands retaining their independent Buffer/pin owners.
  e.positionPinnedAllocation=std::make_shared<Resource>();
  const auto pendingBuffer=e.positionBuffer, pendingPin=e.positionPinnedAllocation;
  e.uvBuffer=e.positionBuffer;e.uvPinnedAllocation=e.positionPinnedAllocation;
  e.uvSnapshotPage=e.positionSnapshotPage;
  e.positionSnapshotOffset=256;e.uvSnapshotOffset=256;
  e.uvInfo=1024;e.uvStride=32;e.uvOffset=24;e.uvFormat=3;
  War3ReleaseDrawTimePositionBacking(e);
  CHECK(!e.positionBuffer&&!e.positionPinnedAllocation&&!e.positionSnapshotPage);
  CHECK(e.positionSnapshotOffset==0&&e.positionCapacity==0&&e.positionInfo==0);
  CHECK(!e.uvBuffer&&!e.uvPinnedAllocation&&!e.uvSnapshotPage);
  CHECK(e.uvSnapshotOffset==0&&e.uvCapacity==0&&e.uvInfo==0);
  CHECK(e.uvStride==0&&e.uvOffset==0&&e.uvFormat==0&&!e.uvSharesPositionBuffer);
  CHECK(e.ownedGpuBytes==512&&e.indexBuffer);
  CHECK(pendingBuffer&&pendingPin&&page); // no forced destruction of other owners
  // Independent UV and IB survive a position-only revocation (retry policy).
  e.uvBuffer=std::make_shared<Resource>();e.uvSnapshotPage=std::make_shared<Resource>();
  e.uvCapacity=2048;e.uvInfo=2048;e.uvSnapshotOffset=4096;
  const auto independentUv=e.uvBuffer, independentPage=e.uvSnapshotPage;
  War3ReleaseDrawTimePositionBacking(e);
  CHECK(e.uvBuffer==independentUv&&e.uvSnapshotPage==independentPage);
  CHECK(e.uvCapacity==2048&&e.uvInfo==2048&&e.uvSnapshotOffset==4096);
  CHECK(e.ownedGpuBytes==2560);
  // P1 CPU model: a successful no-UV capture releases only the stale
  // independent UV cache reference/proof after commit. Position/index backing
  // and separate queued owners remain intact.
  {
    Entry noUv;
    noUv.positionBuffer=std::make_shared<Resource>();
    noUv.positionSnapshotPage=std::make_shared<Resource>();
    noUv.positionCapacity=4096;noUv.positionInfo=4096;
    noUv.indexBuffer=std::make_shared<Resource>();noUv.indexCapacity=512;
    noUv.uvBuffer=std::make_shared<Resource>();
    noUv.uvPinnedAllocation=std::make_shared<Resource>();
    noUv.uvSnapshotPage=std::make_shared<Resource>();
    noUv.uvSnapshotOffset=2048;noUv.uvCapacity=2048;noUv.uvInfo=2048;
    noUv.uvStride=32;noUv.uvOffset=16;noUv.uvFormat=3;noUv.uvSourceProof=0x1234;
    const auto pendingUv=noUv.uvBuffer;
    const auto pendingPin=noUv.uvPinnedAllocation;
    const auto pendingPage=noUv.uvSnapshotPage;
    {
      War3DrawTimeSnapshotCaptureAttempt attempt(noUv,200);
      noUv.captureComplete=true;CHECK(attempt.commit());
      CHECK(noUv.ownedGpuBytes==4096+512+2048);
      War3ReleaseDrawTimeUvBacking(noUv);
    }
    CHECK(noUv.frameSerial==200&&noUv.lastAccessFrameSerial==200);
    CHECK(!noUv.uvBuffer&&!noUv.uvPinnedAllocation&&!noUv.uvSnapshotPage);
    CHECK(noUv.uvSnapshotOffset==0&&noUv.uvCapacity==0&&noUv.uvInfo==0);
    CHECK(noUv.uvStride==0&&noUv.uvOffset==0&&noUv.uvFormat==0);
    CHECK(noUv.uvSourceProof==0&&!noUv.uvSharesPositionBuffer);
    CHECK(noUv.ownedGpuBytes==4096+512);
    CHECK(pendingUv&&pendingPin&&pendingPage);
  }
  // P1 CPU model: after the no-UV release, a later exact independent UV
  // capture can repopulate the same entry and publish normally.
  {
    Entry later;
    later.positionBuffer=std::make_shared<Resource>();later.positionCapacity=4096;later.positionInfo=4096;
    later.indexBuffer=std::make_shared<Resource>();later.indexCapacity=512;
    War3ReleaseDrawTimeUvBacking(later);
    CHECK(!later.uvBuffer&&!later.uvSnapshotPage&&later.uvCapacity==0);
    later.uvBuffer=std::make_shared<Resource>();
    later.uvPinnedAllocation=std::make_shared<Resource>();
    later.uvSnapshotPage=std::make_shared<Resource>();
    later.uvCapacity=2048;later.uvInfo=2048;later.uvStride=32;
    later.uvOffset=16;later.uvFormat=3;later.uvSourceProof=0x55;
    {
      War3DrawTimeSnapshotCaptureAttempt attempt(later,201);
      later.captureComplete=true;CHECK(attempt.commit());
    }
    CHECK(later.uvBuffer&&later.uvCapacity==2048&&later.uvSourceProof==0x55);
    CHECK(later.ownedGpuBytes==4096+512+2048);
  }
  // P1 CPU model: a failed/alpha-missing-UV capture does not early-clear UV
  // backing and never publishes captureComplete.
  {
    Entry failed;
    failed.positionBuffer=std::make_shared<Resource>();failed.positionInfo=4096;
    failed.indexBuffer=std::make_shared<Resource>();
    failed.uvBuffer=std::make_shared<Resource>();
    failed.uvSnapshotPage=std::make_shared<Resource>();
    failed.uvCapacity=2048;failed.uvInfo=2048;
    const auto retainedUv=failed.uvBuffer;
    {
      War3DrawTimeSnapshotCaptureAttempt attempt(failed,300);
      failed.captureComplete=false;
      CHECK(!attempt.commit());
    }
    CHECK(!failed.captureComplete);
    CHECK(failed.uvBuffer==retainedUv&&failed.uvCapacity==2048);
    CHECK(failed.ownedGpuBytes==2048);
  }
  // P1 CPU model: helper on a position alias clears UV wrappers/proof but does
  // not drop the position page/alias ownership.
  {
    Entry aliasUv;
    aliasUv.positionBuffer=std::make_shared<Resource>();
    aliasUv.positionSnapshotPage=std::make_shared<Resource>();
    aliasUv.positionCapacity=4096;aliasUv.positionInfo=4096;
    aliasUv.positionSnapshotOffset=256;
    aliasUv.indexBuffer=std::make_shared<Resource>();aliasUv.indexCapacity=512;
    aliasUv.uvSharesPositionBuffer=true;
    aliasUv.uvBuffer=aliasUv.positionBuffer;
    aliasUv.uvSnapshotPage=aliasUv.positionSnapshotPage;
    aliasUv.uvSnapshotOffset=aliasUv.positionSnapshotOffset;
    aliasUv.uvInfo=4096;aliasUv.uvStride=32;aliasUv.uvOffset=24;aliasUv.uvFormat=3;
    aliasUv.uvSourceProof=0xabc;
    const auto aliasPage=aliasUv.positionSnapshotPage;
    const auto aliasBuffer=aliasUv.positionBuffer;
    War3ReleaseDrawTimeUvBacking(aliasUv);
    CHECK(aliasUv.positionBuffer==aliasBuffer&&aliasUv.positionSnapshotPage==aliasPage);
    CHECK(aliasUv.positionSnapshotOffset==256&&aliasUv.positionCapacity==4096);
    CHECK(aliasUv.positionInfo==4096&&!aliasUv.uvSharesPositionBuffer);
    CHECK(!aliasUv.uvBuffer&&!aliasUv.uvSnapshotPage&&aliasUv.uvInfo==0);
    CHECK(aliasUv.uvSourceProof==0&&aliasUv.ownedGpuBytes==4096+512);
  }
  // P1 CPU model: borrowed/direct UV source has zero owned capacity; release
  // drops only cache wrappers, while independent external Rc owners survive.
  {
    Entry directSource;
    directSource.positionBuffer=std::make_shared<Resource>();
    directSource.positionCapacity=4096;directSource.positionInfo=4096;
    directSource.indexBuffer=std::make_shared<Resource>();directSource.indexCapacity=512;
    directSource.uvBuffer=std::make_shared<Resource>();
    directSource.uvPinnedAllocation=std::make_shared<Resource>();
    directSource.uvInfo=2048;directSource.uvStride=32;directSource.uvOffset=0;
    directSource.uvFormat=3;directSource.uvSourceProof=0xdef;
    const auto externalUv=directSource.uvBuffer;
    const auto externalPin=directSource.uvPinnedAllocation;
    CHECK(directSource.ownedGpuBytes==0);
    War3ReleaseDrawTimeUvBacking(directSource);
    CHECK(!directSource.uvBuffer&&!directSource.uvPinnedAllocation);
    CHECK(!directSource.uvSnapshotPage&&directSource.uvInfo==0);
    CHECK(directSource.uvStride==0&&directSource.uvOffset==0&&directSource.uvFormat==0);
    CHECK(directSource.uvCapacity==0&&directSource.uvSourceProof==0);
    CHECK(directSource.ownedGpuBytes==4096+512);
    CHECK(externalUv&&externalPin);
  }
  // Non-indexed success remains legal; borrowed/direct resources count zero.
  Entry direct;direct.indexed=false;
  {
    War3DrawTimeSnapshotCaptureAttempt attempt(direct,1);
    direct.positionBuffer=pendingBuffer;direct.positionInfo=100;
    direct.captureComplete=true;CHECK(attempt.commit());
  }
  CHECK(direct.captureComplete&&direct.ownedGpuBytes==0&&direct.frameSerial==1);
  // Exact UV census resolver: page identity + offset proof, positionCapacity
  // accounting; zero/stale uvCapacity is representation noise for aliases.
  {
    Entry alias;
    alias.positionSnapshotPage = std::make_shared<Resource>();
    alias.uvSnapshotPage = alias.positionSnapshotPage;
    alias.positionSnapshotOffset = 256;
    alias.uvSnapshotOffset = 256;
    alias.positionCapacity = 4096;
    alias.uvCapacity = 0;
    alias.uvSharesPositionBuffer = true;
    const auto page = alias.positionSnapshotPage;
    const auto refs = page.use_count();
    War3DrawTimeUvCensusSpan span{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(alias, span) ==
          War3DrawTimeUvCensusSpanStatus::PositionAlias);
    CHECK(span.offset == 256 && span.capacity == 4096);
    CHECK(page.use_count() == refs);

    alias.uvCapacity = 8192; // stale previous independent UV capacity
    span = {};
    CHECK(War3ResolveDrawTimeUvCensusSpan(alias, span) ==
          War3DrawTimeUvCensusSpanStatus::PositionAlias);
    CHECK(span.offset == 256 && span.capacity == 4096);
    CHECK(page.use_count() == refs);

    Entry mismatched = alias;
    mismatched.uvSnapshotPage = std::make_shared<Resource>();
    CHECK(War3ResolveDrawTimeUvCensusSpan(mismatched, span) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);

    Entry wrongOffset = alias;
    wrongOffset.uvSnapshotOffset = 257;
    CHECK(War3ResolveDrawTimeUvCensusSpan(wrongOffset, span) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);

    Entry zeroPosition = alias;
    zeroPosition.positionCapacity = 0;
    CHECK(War3ResolveDrawTimeUvCensusSpan(zeroPosition, span) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
  }
  // External both-null alias owns no census page; stale uvCapacity is ignored.
  {
    Entry external;
    external.uvSharesPositionBuffer = true;
    external.uvCapacity = 1234;
    War3DrawTimeUvCensusSpan span{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(external, span) ==
          War3DrawTimeUvCensusSpanStatus::None);
    external.positionCapacity = 1;
    CHECK(War3ResolveDrawTimeUvCensusSpan(external, span) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);
  }
  // Independent UV remains exact; stale null-page state is invalid.
  {
    Entry independent;
    independent.uvSnapshotPage = std::make_shared<Resource>();
    independent.uvSnapshotOffset = 4096;
    independent.uvCapacity = 2048;
    War3DrawTimeUvCensusSpan span{};
    CHECK(War3ResolveDrawTimeUvCensusSpan(independent, span) ==
          War3DrawTimeUvCensusSpanStatus::Independent);
    CHECK(span.offset == 4096 && span.capacity == 2048);

    Entry stale;
    stale.uvCapacity = 2048;
    stale.uvSnapshotOffset = 4096;
    CHECK(War3ResolveDrawTimeUvCensusSpan(stale, span) ==
          War3DrawTimeUvCensusSpanStatus::Invalid);

    Entry none;
    CHECK(War3ResolveDrawTimeUvCensusSpan(none, span) ==
          War3DrawTimeUvCensusSpanStatus::None);
  }
  std::cout<<"snapshot lifetime checks="<<checks<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
