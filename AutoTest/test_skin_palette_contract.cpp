#include "../src/d3d9/war3/render/war3_canonical_draw.h"
#include "../src/d3d9/war3/render/war3_direct_packet_scratch.h"
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>

using namespace dxvk;
using namespace dxvk::war3::render;
namespace s = dxvk::war3::render::skin;
static unsigned checks = 0;
static void check(bool value, const char* name) {
  ++checks;
  if (!value) throw std::runtime_error(name);
}
static s::Selection owned() {
  s::Selection p;
  p.source=s::Source::OwnedPartSnapshot; p.space=s::Space::World;
  p.domain=s::Domain::VertexGroups; p.runtimeModel=0x1000; p.part=0x2000;
  p.ownerEpoch=7; p.publicationTicket=21; p.hash=91; p.slot=8;
  p.actualGroupCount=9; p.frameTag=30;
  return p;
}
int main() {
  try {
    auto p=owned();
    check(s::OwnedSnapshotMatches(p,0x1000,0x2000,7,8,30,9),"owned hit");
    check(!s::OwnedSnapshotMatches(p,0x3000,0x2000,7,8,30,9),"slot reassigned to B");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x4000,7,8,30,9),"part mismatch");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x2000,8,8,30,9),"map epoch mismatch");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x2000,7,UINT32_MAX,30,9),"invalid raw slot");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x2000,7,9,30,9),"slot moved");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x2000,7,8,31,9),"stale frame");
    check(!s::OwnedSnapshotMatches(p,0x1000,0x2000,7,8,30,10),"actual count insufficient");
    check(!s::GroupRange(0,9)&&!s::GroupRange(1,257),"bounded group domain");
    check(s::Usable(p,p.part,9,p.hash),"owned payload admissible");
    check(!s::Usable(p,p.part,9,92),"payload hash differs");
    for(auto source:{s::Source::Unknown,s::Source::CapturedRawArena,s::Source::LegacyGlobalSlot,
                     s::Source::LegacySlotCache,s::Source::PoseGroups,s::Source::CModelGroups}) {
      auto q=p; q.source=source;
      check(!s::Usable(q,p.part,9,p.hash),"unproved source rejected");
    }
    auto q=p; q.publicationTicket=22; q.hash=92;
    check(s::CanReplace(p,q),"same owner legal fresh replacement");
    q.runtimeModel=0x3000;check(!s::CanReplace(p,q),"replacement owner mismatch");
    q=p;q.part=0x4000;check(!s::CanReplace(p,q),"replacement part mismatch");
    q=p;q.ownerEpoch=8;check(!s::CanReplace(p,q),"replacement epoch mismatch");
    q=p;q.domain=s::Domain::Bones;check(!s::CanReplace(p,q),"bones not groups");
    q=p;q.space=s::Space::ModelLocal;check(!s::Usable(q,p.part,9,p.hash),"wrong coordinate space");
    q=p;q.publicationTicket=0;check(!s::Usable(q,p.part,9,p.hash),"unknown publication denied");
    check(p.slotAllocationGeneration==0,"no fabricated native allocation generation");
    std::atomic<uint64_t> tickets{UINT64_MAX-1};
    check(s::NextTicket(tickets)==UINT64_MAX,"last nonzero ticket");
    check(s::NextTicket(tickets)==0&&s::NextTicket(tickets)==0,"ticket never wraps");
    std::atomic_flag flag=ATOMIC_FLAG_INIT;
    {s::TryCell first(flag);s::TryCell conflict(flag);check(bool(first)&&!conflict,"nonblocking conflict");}
    {s::TryCell available(flag);check(bool(available),"lock released by owner only");}
    // Actual canonical adapter: Ready status must not relabel an owned source.
    war3::shadow::ShadowDrawPacket packet;
    packet.path=war3::shadow::ShadowDrawPath::Skinned;
    packet.renderable.renderablePart=reinterpret_cast<void*>(p.part);
    packet.renderable.runtimeModelPtr=reinterpret_cast<void*>(p.runtimeModel);
    packet.renderable.jHandle=100;
    std::vector<float> positions={0,0,0,1,0,0,0,1,0};
    std::vector<uint8_t> groups={0,5,8};
    std::vector<Matrix4> matrices(9);
    packet.resource.positions=&positions;
    packet.resource.vertexGroupIndices=&groups;
    CurrentDrawAuthoritativeSample sample; sample.status=CurrentDrawResolveStatus::Ready;
    CanonicalShadowBuildInputs in;
    in.ownSkinContracts=false;
    in.packet=&packet;in.vertexCount=3;in.currentDrawSample=&sample;
    in.effectiveRuntimeGroupPalette=&matrices;in.effectiveRuntimeGroupPaletteHash=p.hash;
    in.effectiveMaxVertexGroupSlot=8;in.selectedPalette=p;in.hasSelectedPalette=true;
    in.liveRuntimeGroupPaletteReady=true;in.authoritativeGroupSlots=&groups;
    in.authoritativeGroupSlotsReady=true;
    CanonicalShadowDrawItem item;
    check(BuildCanonicalShadowDrawItem(in,item)&&item.readyForShadowConsumer(),"owned canonical ready");
    check(item.instance.skin.paletteSource==CanonicalPaletteSource::OwnedPartWorldPalette,"actual source not Ready label");
    check(item.instance.worldTransform.source==CanonicalWorldTransformSource::CurrentDrawPaletteWorld,"owned world not doubled");
    check(item.instance.skin.paletteRef==&matrices,"same immutable payload vector");
    in.selectedPalette.source=s::Source::CapturedRawArena;
    BuildCanonicalShadowDrawItem(in,item);check(!item.readyForShadowConsumer(),"canonical raw arena rejected");
    in.selectedPalette=p;in.selectedPalette.actualGroupCount=8;
    BuildCanonicalShadowDrawItem(in,item);check(!item.readyForShadowConsumer(),"canonical count rejected");
    in.selectedPalette=p;in.selectedPalette.hash++;
    BuildCanonicalShadowDrawItem(in,item);check(!item.readyForShadowConsumer(),"canonical wrong selected bytes");
    in.selectedPalette=p;in.selectedPalette.source=s::Source::CapturedWriter;
    in.selectedPalette.captureSerial=9;in.selectedPalette.meshPayload=0x5000;
    BuildCanonicalShadowDrawItem(in,item);check(item.readyForShadowConsumer(),"exact writer accepted");
    check(item.instance.skin.paletteSource==CanonicalPaletteSource::CurrentDrawCapturedPalette,"writer classification");
    in.hasSelectedPalette=false;in.selectedPalette={};
    BuildCanonicalShadowDrawItem(in,item);check(item.readyForShadowConsumer(),"gate off legacy canonical");
    packet.paletteSelection=p;sample.paletteSelection=p;
    ResetShadowDrawPacketPreserveScratch(packet);
    ResetCurrentDrawAuthoritativeSamplePreserveScratch(sample);
    check(packet.paletteSelection.hash==0&&sample.paletteSelection.hash==0,"packet and sample reset metadata");
    std::cout << "PASS " << checks << " checks\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
