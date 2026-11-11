#include "../tools/render_host/gpu_wire.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace warvk::host;
namespace {
unsigned checks=0;
void Check(bool b,unsigned line) { ++checks;if(!b){std::fprintf(stderr,"line=%u\n",line);std::exit(1);} }
#define CHECK(x) Check(bool(x),__LINE__)
constexpr ipc::Nonce nonce{0x0807060504030201ull,0x1817161514131211ull};
struct Fixture {
  gpuwire::Session session{nonce};gpuwire::Header h;gpuwire::Packet packet{};size_t size=0;
  std::array<uint8_t,96> payload{};gpuwire::View view;
  Fixture(){h.nonce=nonce;}
  gpuwire::Error send(){CHECK(gpuwire::Encode(h,payload.data(),packet.data(),packet.size(),size)==gpuwire::Error::None);
    return session.receive(packet.data(),size,view);}
  void hello(){h.op=gpuwire::Op::Hello;h.payloadBytes=24;
    auto p=ipc::EncodeHello({32,64,gpuwire::Capability,gpuwire::Capability});std::memcpy(payload.data(),p.data(),p.size());
    CHECK(send()==gpuwire::Error::None);++h.sequence;}
  void begin(uint64_t epoch=1){h.op=gpuwire::Op::Begin;h.mapEpoch=epoch;h.deviceEpoch=1;
    h.resourceId=1;h.generation=epoch;h.frame=0;h.payloadBytes=8;
    ipc::WriteLe(payload.data(),64,4);ipc::WriteLe(payload.data()+4,64,4);
    CHECK(send()==gpuwire::Error::None);++h.sequence;}
  void render(uint64_t frame){h.op=gpuwire::Op::Render;h.frame=frame;h.payloadBytes=4;
    ipc::WriteLe(payload.data(),frame%4,4);CHECK(send()==gpuwire::Error::None);++h.sequence;}
  void release(){h.op=gpuwire::Op::Release;h.payloadBytes=0;CHECK(send()==gpuwire::Error::None);++h.sequence;}
};
}
int main(int argc,char** argv){
  if(argc==2&&!std::strcmp(argv[1],"--golden")){
    Fixture f;f.hello();for(size_t i=0;i<f.size;++i)std::printf("%02x",f.packet[i]);std::puts("");return 0;
  }
  Fixture original;original.hello();
  for(size_t n=0;n<original.size;++n){gpuwire::View v;CHECK(gpuwire::Decode(original.packet.data(),n,v)!=gpuwire::Error::None);CHECK(v.payload==nullptr);}
  for(unsigned offset:{0u,4u,6u,8u,10u,20u,88u,95u}){
    auto p=original.packet;p[offset]^=0xff;gpuwire::View v;
    CHECK(gpuwire::Decode(p.data(),original.size,v)!=gpuwire::Error::None);CHECK(v.payload==nullptr);
  }
  for(unsigned i=0;i<20;++i){
    Fixture f;f.hello();f.begin();
    switch(i){
      case 0:f.h.sequence=1;break;
      case 1:f.h.nonce.high^=1;break;
      case 2:f.h.flags=1;break;
      case 3:f.h.mapEpoch=2;break;
      case 4:f.h.deviceEpoch=2;break;
      case 5:f.h.resourceId=2;break;
      case 6:f.h.generation=2;break;
      case 7:f.h.frame=2;break;
      case 8:f.h.frame=UINT64_MAX;break;
      default:break;
    }
    if(i<=8){f.h.op=gpuwire::Op::Render;f.h.payloadBytes=4;if(i<7)f.h.frame=1;ipc::WriteLe(f.payload.data(),0,4);}
    if(i==9){f.h.op=gpuwire::Op::Release;f.h.payloadBytes=0;f.h.frame=0;}
    if(i==10){f.h.op=gpuwire::Op::Render;f.h.payloadBytes=4;f.h.frame=1;ipc::WriteLe(f.payload.data(),4,4);}
    if(i==11){f.h.op=gpuwire::Op::Close;f.h.payloadBytes=0;}
    if(i==12){f.h.op=gpuwire::Op::Begin;}
    if(i>=13){
      f.render(1);
      if(i==13){f.h.op=gpuwire::Op::End;f.h.frame=0;f.h.payloadBytes=0;}
      if(i>=14){CHECK(f.session.submitted(1)==gpuwire::Error::None);
        if(i==14){f.h.op=gpuwire::Op::Render;f.h.frame=2;}
        if(i==15){f.h.op=gpuwire::Op::Release;f.h.frame=2;f.h.payloadBytes=0;}
        if(i>=16){f.release();
          if(i==16){f.h.op=gpuwire::Op::End;f.h.frame=0;}
          if(i==17){CHECK(f.session.consumerFinished(2)!=gpuwire::Error::None);}
          if(i>=18){CHECK(f.session.consumerFinished(1)==gpuwire::Error::None);
            if(i==18){f.h.op=gpuwire::Op::Render;f.h.frame=1;f.h.payloadBytes=4;}
            if(i==19){f.h.op=gpuwire::Op::Release;f.h.frame=1;}
          }
        }
      }
    }
    auto e=f.send();CHECK(e!=gpuwire::Error::None);CHECK(f.view.payload==nullptr);
    CHECK(f.session.state()==gpuwire::State::Fault);CHECK(f.send()==gpuwire::Error::Terminal);
  }
  Fixture f;f.hello();
  for(uint64_t epoch=1;epoch<=4;++epoch){f.begin(epoch);
    for(uint64_t frame=1;frame<=250;++frame){f.render(frame);
      CHECK(f.session.state()==gpuwire::State::RenderPending);
      CHECK(f.session.submitted(frame)==gpuwire::Error::None);f.release();
      CHECK(f.session.state()==gpuwire::State::ReleasePending);
      CHECK(f.session.consumerFinished(frame)==gpuwire::Error::None);
      CHECK(f.session.state()==gpuwire::State::Active);
    }
    f.h.op=gpuwire::Op::End;f.h.frame=0;f.h.payloadBytes=0;CHECK(f.send()==gpuwire::Error::None);++f.h.sequence;
  }
  f.h.op=gpuwire::Op::Close;f.h.mapEpoch=f.h.deviceEpoch=f.h.resourceId=f.h.generation=f.h.frame=0;
  CHECK(f.send()==gpuwire::Error::None);CHECK(f.session.state()==gpuwire::State::Closing);
  CHECK(f.session.closeSent(f.h.sequence)==gpuwire::Error::None);
  CHECK(f.send()==gpuwire::Error::Terminal);
  // New process connection, not resurrection of a faulted session.
  Fixture recovered;recovered.hello();recovered.begin();CHECK(recovered.session.state()==gpuwire::State::Active);
  std::printf("RH2 codec/session checks=%u bits=%zu PASS\n",checks,sizeof(void*)*8);
}
