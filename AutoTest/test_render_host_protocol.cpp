#include "../tools/render_host/protocol.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace warvk::host::ipc;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
static constexpr Nonce N{0x0807060504030201ull,0x1817161514131211ull};
using Bytes=std::vector<uint8_t>;
Bytes packet(Header h, const Bytes& payload={}) {
  h.payloadBytes=uint32_t(payload.size());Bytes bytes(HeaderBytes+payload.size());size_t n=0;
  CHECK(Encode(h,payload.data(),payload.size(),bytes.data(),bytes.size(),n)==Error::None);
  CHECK(n==bytes.size());return bytes;
}
Header header(Op op, uint64_t seq, uint64_t map=0,uint64_t device=0,uint64_t frame=0) {
  Header h;h.op=op;h.nonce=N;h.sequence=seq;h.mapEpoch=map;h.deviceEpoch=device;h.frameId=frame;return h;
}
Bytes hello(uint32_t sender=32,uint32_t peer=64,uint64_t caps=1) {
  auto p=EncodeHello({sender,peer,caps,caps});return packet(header(Op::Hello,1),Bytes(p.begin(),p.end()));
}
std::string hex(const Bytes& bytes){
  std::string s;static constexpr char digits[]="0123456789abcdef";
  for(auto b:bytes){s+=digits[b>>4];s+=digits[b&15];}return s;
}
void accepted(ServerSession& s,const Bytes& b,Reply& r){
  auto count=s.accepted();CHECK(s.receive(b.data(),b.size(),r)==Error::None);
  CHECK(s.accepted()==count+1);CHECK(r.header.flags==1);
  Bytes response(HeaderBytes+r.header.payloadBytes);size_t n=0;
  CHECK(Encode(r.header,r.payload(),r.header.payloadBytes,response.data(),response.size(),n)==Error::None);
  MessageView v;CHECK(Decode(response.data(),n,v)==Error::None);CHECK(v.header.sequence==r.header.sequence);
}
void rejected(ServerSession& s,const Bytes& b,Error want){
  const auto count=s.accepted(),map=s.mapEpoch(),device=s.deviceEpoch(),frame=s.frame();Reply r;
  CHECK(s.receive(b.data(),b.size(),r)==want);CHECK(s.state()==State::Fault);
  CHECK(s.accepted()==count);CHECK(s.mapEpoch()==map);CHECK(s.deviceEpoch()==device);CHECK(s.frame()==frame);
  auto valid=hello();CHECK(s.receive(valid.data(),valid.size(),r)==Error::Terminal);
}
void ready(ServerSession& s){Reply r;accepted(s,hello(),r);CHECK(s.state()==State::Ready);}
void active(ServerSession& s){ready(s);Reply r;accepted(s,packet(header(Op::BeginEpoch,2,3,7)),r);CHECK(s.state()==State::Active);}
int main(int argc,char** argv){try{
  const auto golden=hello();
  const std::string expected=
    "57564b300000010060000100000000001800000000000000"
    "01020304050607081112131415161718" "0100000000000000"
    "0000000000000000" "0000000000000000" "0000000000000000"
    "0000000000000000" "0000000000000000" "0000000000000000"
    "200000004000000001000000000000000100000000000000";
  CHECK(hex(golden)==expected);
  if(argc==2&&std::string(argv[1])=="--golden"){std::cout<<hex(golden)<<"\n";return 0;}
  CHECK(argc==1);
  MessageView view;CHECK(Decode(nullptr,0,view)==Error::NullBuffer);
  for(size_t n=0;n<golden.size();++n){
    CHECK(Decode(golden.data(),n,view)!=(Error::None));CHECK(!view.payload);
  }
  auto extra=golden;extra.push_back(0);CHECK(Decode(extra.data(),extra.size(),view)==Error::PacketSize);
  Header h;CHECK(DecodeHeader(golden.data(),HeaderBytes,h)==Error::None);CHECK(h.payloadBytes==24);
  for(auto field:std::array<std::pair<unsigned,Error>,13>{{{0,Error::Magic},{4,Error::Version},
      {6,Error::Version},{8,Error::HeaderSize},{10,Error::Opcode},{12,Error::Flags},
      {20,Error::Reserved},{21,Error::Reserved},{23,Error::Reserved},{88,Error::Reserved},
      {91,Error::Reserved},{92,Error::Reserved},{95,Error::Reserved}}}){
    auto bad=golden;bad[field.first]=0xff;ServerSession s(N,64,32);rejected(s,bad,field.second);
  }
  auto tooLong=golden;WriteLe(tooLong.data()+16,MaxPayloadBytes+1,4);
  {ServerSession s(N,64,32);rejected(s,tooLong,Error::PayloadLimit);}
  auto zero=golden;std::fill(zero.begin()+24,zero.begin()+40,0);
  {ServerSession s(N,64,32);rejected(s,zero,Error::EmptyNonce);}
  for(auto field:std::array<std::pair<unsigned,Error>,11>{{{24,Error::Session},{32,Error::Session},
      {40,Error::Sequence},{48,Error::Scope},{56,Error::Scope},{64,Error::ResourceUnsupported},
      {72,Error::ResourceUnsupported},{80,Error::Scope},{96,Error::Bits},{100,Error::Bits},
      {104,Error::Capabilities}}}){
    auto bad=golden;bad[field.first]^=2;ServerSession s(N,64,32);rejected(s,bad,field.second);
  }
  for(uint64_t seq:{0ull,2ull,UINT64_MAX}){
    auto bad=golden;WriteLe(bad.data()+40,seq,8);ServerSession s(N,64,32);
    rejected(s,bad,seq==UINT64_MAX?Error::SequenceExhausted:Error::Sequence);
  }
  for(uint32_t bits:{0u,16u,128u}){ServerSession s(N,bits,32);CHECK(s.state()==State::Fault);}
  {ServerSession s({},64,32);CHECK(s.state()==State::Fault);}
  for(auto bits:{std::pair{32u,32u},std::pair{64u,64u},std::pair{32u,64u},std::pair{64u,32u}}){
    ServerSession s(N,bits.first,bits.second);Reply r;accepted(s,hello(bits.second,bits.first),r);
    CHECK(ReadLe(r.hello.data(),4)==bits.first);CHECK(ReadLe(r.hello.data()+4,4)==bits.second);
  }
  {ServerSession s(N,64,32);rejected(s,hello(32,64,3),Error::Capabilities);}
  {auto bad=golden;WriteLe(bad.data()+112,3,8);ServerSession s(N,64,32);rejected(s,bad,Error::Capabilities);}
  {auto bad=golden;WriteLe(bad.data()+12,1,4);ServerSession s(N,64,32);rejected(s,bad,Error::Flags);}
  {ServerSession s(N,64,32);rejected(s,packet(header(Op::Hello,1)),Error::Payload);}
  for(auto op:{Op::BeginEpoch,Op::SampleEcho,Op::EndEpoch,Op::Close}){
    ServerSession s(N,64,32);rejected(s,packet(header(op,1)),Error::State);
  }
  for(auto op:{Op::Hello,Op::SampleEcho,Op::EndEpoch}){
    ServerSession s(N,64,32);ready(s);rejected(s,packet(header(op,2)),Error::State);
  }
  for(auto scope:std::array<std::pair<uint64_t,uint64_t>,3>{{{0,7},{3,0},{0,0}}}){
    ServerSession s(N,64,32);ready(s);rejected(s,packet(header(Op::BeginEpoch,2,scope.first,scope.second)),Error::Scope);
  }
  for(auto op:{Op::Hello,Op::BeginEpoch,Op::Close}){
    ServerSession s(N,64,32);active(s);rejected(s,packet(header(op,3,3,7)),Error::State);
  }
  for(auto op:{Op::SampleEcho,Op::EndEpoch})for(auto scope:std::array<std::pair<uint64_t,uint64_t>,4>{{{2,7},{4,7},{3,6},{3,8}}}){
    ServerSession s(N,64,32);active(s);rejected(s,packet(header(op,3,scope.first,scope.second,1)),Error::Scope);
  }
  for(uint64_t frame:{0ull,2ull,UINT64_MAX}){
    ServerSession s(N,64,32);active(s);rejected(s,packet(header(Op::SampleEcho,3,3,7,frame)),Error::Frame);
  }
  {ServerSession s(N,64,32);active(s);rejected(s,packet(header(Op::EndEpoch,3,3,7,1)),Error::Frame);}
  {ServerSession s(N,64,32);active(s);rejected(s,packet(header(Op::EndEpoch,3,3,7),Bytes{1}),Error::Payload);}
  for(auto scope:std::array<std::pair<uint64_t,uint64_t>,3>{{{3,7},{2,8},{4,6}}}){
    ServerSession s(N,64,32);active(s);Reply r;accepted(s,packet(header(Op::EndEpoch,3,3,7)),r);
    rejected(s,packet(header(Op::BeginEpoch,4,scope.first,scope.second)),Error::Scope);
  }
  // Positive lifecycle and actual echoed payload at every permitted size.
  ServerSession s(N,64,32);ready(s);uint64_t seq=2;
  for(auto scope:std::array<std::pair<uint64_t,uint64_t>,3>{{{3,7},{3,8},{4,8}}}){
    Reply r;accepted(s,packet(header(Op::BeginEpoch,seq++,scope.first,scope.second)),r);uint64_t frame=1;
    for(size_t bytes:{size_t(0),size_t(1),size_t(4096),size_t(MaxPayloadBytes)}){
      Bytes payload(bytes);for(size_t i=0;i<bytes;++i)payload[i]=uint8_t(i*73+frame);
      auto request=packet(header(Op::SampleEcho,seq++,scope.first,scope.second,frame++),payload);
      accepted(s,request,r);CHECK(r.header.payloadBytes==bytes);
      CHECK(std::equal(payload.begin(),payload.end(),r.payload()));
      size_t n=0;CHECK(Encode(r.header,r.payload(),bytes,request.data(),request.size(),n)==Error::None);
      CHECK(Decode(request.data(),n,view)==Error::None);CHECK(std::equal(payload.begin(),payload.end(),view.payload));
    }
    accepted(s,packet(header(Op::EndEpoch,seq++,scope.first,scope.second)),r);CHECK(s.state()==State::Ready);
  }
  Reply r;accepted(s,packet(header(Op::Close,seq++)),r);CHECK(s.state()==State::Closing);
  CHECK(s.closeReplySent(seq-1)==Error::None);CHECK(s.state()==State::Closed);
  CHECK(s.closeReplySent(seq-1)==Error::Terminal);CHECK(s.state()==State::Closed);
  CHECK(s.receive(golden.data(),golden.size(),r)==Error::Terminal);s.transportFailed();CHECK(s.state()==State::Closed);
  {ServerSession closed(N,64,32);ready(closed);closed.transportFailed();CHECK(closed.state()==State::Fault);
   CHECK(closed.receive(golden.data(),golden.size(),r)==Error::Terminal);}
  // Fresh connection recovery, never implicit revival of a faulted object.
  {ServerSession fresh(N,64,32);ready(fresh);CHECK(fresh.accepted()==1);}
  {ServerSession f(N,64,32);ready(f);CHECK(f.closeReplySent(1)==Error::State);CHECK(f.state()==State::Fault);}
  {ServerSession f(N,64,32);ready(f);accepted(f,packet(header(Op::Close,2)),r);
   CHECK(f.closeReplySent(1)==Error::Sequence);CHECK(f.state()==State::Fault);CHECK(f.accepted()==2);}
  {ServerSession f(N,64,32);ready(f);accepted(f,packet(header(Op::Close,2)),r);
   f.transportFailed();CHECK(f.state()==State::Fault);CHECK(f.closeReplySent(2)==Error::Terminal);}
  {ServerSession f(N,64,32);ready(f);accepted(f,packet(header(Op::Close,2)),r);
   rejected(f,packet(header(Op::Close,3)),Error::State);}
  std::array<uint8_t,128> target;target.fill(0xa5);size_t written=123;
  auto badHeader=header(Op::Hello,1);badHeader.flags=2;
  CHECK(Encode(badHeader,nullptr,0,target.data(),target.size(),written)==Error::Flags);
  CHECK(written==0);CHECK(std::all_of(target.begin(),target.end(),[](auto b){return b==0xa5;}));
  CHECK(Encode(header(Op::Hello,1),nullptr,1,target.data(),target.size(),written)==Error::NullBuffer);
  CHECK(Encode(header(Op::Hello,1),target.data(),SIZE_MAX,target.data(),target.size(),written)==Error::PayloadLimit);
  CHECK(Encode(header(Op::Hello,1),nullptr,0,target.data(),95,written)==Error::PacketSize);
  // Deterministic fuzz of framing: valid packets remain allowed, invalid never crash.
  uint32_t rng=0x981237;
  for(unsigned i=0;i<20000;++i){auto fuzz=golden;rng=rng*1664525+1013904223;
    fuzz[rng%fuzz.size()]^=uint8_t((rng>>24)|1);ServerSession f(N,64,32);Reply reply;
    auto e=f.receive(fuzz.data(),fuzz.size(),reply);CHECK(e==Error::None||f.state()==State::Fault);
    if(e!=Error::None)CHECK(f.accepted()==0);
  }
  std::cout<<"RH0 codec/session checks="<<checks<<" bits="<<sizeof(void*)*8<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
