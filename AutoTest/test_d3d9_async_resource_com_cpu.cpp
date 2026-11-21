// x86 COM entry points and production FIFO against a deterministic in-process peer.
// The peer consumes only at explicit observations or when the owner would wait.
#define WARVK_D3D9_CONNECTION_CPU_TEST 1
#include "../tools/render_host/d3d9_proxy.cpp"
#include <functional>

namespace warvk::host::d3d9proxy {
struct ConnectionCpuTest {
  using Op=d9::Op;
  using Terminal=Connection::Terminal;
  static inline ConnectionCpuTest* active=nullptr;
  static inline unsigned sessions=0;
  static void check(bool ok,int line) {
    if(!ok)throw std::runtime_error("resource CPU check line "+std::to_string(line));
  }
#define CHECK(...) check(bool((__VA_ARGS__)),__LINE__)
  std::shared_ptr<Connection> c;
  std::unique_ptr<d9::ring::PeerChannel> channel;
  std::unique_ptr<d9::ring::Consumer> peer;
  Device* device=nullptr;HWND window=nullptr;
  struct Record {d9::Header h;bool sync;};
  std::vector<Record> records;
  d9::ResourceRegistry host;
  d9::ResourceRef stagedTexture{};
  std::function<void()> atFull;
  HRESULT nextCreate=D3D_OK;
  unsigned hooks=0;
  explicit ConnectionCpuTest(uint32_t features,bool delta=false) {
    active=this;c.reset(new Connection(Connection::PreparedOnly{},ipc::Nonce{GetCurrentProcessId(),++sessions}));
    c->ringRequested=c->drawBatching=c->asyncDrawBatches=c->fixedBatching=c->asyncUploads=c->annotations=true;
    c->fixedDelta=delta;c->ringCapacity=d9::ring::MinCapacity;
    if(features){c->resourceState=std::make_unique<AsyncResourceState>();c->resourceState->features=features;}
    c->beforeReplyWaitForTest=handshake;
    c->beforeRingWaitForTest=[](Connection&,bool space){
      if(space&&active->atFull){std::function<void()> once;once.swap(active->atFull);once();}
      if(active->c->ring->hostState().state==d9::ring::State::Running)active->drain();
    };
    c->setTerminalHook([](void* ctx,const Connection::TerminalReport&){++static_cast<ConnectionCpuTest*>(ctx)->hooks;},this);
    auto* factory=new Factory;factory->pending=c;
    window=CreateWindowExW(0,L"STATIC",L"",0,0,0,64,64,HWND_MESSAGE,nullptr,nullptr,nullptr);
    CHECK(window);SwapChainMode mode;mode.width=mode.height=64;
    device=new Device(factory,window,0,d9::PresentationProfile::Offscreen,mode);device->AddRef();
    CHECK(device->initialize()==D3D_OK&&c->sequence==2&&c->ring);
  }
  ~ConnectionCpuTest(){
    if(device)device->Release();
    c.reset();if(window)DestroyWindow(window);active=nullptr;
  }
  static void handshake(Connection& c) {
    commands::Batch batch;CHECK(c.tryTakeBatch(batch)==commands::Dequeue::Item);
    d9::Header h;CHECK(d9::DecodeHeader(batch.payload.data(),h)&&h.nonce==c.nonce);
    if(h.op==Op::Initialize) {
      d9::Initialization init;CHECK(d9::DecodeInitialization(h,init)&&init.asyncResources==c.resourceFeatures());
    } else {
      CHECK(h.op==Op::AttachRing&&h.sequence==2&&h.args[2]==2);
      auto& f=*active;f.channel=std::make_unique<d9::ring::PeerChannel>(c.nonce,c.ringCapacity);
      auto& ch=*f.channel;
      f.peer=std::make_unique<d9::ring::Consumer>(ch.producerBlock(),ch.data(),ch.consumerBlock(),ch.reply(),ch.capacity(),c.nonce,h.sequence);
    }
    d9::EncodeReply(h,{},c.replyPacket);CHECK(d9::DecodeReply(h,c.replyPacket.data(),c.reply));c.acceptReply(h,batch);
  }
  void drain() {
    using Type=d9::Type;using Read=d9::ring::Consumer::Read;
    d9::Packet packet;d9::Header h;bool sync=false;
    while(true) {
      const auto r=peer->tryRead(packet,h,sync);
      if(r==Read::Empty||r==Read::Closed)return;
      CHECK(r==Read::Item&&h.sequence==3+records.size());records.push_back({h,sync});
      const d9::ResourceRef ref{h.id,h.generation};HRESULT hr=D3D_OK;
      Type type=Type::None;
      if(h.op==Op::CreateTexture2D){type=Type::Texture;hr=nextCreate;nextCreate=D3D_OK;}
      else if(h.op==Op::CreateBuffer)type=h.args[1]?Type::IndexBuffer:Type::VertexBuffer;
      else if(h.op==Op::CreateDeclaration)type=Type::Declaration;
      else if(h.op==Op::CreateShader)type=h.args[0]?Type::PixelShader:Type::VertexShader;
      else if(h.op==Op::CreateQuery)type=Type::EventQuery;
      if(type!=Type::None&&hr==D3D_OK)CHECK(host.commitCreate(ref,type));
      if(h.op==Op::Destroy)CHECK(host.destroy(ref,Type(h.args[0])));
      if(h.op==Op::BeginTextureUpload||h.op==Op::TextureUploadChunk||h.op==Op::CommitTextureUpload)CHECK(host.matches(ref,Type::Texture));
      const auto* data=packet.data()+d9::HeaderBytes;
      if(h.op==Op::AnnotateDraws) {
        d9::DrawAnnotations a;CHECK(d9::DecodeDrawAnnotations(h,data,a)&&a.count==1&&a.draws==1);
        CHECK(a.draw==(c->fixedDelta?Op::DrawFixedDeltaBatch:Op::DrawFixedBatch));
        a.forEach([&](uint16_t i,uint16_t schema,const uint8_t* tag,uint16_t bytes){
          CHECK(!i&&schema==uint16_t(d9::AnnotationSchema::DrawTag)&&bytes==d9::DrawTagBytes&&d9::U64(tag)==7);
        });
      }
      if(d9::IsFixedBatch(h.op)) {
        auto validate=[&](uint32_t i,const uint8_t* p){
          d9::DrawCommand draw;d9::FixedFunctionState state;
          return !i&&d9::DecodeFixedDraw(p,d9::FixedDrawBytes,draw,state)&&draw.resources[2]==stagedTexture
            &&host.matches(draw.resources[2],Type::Texture)&&host.matches(draw.resources[0],Type::VertexBuffer)
            &&host.matches(draw.resources[5],Type::Declaration);
        };
        CHECK(h.args[0]==1);
        if(c->fixedDelta)CHECK(d9::StageFixedDeltaBatch(h,data,validate)==d9::FixedDeltaResult::Valid);
        else CHECK(h.payloadBytes==d9::FixedDrawBytes&&validate(0,data));
      }
      if(h.op==Op::TextureUploadChunk) {
        const uint8_t value=ref==stagedTexture?0x16:0xa7;
        CHECK(h.payloadBytes==16&&std::all_of(data,data+16,[&](uint8_t b){return b==value;}));
      }
      if(h.op==Op::Finish)CHECK(!host.liveCount());
      const uint32_t stamp=uint32_t(h.sequence);
      if(sync)peer->writeReply(h,d9::Reply{uint32_t(hr),0,0,0,0,h.op==Op::Readback?4u:0u},reinterpret_cast<const uint8_t*>(&stamp));
      else CHECK(hr==D3D_OK);
      peer->publishApplied(h.sequence,0,0,0,0);if(h.op==Op::Finish)peer->publishFinished();
      SetEvent(channel->progress());
    }
  }
  HRESULT barrier(){
    uint32_t stamp=0;d9::Header h;h.op=Op::Readback;const auto hr=c->call(h,nullptr,&stamp,4);
    if(hr==D3D_OK)CHECK(stamp==c->sequence);
    return hr;
  }
  IDirect3DTexture9* texture(HRESULT expected=D3D_OK,D3DFORMAT format=D3DFMT_A8R8G8B8) {
    IDirect3DTexture9* t=reinterpret_cast<IDirect3DTexture9*>(uintptr_t(1));
    CHECK(device->CreateTexture(2,2,1,0,format,D3DPOOL_MANAGED,&t,nullptr)==expected);
    CHECK((t!=nullptr)==(expected==D3D_OK));return t;
  }
  void checkFinish(){
    CHECK(device->Release()==0);device=nullptr;
    CHECK(c->healthy()&&c->ringFinish==1&&records.back().h.op==Op::Finish&&!hooks);
  }
  void mixed(bool delta) {
    auto& s=*c->resourceState;auto* t=texture();
    CHECK(records.back().sync&&s.first==1&&s.quota.live[1]==1);
    nextCreate=D3DERR_INVALIDCALL;texture(D3DERR_INVALIDCALL,D3DFMT_L8);
    auto refused=records.back().h;CHECK(!(s.proven&d9::TextureFormatBit(D3DFMT_L8)));
    auto* l=texture(D3D_OK,D3DFMT_L8);CHECK(records.back().sync&&records.back().h.id==refused.id&&records.back().h.generation==refused.generation);
    l->Release();CHECK(barrier()==D3D_OK);
    auto upload=[&](IDirect3DTexture9* target,uint8_t value) {
      D3DLOCKED_RECT lock{};CHECK(target->LockRect(0,&lock,nullptr,0)==D3D_OK&&lock.Pitch==8);
      std::memset(lock.pBits,value,16);CHECK(target->UnlockRect(0)==D3D_OK);
    };
    IDirect3DVertexBuffer9* vb=nullptr;
    CHECK(device->CreateVertexBuffer(36,D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&vb,nullptr)==D3D_OK);
    CHECK(device->SetFVF(D3DFVF_XYZ)==D3D_OK);
    d9::DrawCommand command;command.resources[0]=static_cast<VertexBuffer*>(vb)->ref;
    command.resources[5]=device->decl->ref;command.resources[2]=stagedTexture=static_cast<Texture*>(t)->ref;
    upload(t,0x16);CHECK(barrier()==D3D_OK);
    const auto before=records.size();
    const auto tag=d9::EncodeDrawTag({7,1,0});
    CHECK(c->annotateNextDraw(uint16_t(d9::AnnotationSchema::DrawTag),tag.data(),tag.size())==D3D_OK);
    CHECK(c->submitFixedDraw(d9::EncodeFixedDraw(command,{}))==D3D_OK);
    const auto old=static_cast<Texture*>(t)->ref;t->Release();auto* fresh=texture();
    const auto ref=static_cast<Texture*>(fresh)->ref;CHECK(ref==d9::ResourceRef{old.id,old.generation+1});
    upload(fresh,0xa7);
    CHECK(records.size()==before&&host.matches(old,d9::Type::Texture)&&device->resourceMatches(ref,d9::Type::Texture));
    CHECK(barrier()==D3D_OK);
    const Op order[]={Op::AnnotateDraws,delta?Op::DrawFixedDeltaBatch:Op::DrawFixedBatch,Op::Destroy,Op::CreateTexture2D,
      Op::BeginTextureUpload,Op::TextureUploadChunk,Op::CommitTextureUpload,Op::Readback};
    CHECK(records.size()==before+std::size(order));
    for(size_t i=0;i<std::size(order);++i)CHECK(records[before+i].h.op==order[i]&&records[before+i].sync==(i==7));
    CHECK(s.first==2&&s.unproven==1&&s.creates==1&&s.destroys==2&&s.quota.live[1]==1&&!c->orderedFlushPublished);
    CHECK(c->requestSessionReset(uint32_t(d9::SessionResetReason::MapTransition),1)==D3D_OK);
    d9::Header present;present.op=Op::Present;CHECK(c->call(present)==D3D_OK);
    CHECK(records[records.size()-2].h.op==Op::SessionReset&&device->resourceMatches(ref,d9::Type::Texture));
    fresh->Release();texture()->Release();CHECK(s.creates==2&&s.quota.live[1]==0);vb->Release();
    checkFinish();
  }
  void switches(uint32_t features) {
    auto* first=texture();const auto consumed=records.size();auto* second=texture();
    const bool create=(features&d9::InitializeAsyncCreateTexture)!=0,destroy=(features&d9::InitializeAsyncDestroy)!=0;
    CHECK(records.size()==consumed+!create);
    first->Release();second->Release();CHECK(barrier()==D3D_OK);
    CHECK(records[0].sync&&records[1].sync==!create&&records[2].sync==!destroy&&records[3].sync==!destroy);
    if(const auto* s=c->asyncResources())
      CHECK(s->creates==unsigned(create)&&s->destroys==2u*destroy&&s->disabled==2u*!create);
    checkFinish();
  }
  void shaderQuota() {
    const DWORD vs[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,0x0200001f,0x80000000,0xe00f0000,
      0x02000001,0xe00f0000,0x90e40000,0xffff};
    const DWORD ps[]={0xffff0300,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
      0x02000001,0x800f0800,0xa0e40000,0xffff};
    IDirect3DVertexShader9* v[5]{};IDirect3DPixelShader9* p[5]{};
    for(int i=0;i<4;++i)CHECK(device->CreateVertexShader(vs,&v[i])==D3D_OK&&device->CreatePixelShader(ps,&p[i])==D3D_OK);
    const auto seq=c->sequence;
    CHECK(device->CreateVertexShader(vs,&v[4])==D3DERR_OUTOFVIDEOMEMORY&&!v[4]);
    CHECK(device->CreatePixelShader(ps,&p[4])==D3DERR_OUTOFVIDEOMEMORY&&!p[4]&&seq==c->sequence);
    for(int i=0;i<4;++i){v[i]->Release();p[i]->Release();}
    checkFinish();
  }
  void textureLimits(){
    auto& s=*c->resourceState;auto* first=texture();auto seq=c->sequence;
    auto ref=static_cast<Texture*>(first)->ref;
    // Seed only the count for the last slot; the exhaustive quota test fills all 65,472.
    auto& live=s.quota.live[d9::ResourceQuota::pool(d9::Type::Texture)];
    live=d9::TextureSlots;
    texture(D3DERR_OUTOFVIDEOMEMORY);CHECK(c->sequence==seq&&!device->resources.reservedCount());
    nextCreate=D3DERR_INVALIDCALL;texture(D3DERR_INVALIDCALL,D3DFMT_L8);
    CHECK(c->sequence==++seq&&!device->resources.reservedCount());
    IDirect3DTexture9* bad=nullptr;
    CHECK(device->CreateTexture(0,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&bad,nullptr)==D3DERR_INVALIDCALL&&!bad);
    CHECK(c->sequence==seq);
    auto snapshot=device->resources;
    for(uint32_t i=2;i<=d9::MaxResources;++i){
      const auto q=device->resources.reserve(d9::Type::EventQuery);
      CHECK(q.id==i&&device->resources.commitReservation(q,d9::Type::EventQuery));
    }
    texture(E_OUTOFMEMORY);CHECK(c->sequence==seq&&s.outOfMemory==1&&s.invalid==1&&s.outOfVideoMemory==1);
    device->resources=std::move(snapshot);live=1;first->Release();
    auto* t=texture();CHECK(static_cast<Texture*>(t)->ref==d9::ResourceRef{ref.id,ref.generation+1});
    t->Release();checkFinish();
  }
  void quotaAndReset() {
    auto& s=*c->resourceState;
    IDirect3DVertexBuffer9* vb[7]{};IDirect3DIndexBuffer9* ib=nullptr;
    auto make=[&](IDirect3DVertexBuffer9** out){return device->CreateVertexBuffer(16,D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,out,nullptr);};
    for(auto& v:vb)CHECK(make(&v)==D3D_OK);
    CHECK(device->CreateIndexBuffer(8,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib,nullptr)==D3D_OK);
    const auto seq=c->sequence;IDirect3DVertexBuffer9* extra=nullptr;
    CHECK(make(&extra)==D3DERR_OUTOFVIDEOMEMORY&&!extra&&c->sequence==seq&&!device->resources.reservedCount());
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D16;pp.Windowed=TRUE;
    CHECK(device->Reset(&pp)==D3DERR_INVALIDCALL&&device->TestCooperativeLevel()==D3DERR_DEVICENOTRESET);
    ib->Release();for(auto* v:vb)v->Release();
    CHECK(s.destroys==8&&s.quota.live[0]==0&&device->Reset(&pp)==D3D_OK&&make(&extra)==D3D_OK);extra->Release();
    // SetFVF creates the fifth declaration through normal eviction and retry.
    for(DWORD f:{D3DFVF_XYZ,D3DFVF_XYZ|D3DFVF_NORMAL,D3DFVF_XYZ|D3DFVF_DIFFUSE,
        D3DFVF_XYZ|D3DFVF_TEX1,D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1})CHECK(device->SetFVF(f)==D3D_OK);
    CHECK(device->fvfEvictions==3&&s.quota.live[4]==2&&s.outOfVideoMemory==2);
    checkFinish();CHECK(s.quota.live==d9::ResourceQuota{}.live);
  }
  // Fill to less than the next header using complete records, then let exactly
  // the first blocked publication see the local registry before any peer progress.
  void fill(){
    d9::Header h;h.op=Op::Clear;const auto n=c->ringCapacity/d9::ring::RecordBytes(d9::HeaderBytes);
    CHECK(barrier()==D3D_OK);
    for(unsigned i=0;i<n;++i)CHECK(c->enqueueClear(h)==D3D_OK);
  }
  void full() {
    auto* t=texture();auto ref=static_cast<Texture*>(t)->ref;
    fill();auto seq=c->sequence;
    atFull=[&]{CHECK(c->sequence==seq&&device->resources.matches(ref,d9::Type::Texture)&&!c->resourceState->destroys);};
    t->Release();CHECK(!atFull&&c->sequence==seq+1&&!device->resources.matches(ref,d9::Type::Texture));
    fill();seq=c->sequence;
    atFull=[&]{CHECK(c->sequence==seq&&device->resources.reservedCount()==1&&!device->resources.liveCount()&&!c->resourceState->creates);};
    auto* fresh=texture();CHECK(!atFull&&c->sequence==seq+1&&c->backpressureCalls==2);
    CHECK(static_cast<Texture*>(fresh)->ref==d9::ResourceRef{ref.id,ref.generation+1});
    fresh->Release();checkFinish();
  }
  void uncertainReservation(bool deviceLost) {
    // An older async record fails while a proven texture waits for ring space.
    // Its own record did not get a sequence; the new reservation stays quarantined.
    auto* first=texture();auto* extra=texture();extra->Release();
    CHECK(barrier()==D3D_OK);
    first->Release();const auto refused=c->sequence;
    d9::Header clear;clear.op=Op::Clear;
    const auto n=c->ringCapacity/d9::ring::RecordBytes(d9::HeaderBytes);
    for(unsigned i=1;i<n;++i)CHECK(c->enqueueClear(clear)==D3D_OK);
    const auto seq=c->sequence;atFull=[&]{
      CHECK(device->resources.reservedCount()==1&&peer->expected()==refused);
      d9::Packet packet;d9::Header h;bool sync=true;
      CHECK(peer->tryRead(packet,h,sync)==d9::ring::Consumer::Read::Item&&!sync&&h.op==Op::Destroy);
      peer->publishFault(deviceLost?d9::ring::Fault::DeviceLost:d9::ring::Fault::AsyncRefused,
        uint32_t(deviceLost?D3DERR_DEVICELOST:D3DERR_INVALIDCALL),h.sequence);
    };
    texture(D3DERR_DEVICELOST);
    CHECK(c->sequence==seq&&device->resources.reservedCount()==1&&c->resourceState->creates==1&&hooks==1);
    Connection::TerminalReport r;CHECK(c->terminalReport(r)&&r.kind==(deviceLost?Terminal::HostDeviceLost:Terminal::HostRejected));
    CHECK(c->deviceLost.load()==deviceLost&&c->failed.load()!=deviceLost);
  }
  void uncertainSync() {
    nextCreate=S_FALSE;texture(D3DERR_DEVICELOST);
    CHECK(!c->healthy()&&device->resources.reservedCount()==1&&!c->resourceState->proven&&hooks==1);
  }
  void foreign(){
    auto* t=texture();const auto seq=c->sequence;
    std::thread other([&]{t->Release();});other.join();
    CHECK(!c->healthy()&&hooks==1&&c->sequence==seq&&device->resources.liveCount()==1);
    Connection::TerminalReport r;CHECK(c->terminalReport(r)&&r.kind==Terminal::LocalContract);
  }
  void terminal(bool destroying,Terminal kind) {
    auto* first=texture();IDirect3DTexture9* second=nullptr;
    if(destroying)first->Release();else second=texture();
    const auto seq=c->sequence;CHECK(peer->expected()==seq);
    const auto snapshot=device->resources;
    uint32_t result=0;
    if(kind==Terminal::HostExited) {
      // Exercise the real watcher with an already-signalled fake process handle.
      lab::Handle exited(CreateEventW(nullptr,TRUE,TRUE,nullptr));
      try{c->watchRing(exited.get());}catch(const std::exception&){}
    } else {
      result=uint32_t(kind==Terminal::HostRejected?D3DERR_OUTOFVIDEOMEMORY:D3DERR_DEVICELOST);
      peer->publishFault(kind==Terminal::HostRejected?d9::ring::Fault::AsyncRefused:d9::ring::Fault::DeviceLost,result,seq);
    }
    CHECK(barrier()==D3DERR_DEVICELOST);Connection::TerminalReport r;
    CHECK(c->terminalReport(r)&&r.kind==kind&&r.result==result&&hooks==1);
    CHECK(!device->reserveResource(d9::Type::Texture).id);
    if(second){second->Release();first->Release();}
    CHECK(c->sequence==seq&&device->resources==snapshot&&barrier()==D3DERR_DEVICELOST&&hooks==1);
  }
#undef CHECK
};
}
int wmain(){
  using F=warvk::host::d3d9proxy::ConnectionCpuTest;
  constexpr auto flags=warvk::host::d9::AsyncResourceFeatures;
  try{
    for(uint32_t bits:{0u,8u,16u})F(bits).switches(bits);
    F(flags).shaderQuota();F(flags).textureLimits();
    for(bool delta:{false,true})F(flags,delta).mixed(delta);
    F(flags).quotaAndReset();F(flags).full();F(flags).foreign();F(flags).uncertainSync();
    for(bool lost:{false,true})F(flags).uncertainReservation(lost);
    for(bool destroy:{false,true})for(auto terminal:{F::Terminal::HostRejected,F::Terminal::HostDeviceLost,F::Terminal::HostExited})
      F(flags).terminal(destroy,terminal);
    std::puts("x86 COM async resource FIFO, full, format, quota, Reset, FVF and terminal cases passed");return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
