#include "../tools/render_host/d3d9_resource_registry.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace warvk::host::d9;
// Every allocation of the process is counted: a table reserved whole must not
// allocate again (the host commits a create after the GPU object exists).
static std::size_t allocations=0;
void* operator new(std::size_t bytes){++allocations;if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
int main() {
  ResourceQuota quota;
  const auto require=[](bool ok){if(!ok){std::fputs("resource quota contract failed\n",stderr);std::abort();}};
  for(auto type:{Type::VertexBuffer,Type::Texture,Type::Declaration,Type::VertexShader,Type::PixelShader}) {
    const uint32_t bound=type==Type::VertexBuffer?8:type==Type::Texture?65472:4;
    for(uint32_t i=0;i<bound;++i) {
      const auto actual=type==Type::VertexBuffer&&i%2?Type::IndexBuffer:type;
      require(!quota.full(actual));quota.created(actual);
    }
    require(quota.full(type)&&!quota.full(Type::EventQuery));
    if(type==Type::VertexBuffer)require(quota.full(Type::IndexBuffer));
    if(type==Type::VertexShader)require(!quota.full(Type::PixelShader));
    quota.destroyed(type);require(!quota.full(type));quota.created(type);
  }
  unsigned checks=0,failures=0;
  auto check=[&](bool ok){++checks;if(!ok)++failures;};
  // Game-sized tables (MaxResources): heap, not the test thread's stack.
  auto clientOwner=std::make_unique<ResourceRegistry>(),hostOwner=std::make_unique<ResourceRegistry>();
  auto& client=*clientOwner;auto& host=*hostOwner;
  auto snapshot=std::make_unique<ResourceRegistry>();
  ResourceRef prior{};
  for(uint32_t generation=1;generation<=256;++generation) {
    const auto type=Type(1+(generation-1)%7);
    const auto ref=client.reserve(type);
    check(ref==ResourceRef{1,generation});check(client.liveCount()==0&&client.reservedCount()==1);
    check(!client.matches(ref,type));check(host.canCreate(ref,type));
    *snapshot=client;const auto& reserved=*snapshot;
    check(!client.commitReservation({ref.id,ref.generation+1},type));
    check(reserved==client);
    check(host.commitCreate(ref,type));check(client.commitReservation(ref,type));
    check(client.matches(ref,type)&&host.matches(ref,type));
    *snapshot=host;const auto& committed=*snapshot;
    for(uint32_t candidate=1;candidate<=7;++candidate)
      check(host.matches(ref,Type(candidate))==(Type(candidate)==type));
    check(!host.canCreate(ref,type));check(!host.commitCreate(ref,type));
    check(!host.destroy(prior,type));check(!host.destroy({ref.id,ref.generation+1},type));
    check(committed==host);
    check(host.destroy(ref,type));check(client.destroy(ref,type));
    require(!host.canCreate(ref,type)&&!host.canCreate({ref.id,ref.generation+2},type));
    require(client==host);
    check(!host.matches(ref,type)&&host.liveCount()==0&&client.liveCount()==0);
    prior=ref;
  }
  // Both local allocation failure and proven remote preflight refusal leave
  // the SAME candidate generation available. An uncertain request is retained.
  auto ref=client.reserve(Type::Texture);
  check(ref==ResourceRef{1,257});check(client.cancelReservation(ref,Type::Texture));
  check(client.reserve(Type::Texture)==ref);check(!host.matches(ref,Type::Texture));
  check(client.reservedCount()==1); // No automatic reuse without a valid outcome.
  *snapshot=client;const auto& uncertain=*snapshot;
  check(!client.destroy(ref,Type::Texture));check(!client.cancelReservation(ref,Type::IndexBuffer));
  check(uncertain==client);
  check(client.cancelReservation(ref,Type::Texture));
  auto refsOwner=std::make_unique<std::array<ResourceRef,MaxResources>>();auto& refs=*refsOwner;
  for(auto& item:refs){item=client.reserve(Type::VertexBuffer);check(ValidRef(item));check(client.commitReservation(item,Type::VertexBuffer));}
  check(client.liveCount()==MaxResources&&client.reservedCount()==0);check(!ValidRef(client.reserve(Type::Texture)));
  check(client.destroy(refs[23],Type::VertexBuffer));
  ref=client.reserve(Type::Texture);check(ref==ResourceRef{refs[23].id,refs[23].generation+1});
  check(client.cancelReservation(ref,Type::Texture));
  check(!client.canCreate({0,1},Type::Texture));check(!client.canCreate({MaxResources+1,1},Type::Texture));
  check(!client.canCreate({1,0},Type::Texture));check(!ValidRef(client.reserve(Type::None)));
  auto exhaustedOwner=std::make_unique<BasicResourceRegistry<2>>();auto& exhausted=*exhaustedOwner;
  for(uint32_t id=1;id<=MaxResources;++id)for(uint32_t gen=1;gen<=2;++gen){
    auto item=exhausted.reserve(Type::EventQuery);check(item==ResourceRef{id,gen});
    check(exhausted.commitReservation(item,Type::EventQuery));check(exhausted.destroy(item,Type::EventQuery));
    check(!exhausted.matches(item,Type::EventQuery));
  }
  check(!ValidRef(exhausted.reserve(Type::EventQuery)));
  check(!exhausted.canCreate({1,1},Type::EventQuery));check(!exhausted.canCreate({1,3},Type::EventQuery));
  // The host's table: reserved whole at the session's start, then no create,
  // however high its id, allocates; and it stays equal to one that grew.
  auto reservedOwner=std::make_unique<ResourceRegistry>(),grownOwner=std::make_unique<ResourceRegistry>();
  auto& reserved=*reservedOwner;auto& grown=*grownOwner;
  check(reserved.reserveAll());check(reserved==grown);
  const auto before=allocations;
  for(const uint32_t id:{uint32_t(1),uint32_t(700),uint32_t(16385),MaxResources}) {
    check(reserved.canCreate({id,1},Type::Texture));check(reserved.commitCreate({id,1},Type::Texture));
    check(reserved.matches({id,1},Type::Texture));
  }
  check(allocations==before);check(reserved.liveCount()==4);
  for(const uint32_t id:{uint32_t(1),uint32_t(700),uint32_t(16385),MaxResources})
    check(grown.commitCreate({id,1},Type::Texture));
  check(allocations>before);check(reserved==grown);
  check(reserved.reserveAll());check(reserved==grown);check(reserved.liveCount()==4);
  std::printf("d3d9 resource registry checks=%u failures=%u bits=%zu\n",checks,failures,sizeof(void*)*8);
  return failures?1:0;
}
