#include "../src/d3d9/war3/render/war3_stage11_snapshot_page_policy.h"
#include <memory>
#include <vector>
#include <iostream>
#include <stdexcept>
using namespace dxvk::war3::render;
unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x))throw std::runtime_error(#x);} while(false)
struct Page { uint64_t id=0; static inline int alive=0; Page(){++alive;} ~Page(){--alive;} };
struct Fault { static inline bool allocate=false; };
template<class T> struct Alloc : std::allocator<T> {
  using value_type=T;
  template<class U> struct rebind {using other=Alloc<U>;};
  T* allocate(size_t n) {if(Fault::allocate)throw std::bad_alloc();return std::allocator<T>{}.allocate(n);}
};
int main(){try{
  using R=War3Stage11PagePublication;
  using Pages=std::vector<std::shared_ptr<Page>,Alloc<std::shared_ptr<Page>>>;
  Pages pages;uint64_t next=1,resident=0;bool called=false;
  constexpr auto bytes=kWar3Stage11SnapshotPageBytes;
  auto factory=[&]{called=true;return std::make_shared<Page>();};
  auto run=[&]{return War3PublishStage11SnapshotPage(pages,next,resident,bytes,factory);};
  auto empty=[&]{CHECK(pages.empty());CHECK(next==1);CHECK(resident==0);CHECK(Page::alive==0);};
  CHECK(War3PublishStage11SnapshotPage(pages,next,resident,bytes,
      []()->std::shared_ptr<Page>{throw std::bad_alloc();})==R::HostAllocationFailure);empty();
  std::weak_ptr<Page> failed;
  Fault::allocate=true;
  CHECK(War3PublishStage11SnapshotPage(pages,next,resident,bytes,[&]{auto p=factory();failed=p;return p;})==R::HostAllocationFailure);
  CHECK(called);CHECK(failed.expired());empty();Fault::allocate=false;
  CHECK(War3PublishStage11SnapshotPage(pages,next,resident,bytes,
      []{return std::shared_ptr<Page>{};})==R::NullPage);empty();
  bool propagated=false;
  try{War3PublishStage11SnapshotPage(pages,next,resident,bytes,
      []()->std::shared_ptr<Page>{throw std::logic_error("not an allocation failure");});}
  catch(const std::logic_error&){propagated=true;}
  CHECK(propagated);empty();
  CHECK(run()==R::Success);CHECK(next==2);CHECK(resident==bytes);
  CHECK(pages.size()==1);CHECK(pages[0]->id==1);CHECK(Page::alive==1);
  // Fill all reserved vector slots, then fail its next growth with live pages.
  while(pages.size()<pages.capacity())CHECK(run()==R::Success);
  const auto size=pages.size();const auto savedNext=next,savedResident=resident;auto first=pages[0];
  Fault::allocate=true;CHECK(run()==R::HostAllocationFailure);Fault::allocate=false;
  CHECK(next==savedNext);CHECK(resident==savedResident);CHECK(pages.size()==size);
  CHECK(pages[0]==first);CHECK(Page::alive==int(size));
  CHECK(run()==R::Success);CHECK(pages.back()->id==savedNext); // recovery doesn't consume failed id
  while(resident<kWar3Stage11SnapshotResidentCapBytes)CHECK(run()==R::Success);
  const auto capSize=pages.size();const auto capNext=next;called=false;
  CHECK(run()==R::InvalidState);CHECK(!called);CHECK(next==capNext);CHECK(pages.size()==capSize);
  resident=0;next=UINT64_MAX;called=false;CHECK(run()==R::InvalidState);CHECK(!called);CHECK(next==UINT64_MAX);
  next=0;CHECK(run()==R::InvalidState);CHECK(!called);CHECK(next==0);
  first.reset();pages.clear();CHECK(Page::alive==0);
  // Existing slice policy remains bounded/append-only, including exact limits.
  auto p=War3PlanStage11SnapshotSuballocation(0,bytes,1);CHECK(p.valid&&p.offset==0&&p.nextUsed==256);
  p=War3PlanStage11SnapshotSuballocation(bytes-256,bytes,256);CHECK(p.valid&&p.nextUsed==bytes);
  CHECK(!War3PlanStage11SnapshotSuballocation(bytes,bytes,1).valid);
  CHECK(!War3PlanStage11SnapshotSuballocation(0,bytes,UINT64_MAX).valid);
  std::cout<<"Stage11 production publication fault/recovery checks="<<checks<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
