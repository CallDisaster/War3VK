// Same CPU-only Connection seam and link inputs as test_d3d9_connection_async_cpu.
// Drain the real FIFO at deterministic points; the peer supplies CPU replies only.
#define WARVK_D3D9_CONNECTION_CPU_TEST 1
#include "../tools/render_host/d3d9_connection.h"

namespace warvk::host::d3d9proxy {
struct ConnectionCpuTest {
  static void check(bool value,unsigned line) {
    if(!value)throw std::runtime_error("fixed delta connection check line "+std::to_string(line));
  }
#define CHECK(v) check(bool(v),__LINE__)
  static d9::FixedDrawPacket record(unsigned index) {
    d9::DrawCommand command;command.resources[0]={3,index+1};command.resources[5]={7,1};
    command.state.streamOffset=index*16;
    d9::FixedFunctionState ff;ff.transforms[0]._41=float(index);return d9::EncodeFixedDraw(command,ff);
  }
  static void drain(Connection& c) {
    commands::Batch batch;
    while(c.tryTakeBatch(batch)==commands::Dequeue::Item) {
      d9::Header h;CHECK(d9::DecodeHeader(batch.payload.data(),h));
      const auto* data=batch.payload.data()+d9::HeaderBytes;
      const auto fixed=c.fixedDelta?d9::Op::DrawFixedDeltaBatch:d9::Op::DrawFixedBatch;
      switch(h.sequence) {
        case 1:CHECK(h.op==d9::Op::Initialize&&h.args[6]==(d9::InitializeAnnotations|(c.fixedDelta?d9::InitializeFixedDelta:0)));break;
        case 2:case 10:{
          const auto count=h.sequence==2?2u:1u;
          CHECK(h.op==d9::Op::DrawBatch&&h.args[0]==count&&h.payloadBytes==count*d9::DrawBytes);
          for(uint32_t i=0;i<count;++i) {
            d9::DrawPacket draw;draw.fill(h.sequence==2?uint8_t(i):2);
            CHECK(!std::memcmp(draw.data(),data+i*d9::DrawBytes,draw.size()));
          }
          break;
        }
        case 3:case 5:case 8:{
          const auto count=h.sequence==3?20u:h.sequence==5?1u:2u;
          d9::DrawAnnotations a;CHECK(d9::DecodeDrawAnnotations(h,data,a)&&a.draw==fixed&&a.draws==count&&a.count==1);
          a.forEach([&](uint16_t i,uint16_t schema,const uint8_t* tag,uint16_t bytes) {
            CHECK(uint32_t(i)+1==count&&schema==uint16_t(d9::AnnotationSchema::DrawTag)&&bytes==d9::DrawTagBytes);
            CHECK(d9::U64(tag)==(h.sequence==3?19u:h.sequence==5?20u:22u));
          });
          break;
        }
        case 4:case 6:case 9:{
          const auto count=h.sequence==4?20u:h.sequence==6?1u:2u;
          const auto first=h.sequence==4?0u:h.sequence==6?20u:21u;
          CHECK(h.op==fixed&&h.args[0]==count&&h.args[1]==uint32_t(c.fixedDeltaVerify));
          auto compare=[&](uint32_t i,const uint8_t* bytes) {
            const auto want=record(first+i);return !std::memcmp(want.data(),bytes,want.size());
          };
          if(c.fixedDelta)CHECK(d9::StageFixedDeltaBatch(h,data,compare)==d9::FixedDeltaResult::Valid);
          else {
            CHECK(h.payloadBytes==count*d9::FixedDrawBytes);
            for(uint32_t i=0;i<count;++i)CHECK(compare(i,data+i*d9::FixedDrawBytes));
          }
          break;
        }
        case 7:{d9::Resize size;CHECK(d9::DecodeReset(h,false,size)&&size.width==80&&size.height==64);break;}
        case 11:CHECK(h.op==d9::Op::Finish);break;
        default:CHECK(false);
      }
      d9::EncodeReply(h,d9::Reply{},c.replyPacket);
      CHECK(d9::DecodeReply(h,c.replyPacket.data(),c.reply));c.acceptReply(h,batch);
    }
  }
  static void run(bool delta,bool verify,bool async) {
    std::unique_ptr<Connection> c(new Connection(Connection::PreparedOnly{},ipc::Nonce{3,7}));
    c->drawBatching=c->fixedBatching=c->annotations=true;c->asyncDrawBatches=async;
    c->fixedDelta=delta;c->fixedDeltaVerify=verify;c->beforeReplyWaitForTest=drain;
    d9::Header init;init.args[0]=init.args[1]=64;init.args[6]=d9::InitializeAnnotations;
    CHECK(c->call(init)==D3D_OK);
    d9::DrawPacket draw{};
    for(unsigned i=0;i<2;++i){draw.fill(uint8_t(i));CHECK(c->submitDraw(draw)==D3D_OK);}
    for(unsigned i=0;i<23;++i) {
      if(i==19||i==20||i==22) {
        const auto tag=d9::EncodeDrawTag({i,1,0});
        CHECK(c->annotateNextDraw(uint16_t(d9::AnnotationSchema::DrawTag),tag.data(),uint16_t(tag.size()))==D3D_OK);
      }
      CHECK(c->submitFixedDraw(record(i))==D3D_OK);drain(*c);
      if(i==20){d9::Header reset;reset.op=d9::Op::Reset;reset.args[0]=80;reset.args[1]=64;CHECK(c->call(reset)==D3D_OK);}
    }
    draw.fill(2);CHECK(c->submitDraw(draw)==D3D_OK);
    d9::Header finish;finish.op=d9::Op::Finish;CHECK(c->call(finish)==D3D_OK);
    CHECK(c->sequence==11&&c->done.load()==11&&c->healthy()&&c->fixedDrawsAdmitted==23);
    CHECK(c->fixedBatchesRemoteSucceeded==3&&c->fixedBatchesFlushed==3&&c->drawBatchesRemoteSucceeded==2);
    for(unsigned n=1;n<=d9::MaxFixedDrawBatch;++n)CHECK(c->deltaHistogram[n-1]==unsigned(delta&&(n==1||n==2||n==20)));
    CHECK(delta?(c->deltaSentBytes>0&&c->deltaSentBytes<23*d9::FixedDrawBytes):!c->deltaSentBytes);
  }
#undef CHECK
};
}

int wmain() {
  try {
    using warvk::host::d3d9proxy::ConnectionCpuTest;
    for(bool async:{false,true}) {
      ConnectionCpuTest::run(false,false,async);
      ConnectionCpuTest::run(true,false,async);
      ConnectionCpuTest::run(true,true,async);
    }
    std::puts("fixed delta Connection CPU mixed/reset cases=6");return 0;
  } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
