#include "../tools/render_host/sample_envelope.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

using namespace warvk::host;
using namespace warvk::host::envelope;
static unsigned checks=0;
static void Check(bool value,unsigned line){++checks;if(!value){std::fprintf(stderr,"envelope failure line=%u checks=%u\n",line,checks);std::exit(1);}}
#define CHECK(x) Check(bool(x),__LINE__)
static const inbox::Scope ScopeA{{0x0807060504030201ull,0x1817161514131211ull},3,7};
using Packet=std::array<uint8_t,slots::SlotBytes+1>;
static std::array<uint8_t,MaxPayloadBytes> payload{};
static_assert(!std::is_copy_constructible_v<Validator>&&!std::is_move_constructible_v<Validator>);
static slots::Key Key(Header h,uint32_t bytes){return {h.scope.connection,h.scope.map,h.scope.device,h.transfer,1,0,bytes};}
static Packet Encoded(Header h,uint32_t bytes,size_t& count){
  Packet p{};CHECK(Encode(h,payload.data(),bytes,p.data(),slots::SlotBytes,count)==Error::None);return p;
}
static void Empty(const View& v){CHECK(!v.data&&!v.bytes&&v.header.scope.connection.empty()&&
  !v.header.scope.map&&!v.header.scope.device&&!v.header.sourceFrame&&!v.header.attempt&&!v.header.transfer);}
static void Reject(const Packet& bad,size_t count,Error wanted){
  Validator owner(ScopeA);View out{{ScopeA,99,99,99},payload.data(),9};
  CHECK(owner.accept(Key({ScopeA,9,1,1},uint32_t(count)),bad.data(),count,out)==wanted);Empty(out);
  CHECK(owner.progress().fault==wanted&&owner.progress().transfer==0);
  size_t n=0;auto good=Encoded({ScopeA,9,1,1},1,n);
  CHECK(owner.accept(Key({ScopeA,9,1,1},uint32_t(n)),good.data(),n,out)==Error::Terminal);Empty(out);
}
static void Golden(){payload[0]=0xa5;size_t n=0;auto p=Encoded({ScopeA,42,15,1},1,n);
  for(size_t i=0;i<n;++i)std::printf("%02x",unsigned(p[i]));std::putchar('\n');}
int main(int argc,char** argv){
  if(argc==2&&!std::strcmp(argv[1],"--golden")){Golden();return 0;}
  const Header first{ScopeA,100,2,1};size_t n=0;auto packet=Encoded(first,1,n);View view;
  CHECK(n==81);CHECK(Decode(packet.data(),n,view)==Error::None);
  CHECK(view.header.scope==ScopeA&&view.header.sourceFrame==100&&view.header.attempt==2&&view.header.transfer==1);
  CHECK(view.data==packet.data()+80&&view.bytes==1);
  for(size_t length=0;length<HeaderBytes;++length){view.data=payload.data();CHECK(Decode(packet.data(),length,view)==Error::Size);Empty(view);}
  CHECK(Decode(nullptr,n,view)==Error::NullBuffer);Empty(view);
  CHECK(Decode(packet.data(),slots::SlotBytes+1,view)==Error::Size);Empty(view);
  CHECK(Decode(packet.data(),n+1,view)==Error::Size);Empty(view);
  struct Mutation{unsigned offset,size;uint64_t value;Error expected;};
  for(const auto m:{Mutation{0,4,0,Error::Magic},Mutation{4,2,2,Error::Version},Mutation{6,2,1,Error::Version},
      Mutation{8,4,79,Error::Header},Mutation{12,4,1,Error::Flags},Mutation{16,4,82,Error::Size},
      Mutation{20,4,0,Error::Size},Mutation{20,4,65457,Error::Size},Mutation{40,8,0,Error::Scope},
      Mutation{48,8,0,Error::Scope},Mutation{56,8,0,Error::SourceFrame},Mutation{64,8,0,Error::Attempt},
      Mutation{64,8,uint64_t(UINT32_MAX)+1,Error::Attempt},Mutation{72,8,0,Error::Transfer}}){
    auto bad=packet;ipc::WriteLe(bad.data()+m.offset,m.value,m.size);Reject(bad,n,m.expected);
  }
  auto noNonce=packet;ipc::WriteLe(noNonce.data()+24,0,8);ipc::WriteLe(noNonce.data()+32,0,8);Reject(noNonce,n,Error::Scope);
  for(unsigned offset:{24u,32u,40u,48u}){auto bad=packet;bad[offset]^=0x80;Reject(bad,n,Error::Scope);}
  for(unsigned which=0;which<7;++which){
    Validator v(ScopeA);auto key=Key(first,uint32_t(n));
    if(which==0)key.generation=0;if(which==1)key.slot=4;if(which==2)key.bytes++;
    if(which==3)key.frame++;if(which==4)key.map++;if(which==5)key.device++;if(which==6)key.connection.low++;
    CHECK(v.accept(key,packet.data(),n,view)==(which<4?Error::Lease:Error::Scope));Empty(view);
  }
  for(Header next:{Header{ScopeA,99,3,2},Header{ScopeA,100,2,2},Header{ScopeA,100,3,3}}){
    Validator v(ScopeA);CHECK(v.accept(Key(first,uint32_t(n)),packet.data(),n,view)==Error::None);
    size_t nn=0;auto bad=Encoded(next,1,nn);CHECK(v.accept(Key(next,uint32_t(nn)),bad.data(),nn,view)==Error::Order);Empty(view);
    CHECK(v.progress().transfer==1&&v.progress().attempt==2);
  }
  // Encode errors never modify an existing output buffer or publish a length.
  Packet out;out.fill(0xcd);auto unchanged=out;size_t written=88;
  CHECK(Encode(first,payload.data(),1,out.data(),80,written)==Error::Capacity&&written==0);CHECK(out==unchanged);
  CHECK(Encode(first,nullptr,1,out.data(),out.size(),written)==Error::NullBuffer&&written==0);
  CHECK(Encode(first,payload.data(),0,out.data(),out.size(),written)==Error::Size&&written==0);
  CHECK(Encode(first,payload.data(),MaxPayloadBytes+1,out.data(),out.size(),written)==Error::Size&&written==0);
  CHECK(Encode(first,out.data(),1,out.data(),out.size(),written)==Error::Overlap&&written==0);CHECK(out==unchanged);
  for(const Header bad:{Header{},Header{ScopeA,0,1,1},Header{ScopeA,1,0,1},Header{ScopeA,1,1,0}}){
    CHECK(Encode(bad,payload.data(),1,out.data(),out.size(),written)!=Error::None&&written==0);CHECK(out==unchanged);}
  Validator sequential(ScopeA);
  for(uint32_t i=1;i<=10000;++i){
    const Header h{ScopeA,100+i/3,uint64_t(i)*3,i}; // repeated source frame and real ordinal gaps
    const uint32_t bytes=i%32==0?MaxPayloadBytes:(i%3==0?1u:97u);
    for(uint32_t j=0;j<bytes;++j)payload[j]=uint8_t(h.sourceFrame*13+h.attempt*17+j*29);
    auto p=Encoded(h,bytes,n);CHECK(n==80+bytes);
    CHECK(sequential.accept(Key(h,uint32_t(n)),p.data(),n,view)==Error::None);
    CHECK(view.header.sourceFrame==h.sourceFrame&&view.header.attempt==h.attempt&&view.header.transfer==i);
    CHECK(view.bytes==bytes&&!std::memcmp(view.data,payload.data(),bytes));
  }
  CHECK(sequential.progress().transfer==10000&&sequential.progress().attempt==30000);
  Validator bounded(ScopeA,Limits{2,5});
  for(uint64_t i=1;i<=2;++i){Header h{ScopeA,100,i*2,i};auto p=Encoded(h,1,n);CHECK(bounded.accept(Key(h,uint32_t(n)),p.data(),n,view)==Error::None);}
  const Header third{ScopeA,101,5,3};auto thirdPacket=Encoded(third,1,n);
  CHECK(bounded.accept(Key(third,uint32_t(n)),thirdPacket.data(),n,view)==Error::Exhausted);Empty(view);
  Validator attemptBound(ScopeA,Limits{9,1});packet=Encoded(first,1,n);
  CHECK(attemptBound.accept(Key(first,uint32_t(n)),packet.data(),n,view)==Error::Exhausted);Empty(view);
  Validator disabled(ScopeA,Limits{0,5});CHECK(disabled.accept(Key(first,uint32_t(n)),packet.data(),n,view)==Error::Terminal);Empty(view);
  Validator unconfigured({});CHECK(unconfigured.accept(Key(first,uint32_t(n)),packet.data(),n,view)==Error::Terminal);Empty(view);
  const inbox::Scope freshScope{{991,992},4,8};Validator fresh(freshScope);
  const Header freshHeader{freshScope,1,1,1};auto good=Encoded(freshHeader,MaxPayloadBytes,n);
  CHECK(fresh.accept(Key(freshHeader,uint32_t(n)),good.data(),n,view)==Error::None);CHECK(view.header.scope==freshScope);
  // An adapter's exact configured limit is enforced before queue publication.
  auto queue=std::make_unique<inbox::ProducerInbox>(freshScope,InboxLimits);
  CHECK(queue->tryPush(1,payload.data(),MaxPayloadBytes+1).outcome==inbox::Push::Invalid);
  CHECK(queue->tryPush(1,payload.data(),MaxPayloadBytes).outcome==inbox::Push::Published);
  queue->closeProducer();inbox::Sample sample;
  CHECK(queue->tryPop(sample)==inbox::Pop::Item&&sample.ordinal==2&&sample.bytes==MaxPayloadBytes);
  CHECK(queue->tryPop(sample)==inbox::Pop::Finished);
  const auto q=queue->countersAfterJoin();CHECK(q.attempted==2&&q.invalid==1&&q.published==1);
  // Decode success must roundtrip exactly; malformed mutations always clear view.
  uint32_t rng=0x12345678;
  for(unsigned i=0;i<4000;++i){auto mutated=packet;rng=rng*1664525+1013904223;mutated[rng%81]^=uint8_t((rng>>24)|1);
    const auto e=Decode(mutated.data(),81,view);
    if(e==Error::None){size_t nn=0;Packet back{};CHECK(Encode(view.header,view.data,view.bytes,back.data(),back.size(),nn)==Error::None);
      CHECK(nn==81&&!std::memcmp(back.data(),mutated.data(),nn));}
    else Empty(view);
  }
  std::printf("P3 envelope checks=%u bits=%zu positive=10000 PASS\n",checks,sizeof(void*)*8);
}
