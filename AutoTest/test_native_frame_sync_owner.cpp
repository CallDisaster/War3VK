#include "../src/d3d9/war3/hooks/war3_native_frame_sync_owner.h"
#include "../src/d3d9/war3/hooks/war3_native_frame_sync_policy.h"
#include <cassert>
#include <iostream>
using namespace dxvk::war3::hooks;
int main() {
  NativeFrameSyncOwner o;
  assert(!o.owns(7,7));
  o.complete(1,7); assert(!o.owns(7,7));
  o.begin(1,7); assert(!o.owns(7,7));
  o.complete(1,7); assert(o.owns(7,7));
  assert(!o.owns(7,8) && !o.owns(8,8) && !o.owns(0,0));
  o.revoke(false); assert(!o.owns(7,7));
  o.begin(1,7); o.complete(1,7); assert(o.owns(7,7));
  o.begin(1,7); assert(!o.owns(7,7)); // failed Present does not complete
  o.begin(1,7); o.complete(1,7); assert(o.owns(7,7));
  o.revoke(true); o.begin(1,7); o.complete(1,7); assert(!o.owns(7,7));
  NativeFrameSyncOwner secondChain;
  secondChain.begin(1,7); secondChain.complete(1,7);
  secondChain.begin(2,7); secondChain.complete(2,7);
  secondChain.begin(1,7); secondChain.complete(1,7); assert(!secondChain.owns(7,7));
  NativeFrameSyncOwner secondThread;
  secondThread.begin(1,7); secondThread.complete(1,7);
  secondThread.begin(1,8); secondThread.complete(1,8);
  secondThread.begin(1,7); secondThread.complete(1,7); assert(!secondThread.owns(7,7));
  // Every input combination uses the actual shared skip policy.
  for (int mask=0; mask<16; ++mask)
    for (int request : {-1,0,1,2}) for (unsigned capture : {0u,1u,0xffffffffu}) {
      const bool enabled=mask&1, owner=mask&2, readable=mask&4, nested=mask&8;
      assert(CanElideNativeFrameSync(enabled,owner,readable,nested,request,capture)==
          (enabled && owner && readable && !nested && request==1 && capture==0));
    }
  std::cout << "native owner lifecycle and 192 canonical policy combinations passed\n";
}
