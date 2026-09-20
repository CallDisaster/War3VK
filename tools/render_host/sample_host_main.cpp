#include "slot_host_runtime.h"
#include "sample_envelope.h"
#include "win32_transport.h"
#include <cstdio>
#include <cwchar>
#include <memory>

using namespace warvk::host;
namespace {
class Consumer final : public lab::SlotPayloadConsumer {
public:
  explicit Consumer(const wchar_t* mode) : m_slow(!std::wcscmp(mode, L"slow")),
    m_disconnect(!std::wcscmp(mode, L"disconnect")) { }
  uint64_t capabilities() const noexcept override {
    return slotwire::SharedCpuSlots | slotwire::EnvelopeSamples;
  }
  const char* begin(ipc::Nonce nonce, uint64_t map, uint64_t device) override {
    if (m_validator) return "RepeatedScope";
    m_validator = std::make_unique<envelope::Validator>(inbox::Scope{nonce, map, device});
    return nullptr;
  }
  const char* consume(const slots::ReadPermit& permit, const uint8_t* bytes, size_t count) override {
    if (!m_validator || !permit.active()) return "MissingPermit";
    envelope::View view;
    auto error = m_validator->accept(permit.key(), bytes, count, view);
    if (error != envelope::Error::None) return envelope::ErrorName(error);
    ++m_samples;
    std::printf("{\"envelope\":true,\"sourceFrame\":%llu,\"attempt\":%llu,\"transfer\":%llu,\"payloadBytes\":%u}\n",
      (unsigned long long)view.header.sourceFrame, (unsigned long long)view.header.attempt,
      (unsigned long long)view.header.transfer, view.bytes);
    if (m_slow && m_samples == 1) {
      LARGE_INTEGER frequency{}, start{}, end{};
      if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start)) return "Qpc";
      std::printf("{\"delayStart\":%llu,\"frequency\":%llu}\n",
        (unsigned long long)start.QuadPart, (unsigned long long)frequency.QuadPart);
      std::fflush(stdout);
      Sleep(1200); // Lab-only fault injection, never a production/render callback.
      if (!QueryPerformanceCounter(&end)) return "Qpc";
      std::printf("{\"delayEnd\":%llu,\"frequency\":%llu}\n",
        (unsigned long long)end.QuadPart, (unsigned long long)frequency.QuadPart);
    }
    std::fflush(stdout);
    return m_disconnect && m_samples == 3 ? "InjectedDisconnect" : nullptr;
  }
private:
  std::unique_ptr<envelope::Validator> m_validator;
  unsigned m_samples = 0;
  bool m_slow, m_disconnect;
};
}
int wmain(int argc, wchar_t** argv) {
  if (argc != 5 || (std::wcscmp(argv[4], L"normal") && std::wcscmp(argv[4], L"slow") &&
      std::wcscmp(argv[4], L"disconnect"))) return 3;
  Consumer consumer(argv[4]);
  return lab::RunSlotHost(4, argv, &consumer);
}
