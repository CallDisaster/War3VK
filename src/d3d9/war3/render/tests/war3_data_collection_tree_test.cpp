#include "../../tools/war3_data_collection_tree_core.h"
#include "../../tools/war3_data_collection_tree.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <iostream>
using namespace dxvk::war3::collection;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); std::exit(1); } } while(0)
int main() {
  Tree tree;
  auto a=tree.enter(Tag::SceneCollect,100);
  auto b=tree.enter(Tag::InstanceQuery,110);
  CHECK(tree.leave(b,120));
  auto c=tree.enter(Tag::GeosetCapture,125);
  auto d=tree.enter(Tag::GeosetQuery,127);
  CHECK(tree.leave(d,132)); CHECK(tree.leave(c,145)); CHECK(tree.leave(a,160));
  auto snap=tree.snapshot();
  CHECK(Tree::closed(snap)); CHECK(snap.nodes[0].ticks==60);
  CHECK(snap.nodes[1].childTicks==30 && snap.nodes[1].ticks==60);
  CHECK(snap.nodes[3].ticks==20 && snap.nodes[3].childTicks==5);
  a=tree.enter(Tag::SceneCollect,200); CHECK(tree.leave(a,220));
  CHECK(tree.snapshot().nodes[0].ticks==80 && tree.snapshot().nodes[1].calls==2);
  CHECK(Tree::closed(tree.snapshot()));
  auto broken=tree.snapshot(); ++broken.nodes[0].ticks; CHECK(!Tree::closed(broken));
  broken=tree.snapshot(); broken.nodes[2].parent=500; CHECK(!Tree::closed(broken));
  tree.reset(); a=tree.enter(Tag::ModelBind,10); b=tree.enter(Tag::ModelBind,12);
  CHECK(tree.leave(b,14)); CHECK(tree.leave(a,20)); CHECK(Tree::closed(tree.snapshot()));
  CHECK(tree.snapshot().count==3);
  tree.reset(); a=tree.enter(Tag::ModelBind,10); b=tree.enter(Tag::ModelQuery,11);
  CHECK(!tree.leave(a,12)); CHECK(!Tree::closed(tree.snapshot()));
  tree.reset(); a=tree.enter(Tag::ModelBind,10); CHECK(!tree.leave(a,9));
  tree.reset(); CHECK(!tree.leave(1,20));
  tree.reset(); for (int i=0;i<65;++i) tree.enter(Tag::PoseWrite,i);
  CHECK(tree.snapshot().faulted);
  tree.reset(); a=tree.enter(Tag::ModelBind,0); CHECK(tree.leave(a,UINT64_MAX));
  a=tree.enter(Tag::ModelBind,0); CHECK(!tree.leave(a,1));
  tree.reset();
  for (unsigned i=1;i<unsigned(Tag::Count);++i) {
    for (unsigned j=1;j<unsigned(Tag::Count);++j) {
      a=tree.enter(Tag(i),0); b=tree.enter(Tag(j),1);
      tree.leave(b,2); tree.leave(a,3);
    }
  }
  CHECK(tree.snapshot().faulted); CHECK(tree.snapshot().count==TreeSnapshot::Capacity);
#if defined(WARVK_DATA_COLLECTION_TREE_DEV) && WARVK_DATA_COLLECTION_TREE_DEV
  SetRecording(true); ResetSession(); NoteFrame();
  for(int i=0;i<128;++i) { Scope outer(Tag::SceneCollect); { Scope inner(Tag::InstanceQuery); } }
  { Pause paused; Scope excluded(Tag::MetadataCapture); }
  std::thread worker([] { for(int i=0;i<128;++i) { Scope outer(Tag::GeosetBind); Scope inner(Tag::GeosetQuery); } });
  worker.join();
  SetRecording(false);
  std::cout << CaptureJson(0) << '\n';
#endif
  std::fprintf(stderr,"PASS %u collection-tree checks\n",checks);
}
