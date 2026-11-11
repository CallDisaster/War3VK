#include "../tools/render_host/owned_child.h"
#include "../tools/render_host/wsi_lab_protocol.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace warvk::host;
static_assert(sizeof(void*)==4,"RH3 window owner must be a real Win32 process");
namespace {
constexpr UINT ResizeMessage=WM_APP+73;
struct State {
  lab::Handle done{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
  lab::Handle resized{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
  std::atomic<bool> resizeOk{false};
  DWORD pid=0,exit=STILL_ACTIVE;
  uint64_t creation=0;
  uint32_t exchanges=0,presented=0,gpu=0,resizes=0;
  bool peerVerified=false,ok=false;
};
bool ResizeClient(HWND hwnd,uint32_t width,uint32_t height) {
  RECT r{0,0,LONG(width),LONG(height)};
  if(!AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))return false;
  return SetWindowPos(hwnd,nullptr,0,0,r.right-r.left,r.bottom-r.top,
                      SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE;
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
  if(msg==WM_NCCREATE) {
    const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if(msg==ResizeMessage) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(state) { state->resizeOk.store(ResizeClient(hwnd,400,260),std::memory_order_release); SetEvent(state->resized.get()); }
    return 0;
  }
  if(msg==WM_CLOSE)return 0; // Only the owning test coordinator destroys this window.
  return DefWindowProcW(hwnd,msg,wp,lp);
}
struct Client {
  HANDLE pipe,peer;ipc::Nonce nonce;lab::IoStats stats;
  State& state; HWND window; uint64_t sequence=0,epoch=0;
  uint32_t width=0,height=0;
  void exchange(wsi::Op op,uint32_t frames=0) {
    wsi::Message m;m.op=op;m.nonce=nonce;m.sequence=++sequence;m.windowEpoch=epoch;
    if(epoch)m.hwnd=wsi::EncodeWindow(window);
    m.width=width;m.height=height;m.frames=frames;
    wsi::Write(pipe,peer,m,stats);
    auto reply=wsi::Read(pipe,peer,stats);
    wsi::Require(reply.reply && reply.senderBits==64 && reply.peerBits==32 &&
      reply.nonce==nonce && reply.sequence==m.sequence && reply.op==m.op &&
      reply.windowEpoch==m.windowEpoch && reply.hwnd==m.hwnd &&
      reply.width==width && reply.height==height && reply.frames==frames,"reply-identity");
    const uint32_t next=state.presented+frames;
    const uint32_t resizeCount=state.resizes+(op==wsi::Op::Resize?1u:0u);
    wsi::Require(reply.presented==next && reply.gpuCompleted==next && reply.resized==resizeCount,"reply-counters");
    state.presented=reply.presented;state.gpu=reply.gpuCompleted;state.resizes=reply.resized;
    ++state.exchanges;
  }
};
void Worker(State& state,HWND hwnd,const std::wstring& host,const std::wstring& log) noexcept {
  try {
    const auto nonce=lab::RandomNonce();
    const auto arguments=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+
      std::to_wstring(lab::CreationTime(GetCurrentProcess()));
    lab::OwnedChild child(host,arguments,log);state.pid=child.pid();state.creation=child.creation();
    auto pipe=lab::Connect(nonce,child.process(),child.pid(),lab::Channel::Present3);
    state.peerVerified=true;
    Client client{pipe.get(),child.process(),nonce,{},state,hwnd};
    client.exchange(wsi::Op::Hello);
    client.epoch=1;client.width=320;client.height=240;
    client.exchange(wsi::Op::Create);
    client.exchange(wsi::Op::Frames,wsi::FramesPerBatch);
    wsi::Require(PostMessageW(hwnd,ResizeMessage,0,0)!=FALSE,"resize-post");
    wsi::Require(WaitForSingleObject(state.resized.get(),5000)==WAIT_OBJECT_0 &&
      state.resizeOk.load(std::memory_order_acquire),"resize-timeout");
    client.epoch=2;client.width=400;client.height=260;
    client.exchange(wsi::Op::Resize);
    client.exchange(wsi::Op::Frames,wsi::FramesPerBatch);
    client.exchange(wsi::Op::Close);
    state.exit=child.wait(5000);
    wsi::Require(state.exit==0,"host-exit");
    state.ok=true;
  } catch(...) {state.ok=false;}
  SetEvent(state.done.get());
}
}
int wmain(int argc,wchar_t** argv) {
  HWND hwnd=nullptr;
  try {
    wsi::Require(argc==3,"arguments");
    State state;wsi::Require(bool(state.done)&&bool(state.resized),"events");
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WindowProc;
    wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WarVK_RH3_Owned_Probe";
    wsi::Require(RegisterClassExW(&wc)!=0,"window-class");
    // Deliberately hidden: no foreground activation, global input, desktop switch
    // or player window access. This proves WSI calls, NOT visible composition/FPS.
    hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,
      L"WarVK isolated RH3 probe",WS_OVERLAPPEDWINDOW,0,0,320,240,
      nullptr,nullptr,wc.hInstance,&state);
    wsi::Require(hwnd && ResizeClient(hwnd,320,240),"window-create");
    std::thread worker(Worker,std::ref(state),hwnd,std::wstring(argv[1]),std::wstring(argv[2]));
    bool pumpOk=true;uint32_t dispatched=0;
    const ULONGLONG deadline=GetTickCount64()+30000;
    for (;;) {
      HANDLE done=state.done.get();
      const DWORD wait=MsgWaitForMultipleObjects(1,&done,FALSE,250,QS_ALLINPUT);
      if(wait==WAIT_OBJECT_0)break;
      if(wait==WAIT_FAILED){pumpOk=false;break;}
      MSG msg{};
      while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&msg);DispatchMessageW(&msg);++dispatched;
      }
      if(GetTickCount64()>deadline){pumpOk=false;break;}
    }
    if(!pumpOk) {
      // Do not join a blocked worker after stopping its HWND message pump. The
      // process-local Job closes on exact-process termination; runner reports fail.
      std::printf("{\"bits\":32,\"ok\":false,\"reason\":\"message-pump-timeout\"}\n");
      std::fflush(stdout);ExitProcess(2);
    }
    worker.join();
    const bool parentGpuAbsent=GetModuleHandleW(L"vulkan-1.dll")==nullptr;
    const bool hidden=IsWindowVisible(hwnd)==FALSE;
    const bool destroyed=DestroyWindow(hwnd)!=FALSE;hwnd=nullptr;
    const bool unregistered=UnregisterClassW(wc.lpszClassName,wc.hInstance)!=FALSE;
    const bool ok=state.ok && parentGpuAbsent && hidden && destroyed && unregistered && dispatched>0;
    std::printf("{\"bits\":32,\"ok\":%s,\"childPid\":%lu,\"childCreation\":%llu,"
      "\"childExit\":%lu,\"peerVerified\":%s,\"exchanges\":%u,\"presented\":%u,"
      "\"gpuCompleted\":%u,\"resizes\":%u,\"messagesDispatched\":%u,"
      "\"parentVulkanAbsent\":%s,\"windowVisible\":false,\"windowDestroyed\":%s,"
      "\"globalInputUsed\":false,\"gameTouched\":false}\n",ok?"true":"false",
      static_cast<unsigned long>(state.pid),static_cast<unsigned long long>(state.creation),
      static_cast<unsigned long>(state.exit),state.peerVerified?"true":"false",state.exchanges,
      state.presented,state.gpu,state.resizes,dispatched,parentGpuAbsent?"true":"false",destroyed?"true":"false");
    return ok?0:2;
  } catch(...) {
    if(hwnd)DestroyWindow(hwnd);
    std::printf("{\"bits\":32,\"ok\":false,\"reason\":\"setup\"}\n");return 2;
  }
}
