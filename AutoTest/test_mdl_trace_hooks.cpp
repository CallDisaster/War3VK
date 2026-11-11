// CPU-only x86 ABI and synthetic-thunk test of the production adapters.
// Never loads/patches Game.dll or Storm.dll; no native allocation provenance claim.
#include "../src/d3d9/war3/memory/war3_mdl_trace_hooks.h"
#include "../src/minhook/include/MinHook.h"
#include <cstdio>
#include <cstring>

namespace m = dxvk::war3::memory::mdltrace;
static_assert(sizeof(void*) == 4, "This fixture must execute the x86 ABI");
static unsigned checks = 0, failures = 0;
static void check(bool value, const char* expr, unsigned line) {
  ++checks;
  if (!value) { ++failures; std::fprintf(stderr, "FAIL %u %s\n", line, expr); }
}
#define CHECK(e) check(bool(e), #e, __LINE__)
#if defined(_MSC_VER) && !defined(__clang__)
#define READ_SP(v) __asm mov v, esp
#else
#define READ_SP(v) __asm__ __volatile__("movl %%esp, %0" : "=r"(v))
#endif

constexpr uint32_t base = 0x10000000, table = base+0xBC6AC8;
constexpr uint32_t source = base+0xB7FF90, node = 0x32001000;
constexpr uint32_t descriptor = 0x33002000, oldHead = 0x34003000;
constexpr DWORD beforeError = 0x1357, afterError = 0x2468;
static void* ptr(uint32_t v) { return reinterpret_cast<void*>(uintptr_t(v)); }
static const char* name(uint32_t v) { return reinterpret_cast<const char*>(uintptr_t(v)); }
enum Call { Alloc, Free, Realloc, Bucket, Insert, Unlink, CallCount };
struct Args {
  uint32_t cx=0x11223344, dx=0x55667788, pointer=node, name=source;
  uint32_t size=22404, line=0xFFFFFFFEu, flags=8, hash=0xA9363855;
  uint32_t extra=0, desc=descriptor, result=node;
};
static Args args[CallCount];
static unsigned calls[CallCount], readCalls, readsAfterFree;
static bool freed, partialFail, nestedBucket;
static uint32_t nodeData[5], tableData[3], headData[2], bucketData[2];
static m::Journal journal;
static uint32_t count() { return journal.header.words[5].load(); }
static uint32_t word(unsigned i, unsigned j) { return journal.events[i].words[j].load(); }
static m::Journal& resetJournal() {
  for (auto& w : journal.header.words) w.store(0);
  for (auto& e : journal.events) for (auto& w : e.words) w.store(0);
  return journal;
}
static uint32_t threadId() { SetLastError(0xDEAD); return 77; }
static bool readBlock(uint32_t at, void* out, uint32_t bytes) {
  ++readCalls;
  if (freed) ++readsAfterFree;
  SetLastError(0xBEEF);
  if (partialFail) { std::memset(out, 0xCC, bytes); return false; }
  const void* data = nullptr;
  if (at == node && bytes == 20) data = nodeData;
  if (at == table+4 && bytes == 12) data = tableData;
  if (at == oldHead && bytes == 8) data = headData;
  if (at == descriptor+4 && bytes == 8) data = bucketData;
  if (!data) { std::memset(out, 0xDD, bytes); return false; }
  std::memcpy(out, data, bytes);
  return true;
}
static void begin(Call c) {
  const DWORD error = GetLastError();
  ++calls[c];
  CHECK(error == beforeError);
}
static void* __fastcall fakeAlloc(int cx, int dx, size_t size,
    const char* n, DWORD line, DWORD flags) {
  begin(Alloc); const auto& a=args[Alloc];
  CHECK(uint32_t(cx)==a.cx && uint32_t(dx)==a.dx);
  CHECK(size==a.size && uintptr_t(n)==a.name && line==a.line && flags==a.flags);
  SetLastError(afterError); return ptr(a.result);
}
static void* __fastcall alternateAlloc(int cx, int dx, size_t size,
    const char* n, DWORD line, DWORD flags) {
  void* result=fakeAlloc(cx,dx,size,n,line,flags);
  CHECK(result==ptr(args[Alloc].result));
  return result;
}
static int __stdcall fakeFree(void* p, const char* n, int line, DWORD flags) {
  begin(Free); const auto& a=args[Free];
  CHECK(uintptr_t(p)==a.pointer && uintptr_t(n)==a.name);
  CHECK(uint32_t(line)==a.line && flags==a.flags);
  freed=true;
  SetLastError(afterError); return int(a.result);
}
static void* __fastcall fakeRealloc(int cx, int dx, void* p, size_t size,
    const char* n, int line, DWORD flags) {
  begin(Realloc); const auto& a=args[Realloc];
  CHECK(uint32_t(cx)==a.cx && uint32_t(dx)==a.dx && uintptr_t(p)==a.pointer);
  CHECK(size==a.size && uintptr_t(n)==a.name && uint32_t(line)==a.line && flags==a.flags);
  SetLastError(afterError); return ptr(a.result);
}
static void* __fastcall fakeBucket(void* self, void* dx, void* desc,
    uint32_t extra, uint32_t flags) {
  begin(Bucket); const auto& a=args[Bucket];
  CHECK(uintptr_t(self)==a.cx && uintptr_t(dx)==a.dx && uintptr_t(desc)==a.desc);
  CHECK(extra==a.extra && flags==a.flags);
  nodeData[0]=0; nodeData[1]=descriptor+4; nodeData[2]=0x31000000;
  nodeData[3]=nodeData[4]=0; // bucket linked; global chain not yet linked.
  SetLastError(afterError); return ptr(a.result);
}
static void* __fastcall fakeInsert(void* self, void* dx, uint32_t hash,
    uint32_t extra, uint32_t flags) {
  begin(Insert); const auto& a=args[Insert];
  CHECK(uintptr_t(self)==a.cx && uintptr_t(dx)==a.dx);
  CHECK(hash==a.hash && extra==a.extra && flags==a.flags);
  if (nestedBucket) {
    const auto& b=args[Bucket];
    m::Hooks::BucketFn volatile call=&m::Hooks::bucket;
    SetLastError(beforeError);
    CHECK(call(ptr(b.cx),ptr(b.dx),ptr(b.desc),b.extra,b.flags)==ptr(b.result));
    CHECK(GetLastError()==afterError);
  }
  nodeData[3]=oldHead; nodeData[4]=0x35004000;
  headData[1]=node; tableData[1]=node+12;
  SetLastError(afterError); return ptr(a.result);
}
static uint32_t __fastcall fakeUnlink(void* self, void* dx, void* p) {
  begin(Unlink); const auto& a=args[Unlink];
  CHECK(uintptr_t(self)==a.cx && uintptr_t(dx)==a.dx && uintptr_t(p)==a.pointer);
  for (unsigned i=1;i<5;++i) nodeData[i]=0;
  SetLastError(afterError); return a.result;
}
struct Fixture {
  m::Core core;
  m::HookState state;
  Fixture() : core(resetJournal()) {
    for (unsigned c=0;c<CallCount;++c) { args[c]=Args{}; calls[c]=0; }
    args[Bucket].cx=args[Insert].cx=args[Unlink].cx=table;
    args[Free].result=1;
    args[Unlink].result=0xCAFEA123;
    readCalls=readsAfterFree=0; freed=partialFail=nestedBucket=false;
    std::memset(nodeData,0,sizeof nodeData);
    tableData[0]=12; tableData[1]=oldHead; tableData[2]=0x35004000;
    headData[0]=0x34005000; headData[1]=0x35004000;
    bucketData[0]=node+4; bucketData[1]=node;
    state.core=&core; state.gameBase=base; state.gameSize=0xCD8000;
    state.read=readBlock; state.thread=threadId; m::Hooks::state=&state;
    m::Hooks::originalAlloc=fakeAlloc; m::Hooks::originalFree=fakeFree;
    m::Hooks::originalRealloc=fakeRealloc; m::Hooks::originalBucket=fakeBucket;
    m::Hooks::originalInsert=fakeInsert; m::Hooks::originalUnlink=fakeUnlink;
  }
  ~Fixture() { m::Hooks::state=nullptr; }
};
static void callAlloc(m::Hooks::AllocFn fn=&m::Hooks::alloc) {
  m::Hooks::AllocFn volatile call=fn; const auto& a=args[Alloc];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  void* result=call(int(a.cx),int(a.dx),a.size,name(a.name),a.line,a.flags);
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && result==ptr(a.result) && error==afterError);
}
static void callFree(m::Hooks::FreeFn fn=&m::Hooks::free) {
  m::Hooks::FreeFn volatile call=fn; const auto& a=args[Free];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  int result=call(ptr(a.pointer),name(a.name),int(a.line),a.flags);
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && uint32_t(result)==a.result && error==afterError);
}
static void callRealloc(m::Hooks::ReallocFn fn=&m::Hooks::realloc) {
  m::Hooks::ReallocFn volatile call=fn; const auto& a=args[Realloc];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  void* result=call(int(a.cx),int(a.dx),ptr(a.pointer),a.size,name(a.name),int(a.line),a.flags);
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && result==ptr(a.result) && error==afterError);
}
static void callInsert() {
  m::Hooks::InsertFn volatile call=&m::Hooks::insert; const auto& a=args[Insert];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  void* result=call(ptr(a.cx),ptr(a.dx),a.hash,a.extra,a.flags);
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && result==ptr(a.result) && error==afterError);
}
static void callBucket() {
  m::Hooks::BucketFn volatile call=&m::Hooks::bucket; const auto& a=args[Bucket];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  void* result=call(ptr(a.cx),ptr(a.dx),ptr(a.desc),a.extra,a.flags);
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && result==ptr(a.result) && error==afterError);
}
static void callUnlink() {
  m::Hooks::UnlinkFn volatile call=&m::Hooks::unlink; const auto& a=args[Unlink];
  uint32_t spBefore,spAfter; SetLastError(beforeError); READ_SP(spBefore);
  uint32_t result=call(ptr(a.cx),ptr(a.dx),ptr(a.pointer));
  READ_SP(spAfter); const DWORD error=GetLastError();
  CHECK(spBefore==spAfter && result==a.result && error==afterError);
}
static void testAllocatorArguments() {
  Fixture f;
  callAlloc(); CHECK(count()==1 && readCalls==0 && calls[Alloc]==1);
  CHECK(word(0,1)==1 && word(0,4)==node && word(0,6)!=0);
  CHECK(word(0,8)==22404 && word(0,9)==8 && word(0,10)==0xB7FF90 && word(0,11)==0xFFFFFFFE);
  const uint32_t serial=word(0,6);
  args[Alloc].name=source+1; callAlloc();
  CHECK(count()==1 && readCalls==0 && calls[Alloc]==2);
  args[Free].name=source+1; callFree(); // known pointer, foreign source must retain lifetime event.
  CHECK(count()==3 && calls[Free]==1 && readCalls==0 && readsAfterFree==0);
  CHECK(word(1,1)==2 && word(2,1)==3 && word(1,6)==serial && word(2,6)==serial);
  CHECK(word(1,10)==0 && word(2,12)==1);
}
static void testReallocAndDisabled() {
  Fixture f; callAlloc();
  args[Realloc].name=source+1; args[Realloc].result=node+0x10000;
  callRealloc(); CHECK(count()==3 && readCalls==0 && calls[Realloc]==1);
  CHECK(word(1,1)==4 && word(2,1)==5 && word(2,5)==args[Realloc].result);
  CHECK((word(2,13)&16)!=0 && word(2,24)!=0);
  args[Realloc].pointer=args[Realloc].result; args[Realloc].result=0;
  callRealloc(); CHECK(count()==5 && (word(4,13)&16)==0);
  CHECK(readCalls==0 && calls[Realloc]==2);
  m::Hooks::state=nullptr;
  callAlloc(); callRealloc(); callFree(); callBucket(); callInsert(); callUnlink();
  CHECK(count()==5 && readCalls==0);
  CHECK(calls[Alloc]==2 && calls[Realloc]==3 && calls[Free]==1);
}
static void testOrderedBoundaryAndLifetime() {
  Fixture f; callAlloc(); nestedBucket=true;
  callInsert(); CHECK(count()==4 && calls[Insert]==1 && calls[Bucket]==1);
  CHECK(word(1,1)==7 && word(2,1)==6 && word(3,1)==8);
  CHECK(word(1,20)==oldHead && word(1,22)==oldHead);
  CHECK(word(2,15)==descriptor+4 && word(2,17)==0 && word(2,18)==0);
  CHECK(word(3,17)==oldHead && word(3,18)==0x35004000 && word(3,20)==node+12);
  CHECK(word(3,14)==0); // caller writes hash/name only after insert returns.
  CHECK((word(2,13)&7)==7 && (word(3,13)&7)==7);
  CHECK(word(2,6)==word(0,6) && word(3,6)==word(0,6));
  callUnlink(); CHECK(count()==6 && calls[Unlink]==1);
  CHECK(word(4,1)==9 && word(5,1)==10 && word(5,12)==args[Unlink].result);
  for (unsigned k=15;k<=18;++k) CHECK(word(5,k)==0);
  CHECK(word(4,17)==oldHead && !freed && readsAfterFree==0);
  const unsigned beforeReads=readCalls; callFree();
  CHECK(freed && count()==8 && beforeReads==readCalls && readsAfterFree==0);
}
static void testForeignTablesAndPartialReads() {
  Fixture f;
  args[Bucket].cx=args[Insert].cx=args[Unlink].cx=table+4;
  callBucket(); callInsert(); callUnlink();
  CHECK(count()==0 && readCalls==0);
  args[Bucket].cx=args[Insert].cx=args[Unlink].cx=table;
  partialFail=true; callBucket(); callInsert(); callUnlink();
  CHECK(count()==5 && readCalls>0);
  for (unsigned i=0;i<count();++i) {
    CHECK((word(i,13)&7)==0 && (word(i,13)&8)!=0);
    for (unsigned k=14;k<=24;++k) CHECK(word(i,k)==0);
  }
  // Failed read wrote CC to the callback buffer; no partial block was published.
  CHECK(readsAfterFree==0);
  m::Event e; f.state.fields(e,node,UINT32_MAX-8,oldHead);
  CHECK((e.words[13]&7)==0 && (e.words[13]&8)!=0);
  for (unsigned k=14;k<=24;++k) CHECK(e.words[k]==0);
}
static void testNullReturnAndUnknownFree() {
  Fixture f;
  args[Alloc].result=0; callAlloc();
  CHECK(count()==1 && word(0,4)==0 && word(0,6)==0 && readCalls==0);
  args[Free].name=source+1; callFree();
  CHECK(count()==1 && readsAfterFree==0); // unknown and unselected: no fabricated lifetime.
}
static void testInPlaceZeroAndRefusal() {
  {
    Fixture f; callAlloc();
    callRealloc(); // in-place success, not a move/release fact.
    CHECK(count()==3 && word(2,5)==node && (word(2,13)&16)==0);
    CHECK(word(2,24)!=0 && word(2,24)!=word(0,6));
    args[Realloc].size=0; args[Realloc].result=0; callRealloc();
    CHECK(count()==5 && (word(4,13)&16)==0 && word(4,24)==0 && readCalls==0);
    // Exact Storm #405: zero-size native small shrink returns a live pointer.
    args[Realloc].result=node; callRealloc();
    CHECK(count()==7 && (word(6,13)&16)==0 && word(6,24)!=0);
    CHECK(word(6,24)!=word(2,24));
    // External non-NOMOVE resize instead allocates zero bytes, then frees old.
    args[Realloc].result=node+0x10000; callRealloc();
    CHECK(count()==9 && (word(8,13)&16)!=0 && word(8,24)!=0);
  }
  {
    Fixture f; m::Event e; e.words[1]=7;
    for (unsigned i=0;i<m::kEventCapacity;++i) f.core.noteBoundary(e,true);
    CHECK(count()==m::kEventCapacity);
    callAlloc(); callRealloc(); callBucket(); callInsert(); callUnlink(); callFree();
    CHECK(count()==m::kEventCapacity && readsAfterFree==0);
    for (unsigned i=0;i<CallCount;++i) CHECK(calls[i]==1);
    CHECK(journal.header.words[uint32_t(m::HeaderField::JournalFull)].load()>0);
  }
}
static void writeThunk(unsigned char* target, uintptr_t* slot, uintptr_t function) {
  *slot=function; target[0]=0xFF; target[1]=0x25;
  const uint32_t at=uint32_t(uintptr_t(slot));
  std::memcpy(target+2,&at,sizeof at);
}
static void testAdjacentImportThunks() {
  Fixture f; m::Hooks::state=nullptr;
  auto* page=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
  CHECK(page!=nullptr); if (!page) return;
  std::memset(page,0xCC,4096);
  auto* slots=reinterpret_cast<uintptr_t*>(page+128);
  writeThunk(page,slots,reinterpret_cast<uintptr_t>(&fakeAlloc));
  writeThunk(page+6,slots+1,reinterpret_cast<uintptr_t>(&fakeRealloc));
  unsigned char original[12]; std::memcpy(original,page,sizeof original);
  CHECK(FlushInstructionCache(GetCurrentProcess(),page,4096)!=FALSE);
  auto alloc=reinterpret_cast<m::Hooks::AllocFn>(page);
  auto realloc=reinterpret_cast<m::Hooks::ReallocFn>(page+6);
  callAlloc(alloc); callRealloc(realloc);
  bool initialized=false, madeA=false, madeR=false, enabledA=false, enabledR=false;
  const MH_STATUS init=MH_Initialize(); CHECK(init==MH_OK);
  initialized=init==MH_OK;
  if (initialized) {
    void* originalA=nullptr; void* originalR=nullptr;
    MH_STATUS st=MH_CreateHook(page,reinterpret_cast<void*>(&m::Hooks::alloc),&originalA);
    CHECK(st==MH_OK); madeA=st==MH_OK;
    if (madeA) m::Hooks::originalAlloc=reinterpret_cast<m::Hooks::AllocFn>(originalA);
    st=MH_CreateHook(page+6,reinterpret_cast<void*>(&m::Hooks::realloc),&originalR);
    CHECK(st==MH_OK); madeR=st==MH_OK;
    if (madeR) m::Hooks::originalRealloc=reinterpret_cast<m::Hooks::ReallocFn>(originalR);
    if (madeA && madeR) {
      st=MH_EnableHook(page); CHECK(st==MH_OK); enabledA=st==MH_OK;
      CHECK(std::memcmp(page+6,original+6,6)==0); // first patch cannot touch adjacent thunk.
      CHECK(page[5]==original[5]);
      st=MH_EnableHook(page+6); CHECK(st==MH_OK); enabledR=st==MH_OK;
      CHECK(page[5]==original[5] && page[11]==original[11]);
      if (enabledA && enabledR) {
        for (unsigned i=0;i<32;++i) { callAlloc(alloc); callRealloc(realloc); }
        slots[0]=reinterpret_cast<uintptr_t>(&alternateAlloc);
        CHECK(FlushInstructionCache(GetCurrentProcess(),page,4096)!=FALSE);
        callAlloc(alloc); // copied FF25 keeps following the same IAT slot.
        CHECK(calls[Alloc]==34 && calls[Realloc]==33 && count()==0);
      }
    }
  }
  // Dedicated fixture process owns only these synthetic targets: never ALL_HOOKS.
  if (enabledR) CHECK(MH_DisableHook(page+6)==MH_OK);
  if (enabledA) CHECK(MH_DisableHook(page)==MH_OK);
  if (madeR) CHECK(MH_RemoveHook(page+6)==MH_OK);
  if (madeA) CHECK(MH_RemoveHook(page)==MH_OK);
  if (initialized) CHECK(MH_Uninitialize()==MH_OK);
  CHECK(std::memcmp(page,original,sizeof original)==0);
  m::Hooks::originalAlloc=fakeAlloc; m::Hooks::originalRealloc=fakeRealloc;
  callAlloc(alloc); callRealloc(realloc);
  CHECK(VirtualFree(page,0,MEM_RELEASE)!=FALSE);
}
int main() {
  testAllocatorArguments(); testReallocAndDisabled();
  testOrderedBoundaryAndLifetime(); testForeignTablesAndPartialReads();
  testNullReturnAndUnknownFree(); testInPlaceZeroAndRefusal(); testAdjacentImportThunks();
  std::printf("MDL trace hook ABI checks=%u failures=%u syntheticThunks=2 gameExecution=0\n",checks,failures);
  return failures ? 1 : 0;
}
