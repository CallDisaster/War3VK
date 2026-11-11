#include "../tools/render_host/d3d9_resource_registry.h"
#include <cstdio>
using namespace warvk::host::d9;
int main() {
  unsigned checks=0,failures=0;
  auto check=[&](bool ok){++checks;if(!ok)++failures;};
  ResourceRegistry client,host;
  ResourceRef prior{};
  for(uint32_t generation=1;generation<=256;++generation) {
    const auto type=Type(1+(generation-1)%7);
    const auto ref=client.reserve(type);
    check(ref==ResourceRef{1,generation});check(client.liveCount()==0&&client.reservedCount()==1);
    check(!client.matches(ref,type));check(host.canCreate(ref,type));
    const auto reserved=client;
    check(!client.commitReservation({ref.id,ref.generation+1},type));
    check(reserved==client);
    check(host.commitCreate(ref,type));check(client.commitReservation(ref,type));
    check(client.matches(ref,type)&&host.matches(ref,type));
    const auto committed=host;
    for(uint32_t candidate=1;candidate<=7;++candidate)
      check(host.matches(ref,Type(candidate))==(Type(candidate)==type));
    check(!host.canCreate(ref,type));check(!host.commitCreate(ref,type));
    check(!host.destroy(prior,type));check(!host.destroy({ref.id,ref.generation+1},type));
    check(committed==host);
    check(host.destroy(ref,type));check(client.destroy(ref,type));
    check(!host.matches(ref,type)&&host.liveCount()==0&&client.liveCount()==0);
    prior=ref;
  }
  // Both local allocation failure and proven remote preflight refusal leave
  // the SAME candidate generation available. An uncertain request is retained.
  auto ref=client.reserve(Type::Texture);
  check(ref==ResourceRef{1,257});check(client.cancelReservation(ref,Type::Texture));
  check(client.reserve(Type::Texture)==ref);check(!host.matches(ref,Type::Texture));
  check(client.reservedCount()==1); // No automatic reuse without a valid outcome.
  const auto uncertain=client;
  check(!client.destroy(ref,Type::Texture));check(!client.cancelReservation(ref,Type::IndexBuffer));
  check(uncertain==client);
  check(client.cancelReservation(ref,Type::Texture));
  std::array<ResourceRef,MaxResources> refs{};
  for(auto& item:refs){item=client.reserve(Type::VertexBuffer);check(ValidRef(item));check(client.commitReservation(item,Type::VertexBuffer));}
  check(client.liveCount()==64&&client.reservedCount()==0);check(!ValidRef(client.reserve(Type::Texture)));
  check(client.destroy(refs[23],Type::VertexBuffer));
  ref=client.reserve(Type::Texture);check(ref==ResourceRef{refs[23].id,refs[23].generation+1});
  check(client.cancelReservation(ref,Type::Texture));
  check(!client.canCreate({0,1},Type::Texture));check(!client.canCreate({65,1},Type::Texture));
  check(!client.canCreate({1,0},Type::Texture));check(!ValidRef(client.reserve(Type::None)));
  BasicResourceRegistry<2> exhausted;
  for(uint32_t id=1;id<=64;++id)for(uint32_t gen=1;gen<=2;++gen){
    auto item=exhausted.reserve(Type::EventQuery);check(item==ResourceRef{id,gen});
    check(exhausted.commitReservation(item,Type::EventQuery));check(exhausted.destroy(item,Type::EventQuery));
    check(!exhausted.matches(item,Type::EventQuery));
  }
  check(!ValidRef(exhausted.reserve(Type::EventQuery)));
  check(!exhausted.canCreate({1,1},Type::EventQuery));check(!exhausted.canCreate({1,3},Type::EventQuery));
  std::printf("d3d9 resource registry checks=%u failures=%u bits=%zu\n",checks,failures,sizeof(void*)*8);
  return failures?1:0;
}
