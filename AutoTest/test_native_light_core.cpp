#include "../src/d3d9/war3/model/war3_native_light_core.h"
#include <cassert>
#include <vector>
#include <fstream>
#include <iostream>
using namespace dxvk::war3::native_light;
int main(int argc, char** argv) {
  NativeValue v;
  assert(!Usable(v));
  v.enabled = v.positional = 1; v.directIntensity = 1; v.directColor = 0xffffff;
  assert(Usable(v));
  v.selectionScore = std::numeric_limits<float>::quiet_NaN(); assert(Usable(v));
  v.position[1] = std::numeric_limits<float>::infinity(); assert(!Usable(v));
  v.position[1] = 0; v.directIntensity = -1; assert(!Usable(v));
  v.directIntensity = 1; v.enabled = 0; assert(!Usable(v));
  Generation g;
  assert(g.next() == 1); g.value = UINT64_MAX-1;
  assert(g.next() == UINT64_MAX); assert(g.next() == 0); assert(g.next() == 0);
  Source source;
  assert(!DecodeReviewedSource(nullptr, 0, "", "", source));
  const auto& p = dxvk::war3::render::kNativeModelLightShadowCandidates;
  assert(p.size() == 11);
  assert(dxvk::war3::render::NativeLightSamePath("FOO/bar", "foo\\bar"));
  assert(!dxvk::war3::render::NativeLightSamePath("foo/bar/x", "foo/bar"));
  // Optional extracted original resources: independently decode all supplied
  // files against every frozen variant. This does not deploy or launch a game.
  for (int i=1; i<argc; ++i) {
    std::ifstream in(argv[i], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    assert(!bytes.empty());
    bool found = false;
    for (const auto& entry : p) {
      if (DecodeReviewedSource(bytes.data(), bytes.size(), entry.path, entry.sha256, source)) {
        found = true;
        assert(source.objectId == entry.lightObjectId && source.ordinal < source.count);
        assert(!DecodeReviewedSource(bytes.data(), bytes.size()-1, entry.path, entry.sha256, source));
        assert(!DecodeReviewedSource(bytes.data(), bytes.size(), "other.mdx", entry.sha256, source));
        assert(!DecodeReviewedSource(bytes.data(), bytes.size(), entry.path, std::string(64, '0'), source));
      }
    }
    assert(found);
  }
  std::cout << "native light core: passed; resources=" << argc-1 << '\n';
}
