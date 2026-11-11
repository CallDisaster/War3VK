// Exact production Win32 SRW condition-variable implementation. This is a CPU
// wait contract, not proof that the GPU/driver can be interrupted or cancelled.
#include "../src/util/thread.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
int main() {
  unsigned checks=0;
  auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("deadline condition check "+std::to_string(checks));};
  try {
    dxvk::mutex mutex;dxvk::condition_variable cv;
    {
      std::unique_lock<dxvk::mutex> lock(mutex);
      const auto start=Clock::now();
      check(cv.wait_until(lock,start-1ms)==std::cv_status::timeout);
      check(!cv.wait_until(lock,start-1ms,[]{return false;}));
      check(cv.wait_until(lock,start-1ms,[]{return true;}));
      check(cv.wait_for(lock,-1ms)==std::cv_status::timeout);
      check(Clock::now()-start<1s);
      const auto future=Clock::now();
      check(!cv.wait_until(lock,future+40ms,[]{return false;}));
      check(Clock::now()-future>=35ms&&Clock::now()-future<2s);
    }
    // Wakeups do not authorize progress and do not extend the original deadline.
    for(bool publish:{false,true}) {
      bool ready=false;std::atomic<bool> started{false},stop{false};
      std::thread notifier([&]{
        while(!started.load(std::memory_order_acquire))std::this_thread::yield();
        const auto start=Clock::now();
        while(!stop.load(std::memory_order_acquire)) {
          {std::lock_guard<dxvk::mutex> lock(mutex);if(publish&&Clock::now()-start>=40ms)ready=true;}
          cv.notify_all();std::this_thread::sleep_for(1ms);
        }
      });
      const auto start=Clock::now();bool result;
      {
        std::unique_lock<dxvk::mutex> lock(mutex);started.store(true,std::memory_order_release);
        result=cv.wait_for(lock,80ms,[&]{return ready;});
      }
      const auto elapsed=Clock::now()-start;stop=true;notifier.join();
      check(result==publish);check(elapsed>=(publish?35ms:75ms));check(elapsed<2s);
    }
    // Long positive durations must not wrap to an immediate timeout. An
    // independent notification releases this wait; no long sleep is performed.
    {
      bool ready=false;std::unique_lock<dxvk::mutex> lock(mutex);
      std::thread notifier([&]{std::this_thread::sleep_for(5ms);{std::lock_guard<dxvk::mutex> guard(mutex);ready=true;}cv.notify_all();});
      const bool result=cv.wait_for(lock,std::chrono::milliseconds(1ull<<32),[&]{return ready;});
      lock.unlock();notifier.join();check(result);
    }
    // Notification immediately before waiting is not lost: the protected
    // predicate is the source of truth, independently of notification history.
    for(unsigned i=0;i<1000;++i) {
      std::unique_lock<dxvk::mutex> lock(mutex);cv.notify_all();
      check(cv.wait_until(lock,Clock::now()+1ms,[]{return true;}));
    }
    std::printf("CONDITION_DEADLINE checks=%u failures=0 bits=%zu\n",checks,sizeof(void*)*8);return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
